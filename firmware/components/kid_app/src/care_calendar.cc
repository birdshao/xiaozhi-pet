#include "kid_app/care_calendar.h"

namespace kid_app {
namespace {
struct LunarRow {
    GregorianDate spring, dragon_boat, mid_autumn;
};
// Gregorian dates of lunar 1/1, 5/5 and 8/15 (Lantern = 1/1 + 14 days).
constexpr LunarRow kLunar[] = {
    {{2, 10}, {6, 10}, {9, 17}},  // 2024
    {{1, 29}, {5, 31}, {10, 6}},  // 2025
    {{2, 17}, {6, 19}, {9, 25}},  // 2026
    {{2, 6}, {6, 9}, {9, 15}},    // 2027
    {{1, 26}, {5, 28}, {10, 3}},  // 2028
    {{2, 13}, {6, 16}, {9, 22}},  // 2029
    {{2, 3}, {6, 5}, {9, 12}},    // 2030
    {{1, 23}, {6, 24}, {10, 1}},  // 2031
    {{2, 11}, {6, 12}, {9, 19}},  // 2032
    {{1, 31}, {6, 1}, {9, 8}},    // 2033
    {{2, 19}, {6, 20}, {9, 27}},  // 2034
    {{2, 8}, {6, 10}, {9, 16}},   // 2035
};
static_assert(sizeof(kLunar) / sizeof(kLunar[0]) == kLunarLastYear - kLunarFirstYear + 1,
              "one row per year");

int64_t DayNumber(uint16_t year, uint8_t month, uint8_t day) {
    CivilTime t{};
    t.year = year;
    t.month = month;
    t.day = day;
    return EpochFromCivil(t) / 86400;
}
}  // namespace

bool LunarFestivalDates(uint16_t year, GregorianDate* spring, GregorianDate* dragon_boat,
                        GregorianDate* mid_autumn) {
    if (year < kLunarFirstYear || year > kLunarLastYear) return false;
    const LunarRow& r = kLunar[year - kLunarFirstYear];
    if (spring) *spring = r.spring;
    if (dragon_boat) *dragon_boat = r.dragon_boat;
    if (mid_autumn) *mid_autumn = r.mid_autumn;
    return true;
}

Festival FestivalOn(const CivilTime& now, bool clock_ok, uint8_t birth_month, uint8_t birth_day) {
    if (!clock_ok || !PlausibleWallClock(now)) return Festival::None;
    if (birth_month && now.month == birth_month && now.day == birth_day) return Festival::Birthday;
    // A 29 February birthday is celebrated on the 28th in other years.
    const bool leap = (now.year % 4 == 0 && now.year % 100 != 0) || now.year % 400 == 0;
    if (birth_month == 2 && birth_day == 29 && !leap && now.month == 2 && now.day == 28)
        return Festival::Birthday;
    if (now.month == 1 && now.day == 1) return Festival::NewYear;
    if (now.month == 6 && now.day == 1) return Festival::ChildrensDay;
    GregorianDate spring{}, dragon{}, autumn{};
    if (!LunarFestivalDates(now.year, &spring, &dragon, &autumn)) return Festival::None;
    const int64_t today = DayNumber(now.year, now.month, now.day);
    const int64_t new_year = DayNumber(now.year, spring.month, spring.day);
    if (today >= new_year && today < new_year + 3) return Festival::SpringFestival;
    if (today == new_year + 14) return Festival::Lantern;
    if (now.month == dragon.month && now.day == dragon.day) return Festival::DragonBoat;
    if (now.month == autumn.month && now.day == autumn.day) return Festival::MidAutumn;
    return Festival::None;
}

Season SeasonOn(const CivilTime& now, bool clock_ok) {
    Season s{};
    if (!clock_ok || !PlausibleWallClock(now)) return s;
    GregorianDate spring{}, dragon{}, autumn{};
    if (!LunarFestivalDates(now.year, &spring, &dragon, &autumn)) return s;
    const int64_t today = DayNumber(now.year, now.month, now.day);
    auto within = [&](const GregorianDate& d, int before, int after) {
        const int64_t at = DayNumber(now.year, d.month, d.day);
        return today >= at - before && today <= at + after;
    };
    s.dumpling = within(spring, kSeasonBeforeDays, 14);  // to the Lantern day
    if (within(dragon, kSeasonBeforeDays, kTreatAfterDays)) s.treat = Treat::Zongzi;
    else if (within(autumn, kSeasonBeforeDays, kTreatAfterDays)) s.treat = Treat::Mooncake;
    return s;
}

}  // namespace kid_app
