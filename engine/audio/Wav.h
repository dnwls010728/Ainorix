#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace oe {

constexpr int kAudioSampleRate = 48000;

// Decoded sound: interleaved stereo float samples at kAudioSampleRate.
struct AudioClip {
    std::vector<float> samples;
    size_t Frames() const { return samples.size() / 2; }
    double Seconds() const { return static_cast<double>(Frames()) / kAudioSampleRate; }
};

// Decodes RIFF/WAVE (PCM 8/16/24/32-bit or float32, mono/stereo, any rate).
bool DecodeWav(const std::vector<unsigned char>& bytes, AudioClip& out, std::string* error);

// Writes 16-bit PCM. `samples` are interleaved with `channels` channels.
bool WriteWav(const std::string& path, const std::vector<float>& samples, int channels, int sampleRate);

// Procedural sound effects for projects without audio assets.
// Presets: coin, jump, hit, explosion, powerup, blip, success. Mono.
const std::vector<std::string>& SoundPresets();
bool GenerateSound(const std::string& preset, uint32_t seed, std::vector<float>& monoOut);

}  // namespace oe
