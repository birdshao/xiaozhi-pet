#include "kid_app/status_bar.h"

#include <cassert>
#include <cstdint>
#include <iostream>

using namespace kid_app;

namespace {

StatusReadings Readings(int temp_c10, int battery_pct) {
    StatusReadings in;
    in.time_ok = true;
    in.hour = 7;
    in.minute = 42;
    in.temp_ok = true;
    in.temp_c10 = temp_c10;
    in.battery_pct = battery_pct;
    return in;
}

}  // namespace

int main() {
    // Defaults: every field unknown, so nothing is drawn.
    {
        const StatusBar none;
        assert(none.hour == StatusBar::kNoTime && none.minute == StatusBar::kNoTime);
        assert(none.temp_c == StatusBar::kNoTemp && none.battery_bars == StatusBar::kNoBattery);
        assert(!none.charging && !none.quiet && none.network == NetworkState::Unknown);
        assert(UnpackStatus(PackStatus(none)) == none);
    }

    // The packed word shared between board tasks keeps every field.
    {
        for (int hour : {0, 9, 23, int{StatusBar::kNoTime}}) {
            for (int minute : {0, 30, 59, int{StatusBar::kNoTime}}) {
                for (int temp : {-9, -1, 0, 24, 99, int{StatusBar::kNoTemp}}) {
                    for (int bars : {0, 2, 4, int{StatusBar::kNoBattery}}) {
                        for (int flags = 0; flags < 4; ++flags) {
                            for (NetworkState net : {NetworkState::Unknown, NetworkState::Offline,
                                                     NetworkState::Online}) {
                                StatusBar in;
                                in.hour = static_cast<uint8_t>(hour);
                                in.minute = static_cast<uint8_t>(minute);
                                in.temp_c = static_cast<int8_t>(temp);
                                in.battery_bars = static_cast<uint8_t>(bars);
                                in.charging = (flags & 1) != 0;
                                in.quiet = (flags & 2) != 0;
                                in.network = net;
                                const uint32_t bits = PackStatus(in);
                                assert(bits < (1u << 26));
                                assert(UnpackStatus(bits) == in);
                            }
                        }
                    }
                }
            }
        }
        // Out-of-range values pack as unknown instead of spilling.
        StatusBar wild;
        wild.hour = 24;
        wild.minute = 60;
        wild.battery_bars = 5;
        const StatusBar back = UnpackStatus(PackStatus(wild));
        assert(back.hour == StatusBar::kNoTime && back.minute == StatusBar::kNoTime);
        assert(back.battery_bars == StatusBar::kNoBattery);
        assert(UnpackStatus(0x03u << 24).network == NetworkState::Unknown);
        // D5: the parent-request envelope has its own bit.
        StatusBar request;
        request.parent_request = true;
        assert(PackStatus(request) == (PackStatus(StatusBar{}) | (1u << 26)));
        assert(UnpackStatus(PackStatus(request)) == request && request != StatusBar{});
        StatusReadings asked;
        asked.parent_request = true;
        assert(MakeStatusBar(asked, StatusBar{}).parent_request);
        assert(!MakeStatusBar(StatusReadings{}, request).parent_request);
    }

    // Readings -> shown values: rounding, quantizing, hiding.
    {
        const StatusBar none;
        StatusBar s = MakeStatusBar(Readings(244, 70), none);
        assert(s.hour == 7 && s.minute == 42 && s.temp_c == 24 && s.battery_bars == 3);
        assert(MakeStatusBar(Readings(245, 70), none).temp_c == 25);
        assert(MakeStatusBar(Readings(-14, 70), none).temp_c == -1);
        assert(MakeStatusBar(Readings(-15, 70), none).temp_c == -2);
        // Values the band cannot draw are hidden, not clamped.
        assert(MakeStatusBar(Readings(-95, 70), none).temp_c == StatusBar::kNoTemp);
        assert(MakeStatusBar(Readings(995, 70), none).temp_c == StatusBar::kNoTemp);
        assert(MakeStatusBar(Readings(994, 70), none).temp_c == 99);
        // Battery: 4 bars, thresholds 13 / 38 / 63 / 88 %.
        const int pct[] = {0, 12, 13, 37, 38, 62, 63, 87, 88, 100, 150};
        const int bars[] = {0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 4};
        for (int i = 0; i < 11; ++i) assert(MakeStatusBar(Readings(200, pct[i]), none).battery_bars == bars[i]);
        // No PMIC reading: no battery and no charging bolt.
        StatusReadings no_pmic = Readings(200, -1);
        no_pmic.charging = true;
        s = MakeStatusBar(no_pmic, none);
        assert(s.battery_bars == StatusBar::kNoBattery && !s.charging);
        StatusReadings charging = Readings(200, 50);
        charging.charging = true;
        charging.quiet = true;
        charging.network = NetworkState::Online;
        s = MakeStatusBar(charging, none);
        assert(s.charging && s.quiet && s.network == NetworkState::Online);
        // RTC not set or out of range: no clock.
        StatusReadings no_time = Readings(200, 50);
        no_time.time_ok = false;
        assert(MakeStatusBar(no_time, none).hour == StatusBar::kNoTime);
        no_time.time_ok = true;
        no_time.minute = 60;
        s = MakeStatusBar(no_time, none);
        assert(s.hour == StatusBar::kNoTime && s.minute == StatusBar::kNoTime);
        StatusReadings no_temp = Readings(200, 50);
        no_temp.temp_ok = false;
        assert(MakeStatusBar(no_temp, none).temp_c == StatusBar::kNoTemp);
    }

    // Hysteresis: sensor jitter around a rounding edge or a bar threshold
    // keeps the shown value, a real change still gets through.
    {
        StatusBar shown = MakeStatusBar(Readings(244, 39), StatusBar{});
        assert(shown.temp_c == 24 && shown.battery_bars == 2);
        for (int t : {245, 247, 238, 233}) assert(MakeStatusBar(Readings(t, 39), shown).temp_c == 24);
        assert(MakeStatusBar(Readings(248, 39), shown).temp_c == 25);
        assert(MakeStatusBar(Readings(232, 39), shown).temp_c == 23);
        for (int p : {36, 37}) assert(MakeStatusBar(Readings(244, p), shown).battery_bars == 2);
        assert(MakeStatusBar(Readings(244, 35), shown).battery_bars == 1);
        StatusBar low = shown;
        low.battery_bars = 1;
        for (int p : {38, 39}) assert(MakeStatusBar(Readings(244, p), low).battery_bars == 1);
        assert(MakeStatusBar(Readings(244, 40), low).battery_bars == 2);
        // Jumps of two bars are never held.
        StatusBar empty = shown;
        empty.battery_bars = 0;
        assert(MakeStatusBar(Readings(244, 63), empty).battery_bars == 3);
    }

    // SHTC3 raw frames: CRC-8 (Sensirion example 0xBEEF -> 0x92) and the
    // datasheet conversion T = -45 + 175 * raw / 2^16.
    {
        int c10 = 0;
        const uint8_t example[6] = {0xBE, 0xEF, 0x92, 0, 0, 0};
        assert(Shtc3TempC10(example, &c10));
        assert(c10 == -450 + (1750 * 0xBEEF) / 65536);
        const uint8_t bad[6] = {0xBE, 0xEF, 0x93, 0, 0, 0};
        c10 = 12345;
        assert(!Shtc3TempC10(bad, &c10) && c10 == 12345);
        const uint8_t zero[6] = {0x00, 0x00, 0x81, 0, 0, 0};  // CRC of 0x0000
        assert(Shtc3TempC10(zero, &c10) && c10 == -450);
        assert(!Shtc3TempC10(nullptr, &c10) && !Shtc3TempC10(example, nullptr));
    }

    // Status refresh schedule: only when the band would look different,
    // at most once a minute, wrap-safe.
    {
        StatusSchedule schedule;
        StatusBar shown, latest;
        assert(!schedule.Due(shown, latest, 0));
        latest.hour = 7;
        latest.minute = 42;
        assert(schedule.Due(shown, latest, 0));  // nothing drawn yet
        schedule.Drawn(1000);
        shown = latest;
        latest.minute = 43;
        assert(!schedule.Due(shown, latest, 1000 + StatusSchedule::kMinIntervalMs - 1));
        assert(schedule.Due(shown, latest, 1000 + StatusSchedule::kMinIntervalMs));
        assert(!schedule.Due(latest, latest, 1000 + 5 * StatusSchedule::kMinIntervalMs));
        StatusSchedule wrap;
        wrap.Drawn(0xFFFFF000u);
        assert(!wrap.Due(shown, latest, 0x00001000u));
        assert(wrap.Due(shown, latest, 0xFFFFF000u + StatusSchedule::kMinIntervalMs));
    }

    std::cout << "PASS: status bar packing, readings, SHTC3 CRC and once-a-minute schedule\n";
    return 0;
}
