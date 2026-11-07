#pragma once

#include "kid_app/mini_game.h"

#include <cstddef>
#include <cstdint>

namespace kid_app {

// V8-01 "parrot": the child holds the dial centre and says something (up to
// 5 s); on release TuanTuan repeats it in a higher, faster voice.
//
// PRIVACY (ADR-011, PRD section 2 item 7). The recording lives ONLY in the
// caller's RAM buffer handed to Bind(); this module has no file, flash,
// NVS, network or logging access (tests/test_v8_parrot_privacy.py fails the
// build if one appears in this header or parrot_session.cc). The buffer is
// zeroed
//   - right after the last playback block was produced,
//   - on Interrupt() / Close() / an audio refusal / a too-short take,
//   - when the session is destroyed,
// so nothing of the voice survives the playback. Nothing is ever uploaded.
//
// Button design: BOOT stays "talk to XiaoZhi" everywhere. In the parrot
// activity the board's input task does NOT forward the dial centre edges to
// InputHub (so its 1 s "back home" long press and the parent combo cannot
// fire); it forwards them to Hold() instead. Leaving = turn the dial
// (Input Up/Down) or PWR (sleep) / BOOT (Interrupt). Hold-to-talk is
// physical, so the activity can never record by voice command: the voice
// command only opens the screen.
//
// Pure and deterministic: no clock of its own (uint32 ms passed in,
// wrap-safe), no heap, no randomness (the voice variant comes from the
// caller's seed). Single owner (board input task); the mic task only calls
// Feed() through the board's own queue.
enum class ParrotPhase : uint8_t { Idle, Ready, Recording, Playing };
enum class ParrotVoice : uint8_t { Parrot = 0, Chipmunk = 1 };
enum class ParrotHint : uint8_t { None, Quiet, LowBattery, Busy, TooShort };
const char* ParrotHintName(ParrotHint hint);

struct ParrotContext {
    bool quiet_hours = false;
    bool low_battery = false;
    bool voice_active = false;  // XiaoZhi session running
    bool audio_busy = false;    // story / song / cue playing
};

struct ParrotOutput {
    bool redraw = false;     // draw View() (one partial refresh)
    bool start_mic = false;  // arm the microphone tap
    bool stop_mic = false;   // disarm it
    bool play = false;       // PlaybackSamples() of 24 kHz PCM from Render()
    bool leave = false;      // back home
    ParrotHint hint = ParrotHint::None;
};

struct ParrotView {
    ParrotPhase phase = ParrotPhase::Idle;
    ParrotHint hint = ParrotHint::None;
};

class ParrotSession {
public:
    static constexpr uint32_t kInRate = 16000;   // microphone samples per second
    static constexpr uint32_t kOutRate = 24000;  // local PCM path (AudioService)
    static constexpr size_t kMaxSamples = 5u * kInRate;  // 5 s at kInRate
    static constexpr size_t kMinSamples = kInRate * 4 / 10;  // shorter is a tap
    static constexpr uint32_t kMaxRecordMs = 5000;
    static constexpr uint32_t kIdleExitMs = 60000;  // nobody holds: quietly home
    static constexpr int16_t kOutPeak = 14000;      // hard cap (about 43 % of full scale)
    static constexpr uint32_t kMaxGainQ8 = 3 * 256; // never boost more than 3x
    static constexpr uint32_t kPermille[2] = {1300, 1550};  // speed/pitch factor

    ParrotSession() = default;
    ~ParrotSession() { Wipe(); }
    ParrotSession(const ParrotSession&) = delete;
    ParrotSession& operator=(const ParrotSession&) = delete;

    // The recording buffer (>= kMaxSamples int16 in RAM/PSRAM, owned by the
    // caller, which must outlive the session: the destructor wipes it).
    // Must be called before Open().
    void Bind(int16_t* buffer, size_t capacity);

    // False (and `out->hint` says why) when the activity may not start.
    bool Open(uint32_t now_ms, const ParrotContext& context, ParrotOutput* out);
    // Raw dial-centre edge (see "Button design"). `seed` picks the voice
    // variant of this take (e.g. the take counter); `context` is re-checked
    // at every press.
    ParrotOutput Hold(bool down, uint32_t now_ms, const ParrotContext& context, uint32_t seed);
    // Mic samples at kInRate while Recording; extra samples beyond the cap are
    // dropped. Returns how many were taken.
    size_t Feed(const int16_t* samples, size_t count);
    // Dial turn leaves (not while recording). Long is not used here: the long
    // press is the recording itself.
    ParrotOutput Input(GameInput input, uint32_t now_ms);
    ParrotOutput Tick(uint32_t now_ms);
    // BOOT talk, PWR sleep, a voice session, quiet hours, Pet mode off: stop
    // everything and wipe.
    ParrotOutput Interrupt();
    // The audio arbiter refused or stopped the playback.
    void PlaybackRefused();

    // 24 kHz output after a release; total length in samples while Playing.
    size_t PlaybackSamples() const { return phase_ == ParrotPhase::Playing ? out_samples_ : 0; }
    // Copies up to `max` samples starting at `offset`. Wipes the recording as
    // soon as the final block has been produced. Returns 0 outside Playing.
    size_t Render(size_t offset, int16_t* out, size_t max);

    ParrotPhase Phase() const { return phase_; }
    ParrotView View() const { return {phase_, hint_}; }
    bool Active() const { return phase_ != ParrotPhase::Idle; }
    size_t Recorded() const { return length_; }
    // Test hook for the privacy assertion: true when nothing but zeros is left.
    bool BufferClean() const;

private:
    void Wipe();
    ParrotOutput Stop(bool leave);
    ParrotOutput Release(uint32_t now_ms);
    static bool Reached(uint32_t now_ms, uint32_t at_ms) {
        return static_cast<int32_t>(now_ms - at_ms) >= 0;
    }

    int16_t* buffer_ = nullptr;
    size_t capacity_ = 0;
    size_t length_ = 0;
    ParrotPhase phase_ = ParrotPhase::Idle;
    ParrotHint hint_ = ParrotHint::None;
    ParrotVoice voice_ = ParrotVoice::Parrot;
    uint32_t record_start_ms_ = 0;
    uint32_t last_activity_ms_ = 0;
    // Playback plan, fixed when the key is released.
    size_t out_samples_ = 0;
    uint32_t step_q16_ = 0;
    uint32_t gain_q8_ = 256;
};

}  // namespace kid_app
