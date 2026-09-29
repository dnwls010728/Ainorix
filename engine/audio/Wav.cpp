#include "audio/Wav.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/FileSystem.h"
#include "core/Math.h"

namespace oe {

namespace {

uint32_t U32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint16_t U16(const unsigned char* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

float SampleAt(const unsigned char* p, int bits, bool isFloat) {
    if (isFloat) {
        float f;
        std::memcpy(&f, p, 4);
        return f;
    }
    switch (bits) {
        case 8: return (static_cast<float>(p[0]) - 128.0f) / 128.0f;
        case 16: return static_cast<float>(static_cast<int16_t>(U16(p))) / 32768.0f;
        case 24: {
            int32_t v = (p[0] << 8) | (p[1] << 16) | (static_cast<int32_t>(p[2]) << 24);
            return static_cast<float>(v >> 8) / 8388608.0f;
        }
        case 32: return static_cast<float>(static_cast<int32_t>(U32(p))) / 2147483648.0f;
    }
    return 0.0f;
}

void Put16(std::vector<unsigned char>& b, uint16_t v) {
    b.push_back(static_cast<unsigned char>(v & 0xFF));
    b.push_back(static_cast<unsigned char>(v >> 8));
}
void Put32(std::vector<unsigned char>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<unsigned char>((v >> (8 * i)) & 0xFF));
}

}  // namespace

bool DecodeWav(const std::vector<unsigned char>& bytes, AudioClip& out, std::string* error) {
    auto fail = [&](const char* msg) {
        if (error) *error = msg;
        return false;
    };
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) return fail("not a RIFF/WAVE file");
    int format = 0, channels = 0, rate = 0, bits = 0;
    const unsigned char* data = nullptr;
    size_t dataSize = 0;
    size_t pos = 12;
    while (pos + 8 <= bytes.size()) {
        const unsigned char* chunk = bytes.data() + pos;
        uint32_t size = U32(chunk + 4);
        size_t body = pos + 8;
        if (body + size > bytes.size()) size = static_cast<uint32_t>(bytes.size() - body);
        if (std::memcmp(chunk, "fmt ", 4) == 0 && size >= 16) {
            format = U16(bytes.data() + body);
            channels = U16(bytes.data() + body + 2);
            rate = static_cast<int>(U32(bytes.data() + body + 4));
            bits = U16(bytes.data() + body + 14);
            if (format == 0xFFFE && size >= 26) format = U16(bytes.data() + body + 24);  // WAVE_FORMAT_EXTENSIBLE
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            data = bytes.data() + body;
            dataSize = size;
        }
        pos = body + size + (size & 1);
    }
    if (!data || channels <= 0 || rate <= 0) return fail("missing fmt or data chunk");
    bool isFloat = format == 3;
    if (!(format == 1 || (isFloat && bits == 32))) return fail("unsupported WAV encoding (use PCM 8/16/24/32-bit or float32)");
    if (bits != 8 && bits != 16 && bits != 24 && bits != 32) return fail("unsupported bit depth");
    size_t stride = static_cast<size_t>(bits / 8) * static_cast<size_t>(channels);
    size_t frames = dataSize / stride;

    std::vector<float> stereo(frames * 2);
    for (size_t f = 0; f < frames; ++f) {
        const unsigned char* p = data + f * stride;
        float l = SampleAt(p, bits, isFloat);
        float r = channels > 1 ? SampleAt(p + bits / 8, bits, isFloat) : l;
        stereo[f * 2] = l;
        stereo[f * 2 + 1] = r;
    }
    if (rate == kAudioSampleRate) {
        out.samples = std::move(stereo);
        return true;
    }
    // Linear resampling to the mixer rate.
    size_t outFrames = static_cast<size_t>(static_cast<double>(frames) * kAudioSampleRate / rate);
    out.samples.assign(outFrames * 2, 0.0f);
    for (size_t i = 0; i < outFrames; ++i) {
        double src = static_cast<double>(i) * rate / kAudioSampleRate;
        size_t a = static_cast<size_t>(src);
        size_t b = std::min(a + 1, frames - 1);
        float t = static_cast<float>(src - static_cast<double>(a));
        for (int c = 0; c < 2; ++c) out.samples[i * 2 + static_cast<size_t>(c)] = Lerp(stereo[a * 2 + static_cast<size_t>(c)], stereo[b * 2 + static_cast<size_t>(c)], t);
    }
    return true;
}

