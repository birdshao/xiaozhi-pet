#include "kid_app/rhythm_scheduler.h"

#include <initializer_list>

namespace kid_app {

namespace {
constexpr uint16_t kMinutesPerDay = 24 * 60;

uint16_t MinuteOfDay(const CivilTime& t) {
    return static_cast<uint16_t>(t.hour * 60 + t.minute);
}
}  // namespace

bool RhythmScheduler::InQuiet(const DailySchedule& plan, uint16_t minute) {
    if (plan.quiet_start_min < plan.quiet_end_min)
        return minute >= plan.quiet_start_min && minute < plan.quiet_end_min;
    return minute >= plan.quiet_start_min || minute < plan.quiet_end_min;  // wraps
}

bool RhythmScheduler::Valid(const DailySchedule& p) {
    for (uint16_t m : {p.wake_min, p.evening_min, p.quiet_start_min, p.quiet_end_min,
                       p.nap_start_min, p.nap_end_min}) {
        if (m >= kMinutesPerDay) return false;
    }
    if (p.quiet_start_min == p.quiet_end_min) return false;
    // Wake-up, evening and the nap all happen outside quiet hours, in order.
    if (InQuiet(p, p.wake_min) || InQuiet(p, p.evening_min)) return false;
    if (p.evening_min <= p.wake_min || p.evening_min >= p.quiet_start_min) return false;
    if (p.nap_enabled) {
        if (p.nap_start_min >= p.nap_end_min || p.nap_start_min < p.wake_min ||
            p.nap_end_min > p.evening_min || InQuiet(p, p.nap_start_min))
            return false;
    }
    return true;
}

RhythmScheduler::RhythmScheduler(const DailySchedule& plan)
    : plan_(Valid(plan) ? plan : DailySchedule{}) {}

bool RhythmScheduler::QuietAt(const CivilTime& now) const {
    return InQuiet(plan_, MinuteOfDay(now));
}

DayPhase RhythmScheduler::PhaseAt(const CivilTime& now) const {
    const uint16_t m = MinuteOfDay(now);
    if (InQuiet(plan_, m)) return DayPhase::Night;
    const uint16_t morning_end = static_cast<uint16_t>(plan_.wake_min + kMorningMinutes);
    if (m >= plan_.wake_min && m < morning_end && m < plan_.evening_min)
        return DayPhase::Morning;
    if (plan_.nap_enabled && m >= plan_.nap_start_min && m < plan_.nap_end_min)
        return DayPhase::Nap;
    if (m >= plan_.evening_min) return DayPhase::Evening;
    return DayPhase::Day;
}

CivilTime RhythmScheduler::NextWake(const CivilTime& now) const {
    const int64_t at = EpochFromCivil(now);
    const int64_t midnight = at - (now.hour * 3600 + now.minute * 60 + now.second);
    int64_t wake = midnight + static_cast<int64_t>(plan_.wake_min) * 60;
    if (wake <= at) wake += 86400;
    return CivilFromEpoch(wake);
}

uint32_t RhythmScheduler::SecondsUntilWake(const CivilTime& now) const {
    return static_cast<uint32_t>(EpochFromCivil(NextWake(now)) - EpochFromCivil(now));
}

}  // namespace kid_app
