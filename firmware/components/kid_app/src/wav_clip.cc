#include "kid_app/wav_clip.h"

#include <cstring>

namespace kid_app {

namespace {
uint32_t Le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint16_t Le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
}  // namespace

size_t WavClip::Copy(size_t offset, int16_t* out, size_t max) const {
    if (!data || !out || offset >= samples) return 0;
    const size_t n = samples - offset < max ? samples - offset : max;
    for (size_t i = 0; i < n; ++i) out[i] = Sample(offset + i);
    return n;
}

bool ParseWavClip(const uint8_t* bytes, size_t size, WavClip* clip) {
    if (!clip) return false;
    *clip = WavClip{};
    if (!bytes || size < 12 || std::memcmp(bytes, "RIFF", 4) != 0 ||
        std::memcmp(bytes + 8, "WAVE", 4) != 0)
        return false;

    bool format_ok = false;
    size_t pos = 12;
    while (pos + 8 <= size) {
        const uint8_t* id = bytes + pos;
        const uint32_t length = Le32(bytes + pos + 4);
        const size_t body = pos + 8;
        if (length > size - body) return false;  // chunk runs past the end
        if (std::memcmp(id, "fmt ", 4) == 0) {
            if (length < 16) return false;
            const uint8_t* f = bytes + body;
            format_ok = Le16(f) == 1 &&                    // integer PCM
                        Le16(f + 2) == 1 &&                // mono
                        Le32(f + 4) == WavClip::kSampleRate &&
                        Le16(f + 14) == 16;                // bits per sample
            if (!format_ok) return false;
        } else if (std::memcmp(id, "data", 4) == 0) {
            if (!format_ok || length == 0 || length % 2 != 0) return false;
            const size_t samples = length / 2;
            if (samples > WavClip::kMaxSamples) return false;
            clip->data = bytes + body;
            clip->samples = samples;
            return true;
        }
        pos = body + length + (length & 1);  // chunks are word aligned
    }
    return false;
}

}  // namespace kid_app
