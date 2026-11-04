#pragma once

#include "kid_app/rtc_clock.h"

#include <cstdint>

namespace kid_app {

// The pet's day (PRD v0.2 §6, ADR-010). Minutes after local midnight.
struct DailySchedule {
    uint16_t wake_min = 7 * 60;               // RTC alarm wakes the pet
    uint16_t evening_min = 19 * 60 + 30;      // sleepy, bedtime stories
    uint16_t quiet_start_min = 20 * 60 + 30;  // quiet hours (may wrap)
    uint16_t quiet_end_min = 7 * 60;
    bool nap_enabled = false;
    uint16_t nap_start_min = 13 * 60;
    uint16_t nap_end_min = 14 * 60;
};

enum class DayPhase : uint8_t { Night, Morning, Day, Nap, Evening };

// Pure, offline: works from the PCF85063 wall clock alone.
class RhythmScheduler {
public:
    static constexpr uint16_t kMorningMinutes = 120;

    // Parent settings are untrusted; invalid plans fall back to defaults.
    static bool Valid(const DailySchedule& plan);
    explicit RhythmScheduler(const DailySchedule& plan = DailySchedule{});

    const DailySchedule& Schedule() const { return plan_; }
    DayPhase PhaseAt(const CivilTime& now) const;
    bool QuietAt(const CivilTime& now) const;
    // Next wake time strictly after now (tomorrow if already past).
    CivilTime NextWake(const CivilTime& now) const;
    uint32_t SecondsUntilWake(const CivilTime& now) const;

private:
    static bool InQuiet(const DailySchedule& plan, uint16_t minute);
    DailySchedule plan_;
};

}  // namespace kid_app
