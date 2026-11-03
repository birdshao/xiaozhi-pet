#include "kid_app/rtc_clock.h"

namespace kid_app {

namespace {
// Howard Hinnant's days_from_civil / civil_from_days algorithms.
int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

bool FromBcd(uint8_t value, uint8_t limit_low, uint8_t limit_high, uint8_t* out) {
    const uint8_t high = value >> 4, low = value & 0x0F;
    if (high > 9 || low > 9) return false;
    const uint8_t decimal = static_cast<uint8_t>(high * 10 + low);
    if (decimal < limit_low || decimal > limit_high) return false;
    *out = decimal;
    return true;
}

uint8_t ToBcd(unsigned value) {
    return static_cast<uint8_t>(((value / 10) << 4) | (value % 10));
}
}  // namespace

int64_t EpochFromCivil(const CivilTime& t) {
    return DaysFromCivil(t.year, t.month, t.day) * 86400 + t.hour * 3600 +
           t.minute * 60 + t.second;
}

CivilTime CivilFromEpoch(int64_t seconds) {
    int64_t days = seconds / 86400;
    int64_t rest = seconds % 86400;
    if (rest < 0) {
        rest += 86400;
        --days;
    }
    const int64_t z = days + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    const int64_t y = static_cast<int64_t>(yoe) + era * 400 + (m <= 2);

    CivilTime out;
    out.year = static_cast<uint16_t>(y);
    out.month = static_cast<uint8_t>(m);
    out.day = static_cast<uint8_t>(d);
    out.hour = static_cast<uint8_t>(rest / 3600);
    out.minute = static_cast<uint8_t>((rest % 3600) / 60);
    out.second = static_cast<uint8_t>(rest % 60);
    // 1970-01-01 was a Thursday (4).
    const int64_t weekday = (days % 7 + 7 + 4) % 7;
    out.weekday = static_cast<uint8_t>(weekday);
    return out;
}

bool PlausibleWallClock(const CivilTime& time) {
    return time.year >= 2025 && time.year <= 2099;
}

namespace Pcf85063 {

bool DecodeTime(const uint8_t* regs, CivilTime* out, bool* oscillator_stopped) {
    if (!regs || !out) return false;
    CivilTime t;
    uint8_t year = 0;
    if (!FromBcd(regs[0] & 0x7F, 0, 59, &t.second) ||
        !FromBcd(regs[1] & 0x7F, 0, 59, &t.minute) ||
        !FromBcd(regs[2] & 0x3F, 0, 23, &t.hour) ||
        !FromBcd(regs[3] & 0x3F, 1, 31, &t.day) ||
        !FromBcd(regs[5] & 0x1F, 1, 12, &t.month) ||
        !FromBcd(regs[6], 0, 99, &year)) {
        return false;
    }
    t.weekday = regs[4] & 0x07;
    if (t.weekday > 6) return false;
    t.year = static_cast<uint16_t>(2000 + year);
    *out = t;
    if (oscillator_stopped) *oscillator_stopped = (regs[0] & 0x80) != 0;
    return true;
}

void EncodeTime(const CivilTime& time, uint8_t* regs) {
    regs[0] = ToBcd(time.second);  // bit7 = 0 clears the OS flag
    regs[1] = ToBcd(time.minute);
    regs[2] = ToBcd(time.hour);
    regs[3] = ToBcd(time.day);
    regs[4] = static_cast<uint8_t>(time.weekday & 0x07);
    regs[5] = ToBcd(time.month);
    regs[6] = ToBcd(static_cast<unsigned>(time.year % 100));
}

void EncodeAlarm(const CivilTime& at, uint8_t* regs) {
    regs[0] = ToBcd(at.second);  // bit7 = 0 enables matching
    regs[1] = ToBcd(at.minute);
    regs[2] = ToBcd(at.hour);
    regs[3] = ToBcd(at.day);
    regs[4] = 0x80;  // weekday matching disabled
}

}  // namespace Pcf85063
}  // namespace kid_app
