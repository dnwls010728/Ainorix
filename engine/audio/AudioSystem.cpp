#include "audio/AudioSystem.h"

#include <algorithm>
#include <cmath>

#include "app/Engine.h"
#include "core/FileSystem.h"
#include "core/Log.h"
#include "platform/Platform.h"
#include "scene/Components.h"

namespace oe {

namespace {
constexpr size_t kMaxEvents = 200;
constexpr size_t kMaxVoices = 64;
constexpr double kMaxCaptureSeconds = 600.0;
}  // namespace

AudioSystem::AudioSystem(Engine& engine) : engine_(engine), mix_(static_cast<size_t>(kFramesPerStep) * 2, 0.0f) {}
AudioSystem::~AudioSystem() = default;

std::shared_ptr<const AudioClip> AudioSystem::Load(const std::string& path) {
    auto it = clips_.find(path);
    if (it != clips_.end()) return it->second;
    std::string full = engine_.ResolvePath(path);
    std::vector<unsigned char> bytes;
    if (!ReadBinaryFile(full, bytes)) {
        throw ApiError("not_found", "cannot read audio file '" + path + "'", "Create one with audio.generate {path, preset} or add a .wav file to the project.");
    }
    auto clip = std::make_shared<AudioClip>();
    std::string err;
    if (!DecodeWav(bytes, *clip, &err)) throw ApiError("invalid_audio", path + ": " + err);
    clips_[path] = clip;
    return clip;
}

int AudioSystem::Play(const std::string& path, float volume, float pitch, bool loop, EntityId owner) {
    Voice v;
    v.clip = Load(path);
    v.id = nextVoice_++;
    v.path = path;
    v.volume = Clamp(volume, 0.0f, 2.0f);
    v.pitch = Clamp(pitch, 0.1f, 4.0f);
    v.loop = loop;
    v.owner = owner;
    if (voices_.size() >= kMaxVoices) voices_.erase(voices_.begin());  // steal the oldest
    voices_.push_back(v);
    events_.push_back({engine_.Frame(), v.id, path, owner});
    if (events_.size() > kMaxEvents) events_.erase(events_.begin());
    return v.id;
}

bool AudioSystem::Stop(int voice) {
    auto it = std::find_if(voices_.begin(), voices_.end(), [&](const Voice& v) { return v.id == voice; });
    if (it == voices_.end()) return false;
    voices_.erase(it);
    return true;
}

void AudioSystem::StopAll() { voices_.clear(); }

void AudioSystem::Update(Scene& scene) {
    // Voices owned by entities stop when the entity or its AudioSource goes away.
    voices_.erase(std::remove_if(voices_.begin(), voices_.end(),
                                 [&](const Voice& v) { return v.owner != kNullEntity && !scene.Get<AudioSource>(v.owner); }),
                  voices_.end());
    for (auto it = started_.begin(); it != started_.end();) {
        it = scene.Get<AudioSource>(*it) ? std::next(it) : started_.erase(it);
    }
    for (auto& kv : scene.Pool<AudioSource>()) {
        const AudioSource& src = kv.second;
        if (!src.playOnStart || src.clip.empty() || started_.count(kv.first)) continue;
        started_.insert(kv.first);
        try {
            Play(src.clip, src.volume, src.pitch, src.loop, kv.first);
        } catch (const ApiError& e) {
            OE_LOG_WARN("audio", "entity %u: %s", kv.first, e.what());
        }
    }
    // Live edits of volume/pitch apply to the running voice.
    for (Voice& v : voices_) {
        if (v.owner == kNullEntity) continue;
        if (const AudioSource* src = scene.Get<AudioSource>(v.owner)) {
            v.volume = Clamp(src->volume, 0.0f, 2.0f);
            v.pitch = Clamp(src->pitch, 0.1f, 4.0f);
        }
    }
}

void AudioSystem::Render() {
    std::fill(mix_.begin(), mix_.end(), 0.0f);
    for (Voice& v : voices_) {
        const AudioClip& c = *v.clip;
        const double frames = static_cast<double>(c.Frames());
        if (frames < 1) {
            v.position = frames;
            continue;
        }
        for (int i = 0; i < kFramesPerStep; ++i) {
            if (v.position >= frames) {
                if (!v.loop) break;
                v.position = std::fmod(v.position, frames);
            }
            size_t a = static_cast<size_t>(v.position);
            size_t b = a + 1 < c.Frames() ? a + 1 : (v.loop ? 0 : a);
            float t = static_cast<float>(v.position - static_cast<double>(a));
            mix_[static_cast<size_t>(i) * 2] += Lerp(c.samples[a * 2], c.samples[b * 2], t) * v.volume;
            mix_[static_cast<size_t>(i) * 2 + 1] += Lerp(c.samples[a * 2 + 1], c.samples[b * 2 + 1], t) * v.volume;
            v.position += v.pitch;
        }
    }
    voices_.erase(std::remove_if(voices_.begin(), voices_.end(), [](const Voice& v) { return !v.loop && v.position >= static_cast<double>(v.clip->Frames()); }),
                  voices_.end());
    for (float& s : mix_) s = Clamp(s, -1.0f, 1.0f);
    if (silent_) return;
    if (deferred_) { pending_[engine_.Frame()] = mix_; return; }
    Output(mix_);
}

void AudioSystem::Output(const std::vector<float>& block) {
    if (capturing_ && static_cast<double>(capture_.size()) / 2.0 / kAudioSampleRate < kMaxCaptureSeconds) {
        capture_.insert(capture_.end(), block.begin(), block.end());
    }
    if (device_) device_->Submit(block.data(), kFramesPerStep);
}

void AudioSystem::Confirm(uint64_t frames) {
    while (!pending_.empty() && pending_.begin()->first < frames) { Output(pending_.begin()->second); pending_.erase(pending_.begin()); }
}
std::shared_ptr<const AudioSystem::Snapshot> AudioSystem::SaveState() const {
    return std::make_shared<Snapshot>(Snapshot{clips_, voices_, events_, started_, nextVoice_, mix_, capture_, capturing_});
}
void AudioSystem::LoadState(const Snapshot& state) {
    clips_ = state.clips; voices_ = state.voices; events_ = state.events; started_ = state.started;
    nextVoice_ = state.nextVoice; mix_ = state.mix; capture_ = state.capture; capturing_ = state.capturing;
    pending_.clear();
}

void AudioSystem::OnSceneChanged() {
    voices_.erase(std::remove_if(voices_.begin(), voices_.end(), [](const Voice& v) { return v.owner != kNullEntity; }), voices_.end());
    started_.clear();
}

void AudioSystem::Reset() {
    voices_.clear();
    started_.clear();
    events_.clear();
    clips_.clear();  // pick up edited files next session
}

void AudioSystem::AttachDevice(std::unique_ptr<AudioDevice> device) { device_ = std::move(device); }

void AudioSystem::StartCapture() {
    capture_.clear();
    capturing_ = true;
}

Json AudioSystem::StopCapture(const std::string& path) {
    capturing_ = false;
    Json out = Json::MakeObject();
    double sum = 0.0;
    float peak = 0.0f;
    size_t loudFrames = 0;
    for (size_t i = 0; i + 1 < capture_.size(); i += 2) {
        float m = std::max(std::fabs(capture_[i]), std::fabs(capture_[i + 1]));
        peak = std::max(peak, m);
        sum += static_cast<double>(capture_[i]) * capture_[i] + static_cast<double>(capture_[i + 1]) * capture_[i + 1];
        if (m > 0.01f) ++loudFrames;
    }
    size_t frames = capture_.size() / 2;
    out["seconds"] = static_cast<double>(frames) / kAudioSampleRate;
    out["peak"] = peak;
    out["rms"] = frames ? std::sqrt(sum / static_cast<double>(capture_.size())) : 0.0;
    out["audibleSeconds"] = static_cast<double>(loudFrames) / kAudioSampleRate;
    if (!path.empty()) {
        if (!WriteWav(path, capture_, 2, kAudioSampleRate)) throw ApiError("write_failed", "cannot write " + path);
        out["path"] = path;
    }
    capture_.clear();
    return out;
}

Json AudioSystem::State() const {
    Json out = Json::MakeObject();
    out["sampleRate"] = kAudioSampleRate;
    out["output"] = device_ ? device_->Name() : "none (headless: audio is still mixed and can be captured)";
    Json voices = Json::MakeArray();
    for (const Voice& v : voices_) {
        Json j = Json::MakeObject();
        j["id"] = v.id;
        j["path"] = v.path;
        j["seconds"] = v.position / kAudioSampleRate;
        j["duration"] = v.clip->Seconds();
        j["loop"] = v.loop;
        j["volume"] = v.volume;
        if (v.owner != kNullEntity) j["entity"] = v.owner;
        voices.push(j);
    }
    out["voices"] = voices;
    Json events = Json::MakeArray();
    for (const Event& e : events_) {
        Json j = Json::MakeObject();
        j["frame"] = static_cast<uint64_t>(e.frame);
        j["voice"] = e.voice;
        j["path"] = e.path;
        if (e.owner != kNullEntity) j["entity"] = e.owner;
        events.push(j);
    }
    out["played"] = events;
    out["capturing"] = capturing_;
    if (capturing_) out["capturedSeconds"] = static_cast<double>(capture_.size()) / 2.0 / kAudioSampleRate;
    return out;
}

}  // namespace oe
