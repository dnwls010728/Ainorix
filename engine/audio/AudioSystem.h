#pragma once
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "audio/Wav.h"
#include "core/Json.h"
#include "scene/Reflect.h"

namespace oe {

class Engine;
class Scene;
class AudioDevice;

// Deterministic software mixer. Every simulated frame renders exactly
// 48000/60 = 800 stereo frames, so audio is tied to simulation time: the same
// session always produces the same samples, which tools can capture to a WAV
// and measure. When a device is attached (oe run / oe editor) the frames are
// also sent to the speakers.
class AudioSystem {
public:
    static constexpr int kFramesPerStep = kAudioSampleRate / 60;

    explicit AudioSystem(Engine& engine);
    ~AudioSystem();

    // Starts a clip; returns a voice id. Throws ApiError if the file is bad.
    int Play(const std::string& path, float volume, float pitch, bool loop, EntityId owner);
    bool Stop(int voice);
    void StopAll();

    // Per simulated frame: AudioSource components, then mixing.
    void Update(Scene& scene);
    void Render();
    // Replay mixes silently; rollback submits each confirmed block once.
    void SetOutputMode(bool silent, bool deferred) { silent_ = silent; deferred_ = deferred; }
    void Confirm(uint64_t frames);
    void DiscardPending() { pending_.clear(); }
    void ResetTimeline() { nextVoice_ = 1; capture_.clear(); pending_.clear(); }
    struct Snapshot;
    struct OutputState { std::vector<float> capture; bool capturing; };
    OutputState TakeOutput() { return {std::move(capture_), capturing_}; }
    void RestoreOutput(OutputState state) { capture_ = std::move(state.capture); capturing_ = state.capturing; }
    std::shared_ptr<const Snapshot> SaveState() const;
    void LoadState(const Snapshot& state);
    // Called when a game switches scene: stops sounds owned by entities.
    void OnSceneChanged();
    // End of a play session: stops everything, forgets clips.
    void Reset();

    void AttachDevice(std::unique_ptr<AudioDevice> device);
    bool HasDevice() const { return device_ != nullptr; }

    void StartCapture();
    // Writes the captured mix to `path` (empty = don't write) and returns stats.
    Json StopCapture(const std::string& path);
    bool Capturing() const { return capturing_; }

    Json State() const;

private:
    struct Voice {
        int id = 0;
        std::string path;
        std::shared_ptr<const AudioClip> clip;
        double position = 0.0;  // in frames
        float volume = 1.0f;
        float pitch = 1.0f;
        bool loop = false;
        EntityId owner = kNullEntity;
    };
    struct Event {
        uint64_t frame;
        int voice;
        std::string path;
        EntityId owner;
    };
    std::shared_ptr<const AudioClip> Load(const std::string& path);

public:
    struct Snapshot {
        std::map<std::string, std::shared_ptr<const AudioClip>> clips;
        std::vector<Voice> voices; std::vector<Event> events; std::set<EntityId> started;
        int nextVoice; std::vector<float> mix, capture; bool capturing;
    };
private:
    void Output(const std::vector<float>& block);
    Engine& engine_;
    std::map<std::string, std::shared_ptr<const AudioClip>> clips_;
    std::vector<Voice> voices_;
    std::vector<Event> events_;
    std::set<EntityId> started_;  // AudioSources already triggered this session
    int nextVoice_ = 1;
    std::vector<float> mix_;
    std::unique_ptr<AudioDevice> device_;
    bool capturing_ = false;
    std::vector<float> capture_;
    bool silent_ = false, deferred_ = false;
    std::map<uint64_t, std::vector<float>> pending_;
};

}  // namespace oe
