#pragma once

#include <cstdint>

namespace kid_app {

// Local wall-clock time as stored in the PCF85063 (no time zone).
// weekday: 0 = Sunday ... 6 = Saturday.
struct CivilTime {
    uint16_t year = 2000;
    uint8_t month = 1;
    uint8_t day = 1;
    uint8_t hour = 0;
    uint8_t minute = 0;
    uint8_t second = 0;
    uint8_t weekday = 6;
};

// Proleptic Gregorian arithmetic on seconds since 1970-01-01 00:00:00.
int64_t EpochFromCivil(const CivilTime& time);
CivilTime CivilFromEpoch(int64_t seconds);

// True only for 2025..2099: rejects an unset system clock (1970) and the
// PCF85063's two-digit year range limit.
bool PlausibleWallClock(const CivilTime& time);

// Pure register codec; I2C access stays in the board adapter.
namespace Pcf85063 {
constexpr uint8_t kControl2 = 0x01;      // bit7 AIE, bit6 AF
constexpr uint8_t kTimeRegister = 0x04;  // seconds .. years, 7 bytes
constexpr uint8_t kAlarmRegister = 0x0B; // second .. weekday alarm, 5 bytes
constexpr uint8_t kAlarmInterruptEnable = 0x80;
constexpr uint8_t kAlarmFlag = 0x40;

// regs[0..6] = seconds, minutes, hours, days, weekdays, months, years.
// Returns false for non-BCD or out-of-range fields.
bool DecodeTime(const uint8_t* regs, CivilTime* out, bool* oscillator_stopped);
// Clears the oscillator-stop flag (bit7 of seconds); 24-hour mode, 20xx.
void EncodeTime(const CivilTime& time, uint8_t* regs);
// Second/minute/hour/day matching enabled, weekday matching disabled.
void EncodeAlarm(const CivilTime& at, uint8_t* regs);
}  // namespace Pcf85063

}  // namespace kid_app
