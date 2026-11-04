#include "kid_app/power_manager.h"
#include "kid_app/selftest_clock.h"

namespace kid_app {

void PowerManager::Reset(uint32_t now_ms) {
    state_ = PowerState::Active;
    morning_ = false;
    reason_ = SleepReason::None;
    last_activity_ms_ = now_ms;
}

void PowerManager::StartMorning(uint32_t now_ms) {
    Reset(now_ms);
    morning_ = true;
    morning_since_ms_ = now_ms;
}

PowerEvent PowerManager::OnActivity(uint32_t now_ms) {
    last_activity_ms_ = now_ms;
    const PowerState previous = state_;
    state_ = PowerState::Active;
    if (morning_) {
        morning_ = false;
        return PowerEvent::StayAwake;
    }
    return previous == PowerState::Nap ? PowerEvent::LeaveNap : PowerEvent::None;
}

PowerEvent PowerManager::SleepNow(SleepReason reason) {
    state_ = PowerState::Off;
    reason_ = reason;
    return PowerEvent::Sleep;
}

PowerEvent PowerManager::Tick(uint32_t now_ms, const PowerConditions& conditions) {
    // Off: the board is running the sleep flow; Reset() or input ends it.
    if (state_ == PowerState::Off) return PowerEvent::None;
    if (conditions.busy) {
        // Postpones every timeout, including the morning one: a voice
        // session must not be cut off by "nobody touched the dial".
        last_activity_ms_ = now_ms;
        morning_since_ms_ = now_ms;
        const PowerState previous = state_;
        state_ = PowerState::Active;
        return previous == PowerState::Nap ? PowerEvent::LeaveNap : PowerEvent::None;
    }
    if (morning_) {
        if (now_ms - morning_since_ms_ < SelfTestClock::Scaled(kMorningAwakeMs))
            return PowerEvent::None;
        morning_ = false;
        return SleepNow(SleepReason::Morning);
    }
    const uint32_t quiet_ms = now_ms - last_activity_ms_;
    const bool drowsy = Drowsy(conditions);
    const bool night = conditions.phase_known && conditions.phase == DayPhase::Night;
    switch (state_) {
        case PowerState::Active:
            if (quiet_ms < SelfTestClock::Scaled(kIdleAfterMs)) return PowerEvent::None;
            state_ = PowerState::Idle;
            return PowerEvent::EnterIdle;
        case PowerState::Idle:
            if (quiet_ms < SelfTestClock::Scaled(drowsy ? kIdleAfterMs : kNapAfterMs))
                return PowerEvent::None;
            state_ = PowerState::Nap;
            nap_since_ms_ = now_ms;
            return PowerEvent::EnterNap;
        case PowerState::Nap: {
            const bool low = conditions.low_battery;
            const uint32_t off_after = night ? kNightOffAfterNapMs
                                       : low ? kLowBatteryOffAfterNapMs
                                             : kOffAfterNapMs;
            if (!conditions.may_power_off ||
                now_ms - nap_since_ms_ < SelfTestClock::Scaled(off_after))
                return PowerEvent::None;
            return SleepNow(night ? SleepReason::Night
                            : low ? SleepReason::LowBattery
                                  : SleepReason::Idle);
        }
        case PowerState::Off:
            break;
    }
    return PowerEvent::None;
}

const char* PowerStateName(PowerState state) {
    switch (state) {
        case PowerState::Active: return "active";
        case PowerState::Idle: return "idle";
        case PowerState::Nap: return "nap";
        case PowerState::Off: return "off";
    }
    return "unknown";
}

const char* SleepReasonName(SleepReason reason) {
    switch (reason) {
        case SleepReason::Idle: return "idle";
        case SleepReason::Night: return "night";
        case SleepReason::Morning: return "morning";
        case SleepReason::LowBattery: return "low_battery";
        case SleepReason::None: break;
    }
    return "none";
}

}  // namespace kid_app
