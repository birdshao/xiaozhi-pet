#include "kid_app/rhythm_scheduler.h"

#include <cassert>
#include <iostream>

using namespace kid_app;

static CivilTime At(uint16_t y, uint8_t mo, uint8_t d, uint8_t h, uint8_t mi, uint8_t s = 0) {
    CivilTime t{y, mo, d, h, mi, s, 0};
    return CivilFromEpoch(EpochFromCivil(t));  // fills the weekday
}

int main() {
    // Default family day (PRD v0.2 §6): wake 07:00, evening 19:30,
    // quiet 20:30-07:00, nap off.
    {
        const DailySchedule plan{};
        assert(RhythmScheduler::Valid(plan));
        const RhythmScheduler rhythm(plan);
        assert(rhythm.PhaseAt(At(2026, 11, 3, 6, 59, 59)) == DayPhase::Night);
        assert(rhythm.PhaseAt(At(2026, 11, 3, 7, 0)) == DayPhase::Morning);
        assert(rhythm.PhaseAt(At(2026, 11, 3, 8, 59)) == DayPhase::Morning);
        assert(rhythm.PhaseAt(At(2026, 11, 3, 9, 0)) == DayPhase::Day);
        assert(rhythm.PhaseAt(At(2026, 11, 3, 13, 30)) == DayPhase::Day);  // nap off
        assert(rhythm.PhaseAt(At(2026, 11, 3, 19, 29)) == DayPhase::Day);
        assert(rhythm.PhaseAt(At(2026, 11, 3, 19, 30)) == DayPhase::Evening);
        assert(rhythm.PhaseAt(At(2026, 11, 3, 20, 29)) == DayPhase::Evening);
        assert(rhythm.PhaseAt(At(2026, 11, 3, 20, 30)) == DayPhase::Night);
        assert(rhythm.PhaseAt(At(2026, 11, 4, 0, 0)) == DayPhase::Night);
        // Quiet hours wrap midnight.
        assert(rhythm.QuietAt(At(2026, 11, 3, 23, 0)));
        assert(rhythm.QuietAt(At(2026, 11, 4, 6, 59)));
        assert(!rhythm.QuietAt(At(2026, 11, 4, 7, 0)));
        assert(!rhythm.QuietAt(At(2026, 11, 3, 20, 29)));
    }

    // Next wake-up is the next 07:00 strictly after now, across day, month
    // and year boundaries (used for the RTC alarm when the pet sleeps).
    {
        const RhythmScheduler rhythm(DailySchedule{});
        CivilTime next = rhythm.NextWake(At(2026, 11, 3, 20, 45));
        assert(next.year == 2026 && next.month == 11 && next.day == 4 && next.hour == 7 &&
               next.minute == 0 && next.second == 0);
        next = rhythm.NextWake(At(2026, 11, 4, 5, 10));
        assert(next.day == 4 && next.hour == 7);
        next = rhythm.NextWake(At(2026, 11, 4, 7, 0));  // exactly at wake: tomorrow
        assert(next.day == 5 && next.hour == 7);
        next = rhythm.NextWake(At(2026, 12, 31, 21, 0));
        assert(next.year == 2027 && next.month == 1 && next.day == 1 && next.hour == 7);
        next = rhythm.NextWake(At(2028, 2, 28, 22, 0));
        assert(next.month == 2 && next.day == 29);
        assert(rhythm.SecondsUntilWake(At(2026, 11, 3, 20, 45)) == (10 * 60 + 15) * 60);
    }

    // Optional nap and a custom schedule.
    {
        DailySchedule plan{};
        plan.nap_enabled = true;
        plan.nap_start_min = 13 * 60;
        plan.nap_end_min = 14 * 60 + 30;
        plan.wake_min = 6 * 60 + 30;
        plan.quiet_end_min = 6 * 60 + 30;
        assert(RhythmScheduler::Valid(plan));
        const RhythmScheduler rhythm(plan);
        assert(rhythm.PhaseAt(At(2026, 11, 3, 6, 30)) == DayPhase::Morning);
        assert(rhythm.PhaseAt(At(2026, 11, 3, 12, 59)) == DayPhase::Day);
        assert(rhythm.PhaseAt(At(2026, 11, 3, 13, 0)) == DayPhase::Nap);
        assert(rhythm.PhaseAt(At(2026, 11, 3, 14, 29)) == DayPhase::Nap);
        assert(rhythm.PhaseAt(At(2026, 11, 3, 14, 30)) == DayPhase::Day);
        assert(!rhythm.QuietAt(At(2026, 11, 3, 13, 30)));  // nap is not quiet hours
    }

    // Invalid schedules are rejected (parent settings are untrusted input).
    {
        DailySchedule bad{};
        bad.wake_min = 24 * 60;
        assert(!RhythmScheduler::Valid(bad));
        bad = DailySchedule{};
        bad.quiet_start_min = bad.quiet_end_min;  // empty quiet window
        assert(!RhythmScheduler::Valid(bad));
        bad = DailySchedule{};
        bad.evening_min = 21 * 60;  // evening after quiet starts
        assert(!RhythmScheduler::Valid(bad));
        bad = DailySchedule{};
        bad.wake_min = 6 * 60;  // waking inside quiet hours (ends 07:00)
        assert(!RhythmScheduler::Valid(bad));
        bad = DailySchedule{};
        bad.nap_enabled = true;
        bad.nap_start_min = 14 * 60;
        bad.nap_end_min = 13 * 60;
        assert(!RhythmScheduler::Valid(bad));
        // Constructing from an invalid plan falls back to the defaults.
        const RhythmScheduler fallback(bad);
        assert(fallback.Schedule().wake_min == DailySchedule{}.wake_min);
        assert(!fallback.Schedule().nap_enabled);
    }

    std::cout << "PASS: rhythm phases, quiet hours over midnight, next wake, validation\n";
}
