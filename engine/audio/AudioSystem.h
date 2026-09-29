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
};

}  // namespace oe
