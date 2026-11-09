#pragma once

#include <cstdint>

namespace kid_app {

// V9 wake guard (voice baseline C3 / C6), pure decisions, no ESP-IDF.
// Owner: the board input task; uint32 ms clock, wrap-safe.

// Re-arm gate for the wake word. Call Observe() every loop with the LIVE
// device state and wake word flag. It returns true once when the wake word
// should be switched on again: the board has been idle with the detector off
// for kSettleMs without a break (a wake word clears "running" just before the
// main task leaves idle, so a single sample proves nothing), and at most
// kMaxTries times, kRetryMs apart, until the detector runs again.
class WakeRepairGate {
public:
    static constexpr uint32_t kSettleMs = 1000;
    static constexpr uint32_t kRetryMs = 5000;
    static constexpr int kMaxTries = 6;

    // blocked: something owns the board (sleep flow). Never repairs then.
    bool Observe(bool idle, bool running, bool blocked, uint32_t now_ms);
    int Tries() const { return tries_; }

private:
    bool missing_ = false;
    uint32_t missing_since_ms_ = 0;
    uint32_t last_try_ms_ = 0;
    int tries_ = 0;
};

// What may stop the "chat ended" cue (C6).
struct CueConditions {
    bool idle = false;             // XiaoZhi is idle right now
    bool phase_known = false;      // RTC set
    bool night = false;
    bool sleeping = false;         // sleep flow / sleep scene
    bool quiet_hours = false;      // the parent's quiet hours
    bool power_off = false;
    bool command_pending = false;  // a voice command waits or is deferred
    bool volume_zero = false;      // the parent volume cap is 0
    bool audio_busy = false;       // an error alert, a prompt, a song ...
};

bool ChatEndCueAllowed(const CueConditions& c);

// Armed when a chat (listening / speaking) falls back to idle, fires once
// kDelayMs later if every condition still holds; any failing condition
// cancels it (the error alert's own sound, a "sleep" command, ...).
class ChatEndCue {
public:
    static constexpr uint32_t kDelayMs = 800;

    // Every loop: was_chatting = the previous state was listening/speaking.
    void OnState(bool was_chatting, bool idle, uint32_t now_ms);
    // True once, when the cue is due and still allowed.
    bool Poll(const CueConditions& c, uint32_t now_ms);
    bool Armed() const { return armed_; }

private:
    bool armed_ = false;
    uint32_t due_ms_ = 0;
};

}  // namespace kid_app
