#pragma once

#include "kid_app/care_store.h"
#include "kid_app/rtc_clock.h"

#include <cstdint>

namespace kid_app {

// V8 festivals from the PCF85063 wall clock alone (offline). Gregorian
// festivals are fixed dates; the lunar ones (Spring Festival, Lantern,
// Dragon Boat, Mid-Autumn) come from a built-in table of their Gregorian
// dates for kLunarFirstYear..kLunarLastYear (checked against the
// `lunardate` tables). Outside those years, or with an unknown clock, no
// lunar festival is reported: the pet just has an ordinary day.
constexpr uint16_t kLunarFirstYear = 2024;
constexpr uint16_t kLunarLastYear = 2035;

struct GregorianDate {
    uint8_t month = 0;
    uint8_t day = 0;
};
// Gregorian date of lunar new year (1/1), Dragon Boat (5/5) and Mid-Autumn
// (8/15) in `year`; false outside the table.
bool LunarFestivalDates(uint16_t year, GregorianDate* spring, GregorianDate* dragon_boat,
                        GregorianDate* mid_autumn);

// The festival of that day. The child's birthday (month/day from the parent
// menu; 0/0 = not set) wins over a public festival; the Spring Festival
// lasts lunar 1/1..1/3, the others one day. clock_ok false -> None.
Festival FestivalOn(const CivilTime& now, bool clock_ok, uint8_t birth_month, uint8_t birth_day);

}  // namespace kid_app
