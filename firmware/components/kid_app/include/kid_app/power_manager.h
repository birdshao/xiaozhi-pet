#pragma once

#include "kid_app/rhythm_scheduler.h"

#include <cstdint>

namespace kid_app {

// ADR-010 power states while Pet mode is on (replaces XiaoZhi's
// PowerSaveTimer, which powers off after 300 s with an "OFF" message and no
// wake-up alarm):
//
//   Active --60 s no input--> Idle --10 min no input--> Nap --30 min--> Off
//   Night (quiet hours) and the afternoon Nap phase: Nap right after Idle;
//   at Night Off follows 5 min into the nap.
//
// Off means "go to sleep": the board runs the usual sleep flow (sleeping
// picture, 3 s cancel window, RTC alarm for the next wake, PMIC power-off).
// Idle and Nap only change what the pet does on screen for now (Nap shows a
// drowsy face, no idle blinks); CPU light sleep and Wi-Fi off wait for the
// V3-07 battery measurements.
//
// Morning (V3-04): after an RTC-alarm power-on the pet says good morning and
// goes back to sleep if nobody touches it for 60 s; any input keeps it awake.
//
// Single owner (board input task); uint32 ms clock, wrap-safe.
enum class PowerState : uint8_t { Active, Idle, Nap, Off };
enum class PowerEvent : uint8_t {
    None,
    EnterIdle,  // no visible change
    EnterNap,   // show the drowsy face
    LeaveNap,   // input in Nap: restore the picture, the input only wakes
    StayAwake,  // input during the morning window: restore the home picture
    Sleep,      // start the sleep flow (see SleepReason)
};
enum class SleepReason : uint8_t { None, Idle, Night, Morning };

struct PowerConditions {
    // Voice session, sleep flow or anything else that must not be cut
    // short: counts as activity (as XiaoZhi's CanEnterSleepMode()).
    bool busy = false;
    // Idle/night power-off only on battery (XiaoZhi's timer only runs while
    // discharging) and never during a scripted self-test without an alarm.
    bool may_power_off = false;
    // DayPhase from RhythmScheduler; false when the RTC is not set.
    bool phase_known = false;
    DayPhase phase = DayPhase::Day;
};

class PowerManager {
public:
    static constexpr uint32_t kIdleAfterMs = 60u * 1000;
    static constexpr uint32_t kNapAfterMs = 10u * 60 * 1000;
    static constexpr uint32_t kOffAfterNapMs = 30u * 60 * 1000;
    static constexpr uint32_t kNightOffAfterNapMs = 5u * 60 * 1000;
    static constexpr uint32_t kMorningAwakeMs = 60u * 1000;

    // Pet mode entered, or power-off returned: Active from now on.
    void Reset(uint32_t now_ms);
    // RTC-alarm wake: Active, and Sleep after kMorningAwakeMs without input.
    void StartMorning(uint32_t now_ms);
    // Dial, BOOT, PWR or a gesture.
    PowerEvent OnActivity(uint32_t now_ms);
    // At most one event per call; the board calls this every loop.
    PowerEvent Tick(uint32_t now_ms, const PowerConditions& conditions);

    PowerState State() const { return state_; }
    bool Morning() const { return morning_; }
    SleepReason LastSleepReason() const { return reason_; }

private:
    static bool Drowsy(const PowerConditions& c) {
        return c.phase_known && (c.phase == DayPhase::Night || c.phase == DayPhase::Nap);
    }
    PowerEvent SleepNow(SleepReason reason);

    PowerState state_ = PowerState::Active;
    bool morning_ = false;
    SleepReason reason_ = SleepReason::None;
    uint32_t last_activity_ms_ = 0;
    uint32_t nap_since_ms_ = 0;
    uint32_t morning_since_ms_ = 0;
};

const char* PowerStateName(PowerState state);
const char* SleepReasonName(SleepReason reason);

}  // namespace kid_app