bool WriteWav(const std::string& path, const std::vector<float>& samples, int channels, int sampleRate) {
    std::vector<unsigned char> b;
    uint32_t dataBytes = static_cast<uint32_t>(samples.size() * 2);
    b.insert(b.end(), {'R', 'I', 'F', 'F'});
    Put32(b, 36 + dataBytes);
    b.insert(b.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    Put32(b, 16);
    Put16(b, 1);
    Put16(b, static_cast<uint16_t>(channels));
    Put32(b, static_cast<uint32_t>(sampleRate));
    Put32(b, static_cast<uint32_t>(sampleRate * channels * 2));
    Put16(b, static_cast<uint16_t>(channels * 2));
    Put16(b, 16);
    b.insert(b.end(), {'d', 'a', 't', 'a'});
    Put32(b, dataBytes);
    for (float s : samples) Put16(b, static_cast<uint16_t>(static_cast<int16_t>(std::lround(Clamp(s, -1.0f, 1.0f) * 32767.0f))));
    CreateDirectories(ParentPath(path));
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    size_t written = std::fwrite(b.data(), 1, b.size(), f);
    std::fclose(f);
    return written == b.size();
}

// ----- Sound generator ------------------------------------------------------------

namespace {

struct Rng {
    uint32_t s;
    float Next() {  // 0..1
        s = s * 1664525u + 1013904223u;
        return static_cast<float>(s >> 8) / 16777216.0f;
    }
};

// Appends a tone: waveform 0 square, 1 triangle, 2 sine, 3 noise.
void Tone(std::vector<float>& out, Rng& rng, int wave, float f0, float f1, float seconds, float volume, float decay) {
    const int n = static_cast<int>(seconds * kAudioSampleRate);
    double phase = 0.0;
    float noise = 0.0f;
    for (int i = 0; i < n; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(n);
        float freq = f0 + (f1 - f0) * t;
        phase += freq / kAudioSampleRate;
        float ph = static_cast<float>(phase - std::floor(phase));
        float s = 0.0f;
        switch (wave) {
            case 0: s = ph < 0.5f ? 1.0f : -1.0f; break;
            case 1: s = 4.0f * std::fabs(ph - 0.5f) - 1.0f; break;
            case 2: s = std::sin(2.0f * kPi * ph); break;
            default: {
                // Noise resampled at `freq` to control its brightness.
                if (ph < freq / kAudioSampleRate || i == 0) noise = rng.Next() * 2.0f - 1.0f;
                s = noise;
                break;
            }
        }
        float attack = std::min(1.0f, static_cast<float>(i) / (0.002f * kAudioSampleRate));
        float env = attack * std::pow(1.0f - t, decay);
        out.push_back(s * volume * env);
    }
}

}  // namespace

const std::vector<std::string>& SoundPresets() {
    static const std::vector<std::string> presets = {"coin", "jump", "hit", "explosion", "powerup", "blip", "success"};
    return presets;
}

bool GenerateSound(const std::string& preset, uint32_t seed, std::vector<float>& out) {
    Rng rng{seed * 2654435761u + 12345u};
    // seed 0 = canonical sound; other seeds vary the pitch slightly.
    float v = seed == 0 ? 1.0f : 0.92f + 0.16f * rng.Next();
    out.clear();
    if (preset == "coin") {
        Tone(out, rng, 0, 988 * v, 988 * v, 0.07f, 0.3f, 0.2f);
        Tone(out, rng, 0, 1319 * v, 1319 * v, 0.28f, 0.3f, 1.5f);
    } else if (preset == "jump") {
        Tone(out, rng, 0, 280 * v, 720 * v, 0.24f, 0.28f, 1.2f);
    } else if (preset == "hit") {
        Tone(out, rng, 3, 9000 * v, 2500 * v, 0.14f, 0.5f, 2.0f);
    } else if (preset == "explosion") {
        Tone(out, rng, 3, 2200 * v, 300 * v, 0.9f, 0.6f, 2.5f);
    } else if (preset == "powerup") {
        const float notes[] = {523.25f, 659.25f, 783.99f, 1046.5f};
        for (float n : notes) Tone(out, rng, 0, n * v, n * v, 0.07f, 0.25f, 0.3f);
        Tone(out, rng, 0, 1046.5f * v, 1046.5f * v, 0.2f, 0.25f, 1.5f);
    } else if (preset == "blip") {
        Tone(out, rng, 2, 880 * v, 880 * v, 0.07f, 0.4f, 1.0f);
    } else if (preset == "success") {
        const float notes[] = {523.25f, 659.25f, 783.99f, 1046.5f, 783.99f, 1046.5f};
        for (float n : notes) Tone(out, rng, 1, n * v, n * v, 0.11f, 0.4f, 0.4f);
        Tone(out, rng, 1, 1318.5f * v, 1318.5f * v, 0.5f, 0.4f, 1.5f);
    } else {
        return false;
    }
    return true;
}

}  // namespace oe
