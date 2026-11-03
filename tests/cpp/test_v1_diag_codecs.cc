#include "kid_app/motion_stats.h"
#include "kid_app/rtc_clock.h"

#include <array>
#include <cassert>
#include <iostream>

using namespace kid_app;

static bool Same(const CivilTime& a, const CivilTime& b) {
    return a.year == b.year && a.month == b.month && a.day == b.day &&
           a.hour == b.hour && a.minute == b.minute && a.second == b.second &&
           a.weekday == b.weekday;
}

int main() {
    // Civil <-> epoch round trip, including leap day and weekday.
    {
        const CivilTime leap{2028, 2, 29, 23, 59, 58, 2};  // Tuesday
        const int64_t epoch = EpochFromCivil(leap);
        assert(Same(CivilFromEpoch(epoch), leap));
        assert(CivilFromEpoch(0).year == 1970 && CivilFromEpoch(0).weekday == 4);
        const CivilTime next = CivilFromEpoch(epoch + 3);
        assert(next.month == 3 && next.day == 1 && next.hour == 0 &&
               next.minute == 0 && next.second == 1 && next.weekday == 3);
    }

    // PCF85063 time registers 0x04..0x0A: BCD, OS flag, century 20xx.
    {
        // Values observed on the board in the V1 round 1 trial.
        const std::array<uint8_t, 7> regs = {0x89, 0x42, 0x19, 0x09, 0x00, 0x01, 0x00};
        CivilTime t{};
        bool stopped = false;
        assert(Pcf85063::DecodeTime(regs.data(), &t, &stopped));
        assert(stopped);
        assert(t.year == 2000 && t.month == 1 && t.day == 9 && t.hour == 19 &&
               t.minute == 42 && t.second == 9);

        const CivilTime set{2026, 11, 3, 14, 5, 59, 2};
        std::array<uint8_t, 7> out{};
        Pcf85063::EncodeTime(set, out.data());
        assert(out == (std::array<uint8_t, 7>{0x59, 0x05, 0x14, 0x03, 0x02, 0x11, 0x26}));
        assert(Pcf85063::DecodeTime(out.data(), &t, &stopped));
        assert(!stopped && Same(t, set));  // encoding clears the OS flag

        std::array<uint8_t, 7> bad = out;
        bad[5] = 0x13;  // month 13
        assert(!Pcf85063::DecodeTime(bad.data(), &t, &stopped));
        bad = out;
        bad[0] = 0x5A;  // not BCD
        assert(!Pcf85063::DecodeTime(bad.data(), &t, &stopped));
    }

    // Alarm registers 0x0B..0x0F: second/minute/hour/day enabled (bit7=0),
    // weekday disabled (bit7=1), computed with full carry across a month.
    {
        const CivilTime now{2026, 11, 30, 23, 59, 30, 1};
        const CivilTime at = CivilFromEpoch(EpochFromCivil(now) + 90);
        assert(at.month == 12 && at.day == 1 && at.hour == 0 && at.minute == 1 &&
               at.second == 0);
        std::array<uint8_t, 5> alarm{};
        Pcf85063::EncodeAlarm(at, alarm.data());
        assert(alarm == (std::array<uint8_t, 5>{0x00, 0x01, 0x00, 0x01, 0x80}));
    }

    // Plausibility gate for system time before it may overwrite the RTC.
    assert(!PlausibleWallClock(CivilFromEpoch(0)));
    assert(PlausibleWallClock(CivilTime{2026, 11, 3, 8, 0, 0, 2}));
    assert(!PlausibleWallClock(CivilTime{2101, 1, 1, 0, 0, 0, 6}));

    // Accelerometer stats at +/-4 g (8192 LSB per g).
    {
        MotionStats stats;
        assert(stats.TakeSummary().samples == 0);
        for (int i = 0; i < 10; ++i) stats.Add(0, 0, 8192);  // flat, still
        auto s = stats.TakeSummary();
        assert(s.samples == 10 && s.mean_z_mg == 1000 && s.mean_x_mg == 0);
        assert(s.max_dev_mg == 0 && s.min_mag_mg == 1000 && s.max_mag_mg == 1000);
        assert(stats.TakeSummary().samples == 0);  // summary resets the window

        stats.Add(0, 0, 8192);
        stats.Add(0, 0, 16384);   // 2 g spike (shake)
        stats.Add(0, 0, 4096);    // 0.5 g dip (lift / fall)
        stats.Add(-8192, 0, 0);   // turned on its side, still 1 g
        s = stats.TakeSummary();
        assert(s.samples == 4 && s.max_mag_mg == 2000 && s.min_mag_mg == 500);
        assert(s.max_dev_mg == 1000);
        assert(s.mean_x_mg == -250 && s.mean_z_mg == 875);

        // Full-scale values do not overflow the magnitude.
        stats.Add(-32768, -32768, -32768);
        s = stats.TakeSummary();
        assert(s.max_mag_mg >= 6927 && s.max_mag_mg <= 6929);  // 4 g * sqrt(3)
    }

    std::cout << "PASS: PCF85063 BCD/alarm codec, civil time math, accelerometer stats\n";
}
