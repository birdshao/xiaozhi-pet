#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>

namespace kid_app {

// V4-08 microphone games, level 1: from a stream of loudness values to
// "blowing" and "clap" (docs/V4_MINI_GAMES.md section 12).
//
// PRIVACY (ADR-011, PRD section 2 item 7): the only thing that ever leaves
// the audio input task is one RMS number per 10 ms frame (uint16). The
// samples are measured and dropped on the spot: nothing here records,
// stores, encodes or sends audio, and this module has no file, flash or
// network access at all (tests/test_v4_mic_games.py fails the build if one
// appears).
//
// Pure and deterministic: no clock, no heap, no hardware; the same frames
// always give the same decisions. Frames are kFrameMs apart.
//
// The decision looks at one burst of sound at a time (loud enough to rise
// above the learned room level, until it falls back):
//   clap   short (<= 150 ms), loud, and it starts at full strength
//   blow   long (>= 250 ms), steady and loud, no syllable-like gaps
//   other  speech (bursts that are short or keep dipping), room noise
//          (stays below the on-level and is learned as the floor), a loud
//          drone (> 6 s: absorbed as the new floor)
// All thresholds are first values; V1-10 hardware data tunes them.
struct MicDecision {
    bool blowing = false;  // true on every frame while a blow is going on
    bool clap = false;     // true on the one frame where a clap is confirmed
};

class MicClassifier {
public:
    static constexpr uint32_t kFrameMs = 10;
    static constexpr uint32_t kWarmFrames = 30;     // learn the room level first
    static constexpr uint32_t kQuietEndFrames = 3;  // 30 ms below the off-level ends a burst
    static constexpr uint32_t kBlowMinFrames = 25;  // 250 ms
    static constexpr uint32_t kClapMaxFrames = 15;  // 150 ms
    static constexpr uint32_t kDroneFrames = 600;   // 6 s: ambient, not a blow
    static constexpr uint32_t kBlowMinLevel = 1500; // mean RMS of a blow
    static constexpr uint32_t kClapMinPeak = 3000;

    MicDecision Feed(uint16_t rms);
    void Reset();

    uint16_t Floor() const { return static_cast<uint16_t>(floor_); }
    bool Warm() const { return frames_seen_ >= kWarmFrames; }

private:
    uint32_t OnLevel() const;   // a burst starts above this
    uint32_t OffLevel() const;  // ... and ends below this
    void EndBurst(MicDecision* out);

    uint32_t floor_ = 0;
    uint32_t frames_seen_ = 0;
    uint32_t warm_sum_ = 0;
    bool in_burst_ = false;
    uint32_t frames_ = 0;  // burst length so far
    uint32_t quiet_ = 0;   // trailing quiet frames
    uint32_t peak_ = 0;
    uint32_t sum_ = 0;
    uint32_t early_peak_ = 0;  // loudest of the first three frames
    uint32_t dips_ = 0;
    bool in_dip_ = false;
    bool blow_ = false;
};

// Deterministic stand-ins for the loudness of real sounds (RMS per frame at
// time t_ms since the sound began). The host self-test (`envelope` step)
// feeds them to the classifier instead of the microphone so a hardware
// trial is repeatable; the unit tests use the same shapes.
enum class MicSynth : uint8_t { Silence, Blow, Clap, Speech, Noise };
uint16_t SynthEnvelopeRms(MicSynth kind, uint32_t t_ms);
inline const char* MicSynthName(MicSynth kind) {
    switch (kind) {
        case MicSynth::Silence: return "silence";
        case MicSynth::Blow: return "blow";
        case MicSynth::Clap: return "clap";
        case MicSynth::Speech: return "speech";
        case MicSynth::Noise: return "noise";
    }
    return "unknown";
}
// "blow", "clap", "speech", "noise" (silence is what a finished step falls
// back to, not a step of its own). Inline: the self-test script parser uses
// it without linking the classifier.
inline bool MicSynthFromName(const char* name, MicSynth* out) {
    if (!name || !out) return false;
    for (MicSynth kind : {MicSynth::Blow, MicSynth::Clap, MicSynth::Speech, MicSynth::Noise}) {
        const char* want = MicSynthName(kind);
        size_t i = 0;
        while (want[i] && name[i] == want[i]) ++i;
        if (!want[i] && !name[i]) {
            *out = kind;
            return true;
        }
    }
    return false;
}

}  // namespace kid_app
