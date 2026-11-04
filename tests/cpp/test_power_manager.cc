#include "kid_app/power_manager.h"

#include <cassert>
#include <cstring>
#include <iostream>

using namespace kid_app;

namespace {

PowerConditions OnBattery(DayPhase phase = DayPhase::Day) {
    PowerConditions c;
    c.may_power_off = true;
    c.phase_known = true;
    c.phase = phase;
    return c;
}

// Ticks every 100 ms from *now until `until`; returns the first event.
PowerEvent RunUntil(PowerManager& pm, uint32_t* now, uint32_t until, const PowerConditions& c) {
    for (; *now <= until; *now += 100) {
        const PowerEvent e = pm.Tick(*now, c);
        if (e != PowerEvent::None) return e;
    }
    return PowerEvent::None;
}

}  // namespace

int main() {
    using PM = PowerManager;

    // Daytime on battery: Active -> Idle (60 s) -> Nap (10 min after the
    // last input) -> Sleep 30 min into the nap.
    {
        PowerManager pm;
        uint32_t now = 1000;
        pm.Reset(now);
        const auto c = OnBattery();
        assert(pm.State() == PowerState::Active && !pm.Morning());
        assert(pm.Tick(now + PM::kIdleAfterMs - 1, c) == PowerEvent::None);
        assert(pm.Tick(now + PM::kIdleAfterMs, c) == PowerEvent::EnterIdle);
        assert(pm.State() == PowerState::Idle);
        assert(pm.Tick(now + PM::kIdleAfterMs + 100, c) == PowerEvent::None);
        assert(pm.Tick(now + PM::kNapAfterMs - 1, c) == PowerEvent::None);
        assert(pm.Tick(now + PM::kNapAfterMs, c) == PowerEvent::EnterNap);
        assert(pm.State() == PowerState::Nap);
        const uint32_t nap = now + PM::kNapAfterMs;
        assert(pm.Tick(nap + PM::kOffAfterNapMs - 1, c) == PowerEvent::None);
        assert(pm.Tick(nap + PM::kOffAfterNapMs, c) == PowerEvent::Sleep);
        assert(pm.State() == PowerState::Off && pm.LastSleepReason() == SleepReason::Idle);
        // Only once: the board runs the sleep flow now.
        assert(pm.Tick(nap + PM::kOffAfterNapMs + 5000, c) == PowerEvent::None);
        // The PMIC did not cut power (or the child cancelled): awake again.
        pm.Reset(nap + PM::kOffAfterNapMs + 6000);
        assert(pm.State() == PowerState::Active);
        assert(pm.LastSleepReason() == SleepReason::None);
    }

    // Input resets the timers; in Idle it is not consumed, in Nap it only
    // wakes the pet (restore the picture).
    {
        PowerManager pm;
        pm.Reset(0);
        const auto c = OnBattery();
        assert(pm.Tick(59000, c) == PowerEvent::None);
        assert(pm.OnActivity(59000) == PowerEvent::None);
        assert(pm.Tick(59000 + PM::kIdleAfterMs - 1, c) == PowerEvent::None);
        assert(pm.Tick(59000 + PM::kIdleAfterMs, c) == PowerEvent::EnterIdle);
        assert(pm.OnActivity(130000) == PowerEvent::None);
        assert(pm.State() == PowerState::Active);
        uint32_t now = 130000;
        assert(RunUntil(pm, &now, 130000 + PM::kNapAfterMs, c) == PowerEvent::EnterIdle);
        assert(RunUntil(pm, &now, 130000 + PM::kNapAfterMs, c) == PowerEvent::EnterNap);
        assert(now == 130000 + PM::kNapAfterMs);
        assert(pm.OnActivity(now + 5000) == PowerEvent::LeaveNap);
        assert(pm.State() == PowerState::Active);
        assert(pm.OnActivity(now + 6000) == PowerEvent::None);  // already awake
    }

    // Night (quiet hours) and the afternoon nap phase: Nap right after Idle.
    // Night powers off 5 min into the nap; the nap phase keeps 30 min.
    {
        PowerManager pm;
        pm.Reset(0);
        const auto night = OnBattery(DayPhase::Night);
        assert(pm.Tick(PM::kIdleAfterMs, night) == PowerEvent::EnterIdle);
        assert(pm.Tick(PM::kIdleAfterMs + 20, night) == PowerEvent::EnterNap);
        const uint32_t nap = PM::kIdleAfterMs + 20;
        assert(pm.Tick(nap + PM::kNightOffAfterNapMs - 1, night) == PowerEvent::None);
        assert(pm.Tick(nap + PM::kNightOffAfterNapMs, night) == PowerEvent::Sleep);
        assert(pm.LastSleepReason() == SleepReason::Night);

        pm.Reset(0);
        const auto afternoon = OnBattery(DayPhase::Nap);
        assert(pm.Tick(PM::kIdleAfterMs, afternoon) == PowerEvent::EnterIdle);
        assert(pm.Tick(PM::kIdleAfterMs + 20, afternoon) == PowerEvent::EnterNap);
        assert(pm.Tick(nap + PM::kNightOffAfterNapMs, afternoon) == PowerEvent::None);
        assert(pm.Tick(nap + PM::kOffAfterNapMs - 1, afternoon) == PowerEvent::None);
        assert(pm.Tick(nap + PM::kOffAfterNapMs, afternoon) == PowerEvent::Sleep);
        assert(pm.LastSleepReason() == SleepReason::Idle);

        // RTC not set: no phase, daytime timers.
        pm.Reset(0);
        PowerConditions unset = OnBattery(DayPhase::Night);
        unset.phase_known = false;
        assert(pm.Tick(PM::kIdleAfterMs, unset) == PowerEvent::EnterIdle);
        assert(pm.Tick(PM::kIdleAfterMs + 20, unset) == PowerEvent::None);
        assert(pm.Tick(PM::kNapAfterMs, unset) == PowerEvent::EnterNap);
    }

    // On USB power (or during a self-test without an alarm) the pet naps
    // but never powers off by itself; once allowed, it sleeps.
    {
        PowerManager pm;
        pm.Reset(0);
        PowerConditions usb = OnBattery();
        usb.may_power_off = false;
        uint32_t now = 0;
        assert(RunUntil(pm, &now, 3600000, usb) == PowerEvent::EnterIdle);
        assert(RunUntil(pm, &now, 3600000, usb) == PowerEvent::EnterNap);
        assert(RunUntil(pm, &now, 3600000, usb) == PowerEvent::None);
        assert(pm.State() == PowerState::Nap);
        assert(pm.Tick(now + 100, OnBattery()) == PowerEvent::Sleep);
    }

    // Busy (voice session, sleep flow) counts as activity and wakes a nap.
    {
        PowerManager pm;
        pm.Reset(0);
        const auto c = OnBattery();
        PowerConditions busy = c;
        busy.busy = true;
        uint32_t now = 0;
        assert(RunUntil(pm, &now, 3600000, busy) == PowerEvent::None);
        assert(pm.State() == PowerState::Active);
        assert(pm.Tick(3600000 + PM::kIdleAfterMs - 1, c) == PowerEvent::None);
        now = 3600000 + PM::kIdleAfterMs;
        assert(pm.Tick(now, c) == PowerEvent::EnterIdle);
        assert(pm.Tick(now + PM::kNapAfterMs, c) == PowerEvent::EnterNap);
        assert(pm.Tick(now + PM::kNapAfterMs + 100, busy) == PowerEvent::LeaveNap);
        assert(pm.State() == PowerState::Active);
    }

    // Morning: 60 s without input after the alarm wake -> back to sleep,
    // regardless of battery (the alarm must be re-armed for tomorrow).
    {
        PowerManager pm;
        pm.StartMorning(5000);
        assert(pm.Morning() && pm.State() == PowerState::Active);
        PowerConditions usb;
        usb.may_power_off = false;
        // No Idle/Nap inside the morning window.
        assert(pm.Tick(5000 + PM::kMorningAwakeMs - 1, usb) == PowerEvent::None);
        assert(pm.Tick(5000 + PM::kMorningAwakeMs, usb) == PowerEvent::Sleep);
        assert(pm.LastSleepReason() == SleepReason::Morning && !pm.Morning());
        assert(pm.State() == PowerState::Off);
    }

    // Any input during the morning keeps the pet awake: normal timers after.
    {
        PowerManager pm;
        pm.StartMorning(0);
        const auto c = OnBattery(DayPhase::Morning);
        assert(pm.Tick(30000, c) == PowerEvent::None);
        assert(pm.OnActivity(30000) == PowerEvent::StayAwake);
        assert(!pm.Morning());
        assert(pm.OnActivity(31000) == PowerEvent::None);
        assert(pm.Tick(31000 + PM::kMorningAwakeMs, c) == PowerEvent::EnterIdle);
        assert(pm.State() == PowerState::Idle);
    }

    // A voice session during the morning postpones the timeout; the window
    // starts again when it ends. Reset() ends the morning (Pet mode left).
    {
        PowerManager pm;
        pm.StartMorning(0);
        PowerConditions busy = OnBattery();
        busy.busy = true;
        assert(pm.Tick(50000, busy) == PowerEvent::None);
        assert(pm.Morning());
        assert(pm.Tick(50000 + PM::kMorningAwakeMs - 1, OnBattery()) == PowerEvent::None);
        assert(pm.Tick(50000 + PM::kMorningAwakeMs, OnBattery()) == PowerEvent::Sleep);
        pm.StartMorning(0);
        pm.Reset(10);
        assert(!pm.Morning());
        assert(pm.Tick(PM::kMorningAwakeMs + 10, OnBattery()) == PowerEvent::EnterIdle);
    }

    // Input while the sleep flow runs (state Off) returns to Active without
    // a picture change: the sleep flow itself restores the scene.
    {
        PowerManager pm;
        pm.Reset(0);
        assert(pm.Tick(0, OnBattery()) == PowerEvent::None);
        pm.StartMorning(0);
        assert(pm.Tick(PM::kMorningAwakeMs, OnBattery()) == PowerEvent::Sleep);
        assert(pm.OnActivity(PM::kMorningAwakeMs + 1000) == PowerEvent::None);
        assert(pm.State() == PowerState::Active);
    }

    // The uint32 millisecond clock wraps after ~49.7 days.
    {
        PowerManager pm;
        const uint32_t start = 0xFFFFFFFFu - 30000;
        pm.Reset(start);
        assert(pm.Tick(start + 59000, OnBattery()) == PowerEvent::None);
        assert(pm.Tick(start + PM::kIdleAfterMs, OnBattery()) == PowerEvent::EnterIdle);
        pm.StartMorning(start);
        assert(pm.Tick(start + 59000, OnBattery()) == PowerEvent::None);
        assert(pm.Tick(start + PM::kMorningAwakeMs, OnBattery()) == PowerEvent::Sleep);
    }

    assert(std::strcmp(PowerStateName(PowerState::Nap), "nap") == 0);
    assert(std::strcmp(SleepReasonName(SleepReason::Morning), "morning") == 0);
    assert(std::strcmp(SleepReasonName(SleepReason::None), "none") == 0);

    std::cout << "PASS: power states, night/nap phases, battery gate, busy, morning window\n";
    return 0;
}
