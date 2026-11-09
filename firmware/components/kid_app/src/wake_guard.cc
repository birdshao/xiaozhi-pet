#include "kid_app/wake_guard.h"

namespace kid_app {

bool WakeRepairGate::Observe(bool idle, bool running, bool blocked, uint32_t now_ms) {
    if (!idle || running) {
        missing_ = false;
        if (running) tries_ = 0;
        return false;
    }
    if (!missing_) {
        missing_ = true;
        missing_since_ms_ = now_ms;
        return false;
    }
    if (now_ms - missing_since_ms_ < kSettleMs) return false;
    if (blocked || tries_ >= kMaxTries) return false;
    if (tries_ > 0 && now_ms - last_try_ms_ < kRetryMs) return false;
    last_try_ms_ = now_ms;
    ++tries_;
    return true;
}

bool ChatEndCueAllowed(const CueConditions& c) {
    return c.idle && c.phase_known && !c.night && !c.sleeping && !c.quiet_hours &&
           !c.power_off && !c.command_pending && !c.volume_zero && !c.audio_busy;
}

void ChatEndCue::OnState(bool was_chatting, bool idle, uint32_t now_ms) {
    if (!idle) {
        armed_ = false;
    } else if (was_chatting) {
        armed_ = true;
        due_ms_ = now_ms + kDelayMs;
    }
}

bool ChatEndCue::Poll(const CueConditions& c, uint32_t now_ms) {
    if (!armed_) return false;
    if (!ChatEndCueAllowed(c)) {
        armed_ = false;
        return false;
    }
    if (static_cast<int32_t>(now_ms - due_ms_) < 0) return false;
    armed_ = false;
    return true;
}

}  // namespace kid_app
