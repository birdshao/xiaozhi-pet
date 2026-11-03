#pragma once

#include <cstddef>
#include <cstdint>

namespace kid_app {

// A validated 24 kHz mono 16-bit PCM WAV (the XiaoZhi AudioService output
// format). Points into the caller's bytes (e.g. an embedded flash file);
// samples are read byte-wise, so any alignment is fine.
struct WavClip {
    static constexpr uint32_t kSampleRate = 24000;
    static constexpr size_t kMaxSamples = 2 * kSampleRate;  // <= 2 s prompt

    const uint8_t* data = nullptr;  // first PCM byte
    size_t samples = 0;

    int16_t Sample(size_t i) const {
        return static_cast<int16_t>(data[2 * i] | (data[2 * i + 1] << 8));
    }
    // Copies up to max samples starting at offset; returns how many.
    size_t Copy(size_t offset, int16_t* out, size_t max) const;
};

// Strict RIFF/WAVE parser; on failure clip is left empty.
bool ParseWavClip(const uint8_t* bytes, size_t size, WavClip* clip);

}  // namespace kid_app
