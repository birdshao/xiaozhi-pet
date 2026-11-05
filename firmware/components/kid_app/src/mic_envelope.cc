#include "kid_app/mic_envelope.h"

#include <algorithm>
#include <cstring>

namespace kid_app {
namespace {
// Small deterministic jitter, 0..range-1, from the sample time.
uint32_t Jitter(uint32_t t_ms, uint32_t range) {
    uint32_t h = (t_ms / MicClassifier::kFrameMs + 1) * 2654435761u;
    h ^= h >> 15;
    return (h >> 8) % range;
}
}  // namespace

uint32_t MicClassifier::OnLevel() const { return std::max(floor_ * 3, floor_ + 600); }
uint32_t MicClassifier::OffLevel() const { return std::max(floor_ * 2, floor_ + 300); }

void MicClassifier::Reset() { *this = MicClassifier(); }

MicDecision MicClassifier::Feed(uint16_t rms) {
    MicDecision out;
    if (frames_seen_ < kWarmFrames) {  // learn the room level; decide nothing yet
        warm_sum_ += rms;
        if (++frames_seen_ == kWarmFrames) floor_ = warm_sum_ / kWarmFrames;
        return out;
    }
    if (!in_burst_) {
        if (rms < OnLevel()) {
            // Slow follower: the floor tracks the room, never a sound.
            const int32_t diff = static_cast<int32_t>(rms) - static_cast<int32_t>(floor_);
            floor_ = static_cast<uint32_t>(static_cast<int32_t>(floor_) + diff / 32);
            return out;
        }
        in_burst_ = true;
        frames_ = 1;
        quiet_ = 0;
        peak_ = rms;
        sum_ = rms;
        early_peak_ = rms;
        dips_ = 0;
        in_dip_ = false;
        blow_ = false;
        return out;
    }

    // Inside a burst. A dip is a frame well under the burst's running mean
    // (the gap between syllables); a steady blow has none.
    const uint32_t mean_before = sum_ / frames_;
    ++frames_;
    sum_ += rms;
    peak_ = std::max<uint32_t>(peak_, rms);
    if (frames_ <= 3) early_peak_ = std::max<uint32_t>(early_peak_, rms);
    if (frames_ > 8) {
        if (!in_dip_ && static_cast<uint32_t>(rms) * 2 < mean_before) {
            in_dip_ = true;
            ++dips_;
        } else if (in_dip_ && static_cast<uint32_t>(rms) * 10 >= mean_before * 6) {
            in_dip_ = false;
        }
    }
    quiet_ = rms < OffLevel() ? quiet_ + 1 : 0;
    if (quiet_ >= kQuietEndFrames) {
        EndBurst(&out);
        return out;
    }
    const uint32_t length = frames_ - quiet_;
    if (length >= kDroneFrames) {  // a fan or a TV: the room got louder
        floor_ = sum_ / frames_;
        in_burst_ = false;
        blow_ = false;
        return out;
    }
    if (length >= kBlowMinFrames && dips_ <= 1 &&
        sum_ / frames_ >= std::max(kBlowMinLevel, floor_ * 5))
        blow_ = true;
    if (dips_ > 1) blow_ = false;  // syllables: that is speech
    out.blowing = blow_;
    return out;
}

void MicClassifier::EndBurst(MicDecision* out) {
    const uint32_t length = frames_ - quiet_;
    // A clap: short, loud, and loudest right at the start (a blow or a
    // vowel builds up; a clap does not).
    if (!blow_ && length <= kClapMaxFrames && peak_ >= std::max(kClapMinPeak, floor_ * 8) &&
        early_peak_ * 10 >= peak_ * 6)
        out->clap = true;
    in_burst_ = false;
    blow_ = false;
}

uint16_t SynthEnvelopeRms(MicSynth kind, uint32_t t_ms) {
    switch (kind) {
        case MicSynth::Silence:
            return static_cast<uint16_t>(135 + Jitter(t_ms, 30));
        case MicSynth::Noise:  // a steady room (fan, TV next door): under the on-level
            return static_cast<uint16_t>(380 + Jitter(t_ms, 60));
        case MicSynth::Blow: {  // wind on the microphone: builds up over 60 ms, then steady
            const uint32_t ramp = std::min<uint32_t>(t_ms, 60);
            return static_cast<uint16_t>(500 + 2700 * ramp / 60 + Jitter(t_ms, 200));
        }
        case MicSynth::Clap: {  // full strength at once, gone in 60 ms
            static const uint16_t kShape[] = {9000, 6500, 3500, 1500, 600, 300};
            const uint32_t i = t_ms / MicClassifier::kFrameMs;
            return i < sizeof(kShape) / sizeof(kShape[0])
                       ? kShape[i]
                       : static_cast<uint16_t>(135 + Jitter(t_ms, 30));
        }
        case MicSynth::Speech: {  // four syllables a second, each a hump with a gap
            const uint32_t phase = t_ms % 250;
            const uint32_t hump = phase < 190 ? (phase < 95 ? phase : 190 - phase) : 0;
            return static_cast<uint16_t>(135 + hump * 26 + Jitter(t_ms, 40));
        }
    }
    return 0;
}

}  // namespace kid_app
