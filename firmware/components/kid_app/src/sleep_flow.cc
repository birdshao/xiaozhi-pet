#include "kid_app/sleep_flow.h"

namespace kid_app {

SleepAction SleepFlow::Request(uint32_t now_ms) {
    if (phase_ != SleepPhase::Awake) return SleepAction::None;
    phase_ = SleepPhase::Showing;
    since_ms_ = now_ms;
    return SleepAction::ShowSleepScene;
}

SleepAction SleepFlow::OnInput(InputKind kind, uint32_t) {
    if (phase_ != SleepPhase::Showing && phase_ != SleepPhase::Countdown)
        return SleepAction::None;
    switch (kind) {
        case InputKind::DialUp:
        case InputKind::DialDown:
        case InputKind::DialPress:
        case InputKind::DialLong:
        case InputKind::Talk:
        case InputKind::BootLong:
            phase_ = SleepPhase::Awake;
            return SleepAction::RestoreScene;
        case InputKind::SleepRequest:   // pressing PWR again keeps going
        case InputKind::ParentCombo:    // no surprise wake-up
            return SleepAction::None;
    }
    return SleepAction::None;
}

void SleepFlow::OnSceneShown(uint32_t now_ms) {
    if (phase_ != SleepPhase::Showing) return;
    phase_ = SleepPhase::Countdown;
    since_ms_ = now_ms;
}

SleepAction SleepFlow::Tick(uint32_t now_ms) {
    const uint32_t elapsed = now_ms - since_ms_;
    if (phase_ == SleepPhase::Showing && elapsed >= kShowTimeoutMs) {
        phase_ = SleepPhase::Awake;
        return SleepAction::RestoreScene;
    }
    if (phase_ == SleepPhase::Countdown && elapsed >= kCancelWindowMs) {
        phase_ = SleepPhase::PoweringOff;
        return SleepAction::PowerOff;
    }
    return SleepAction::None;
}

}  // namespace kid_app
