#include "kid_app/status_bar.h"

#include <algorithm>
#include <cstdlib>

namespace kid_app {
namespace {

constexpr uint32_t kPackNoHour = 31, kPackNoMinute = 63, kPackNoBattery = 7;
// Readings this close to the previous value keep it: SHTC3 noise around
// x.5 degC, PMIC fuel gauge steps around a bar threshold.
constexpr int kTempHoldC10 = 7;
constexpr int kBatteryHoldPct = 2;

uint8_t BatteryBars(int pct) {
    // 13 % -> 1 bar, 38 % -> 2, 63 % -> 3, 88 % -> 4.
    pct = std::max(0, std::min(100, pct));
    return static_cast<uint8_t>(std::min<int>(StatusBar::kBatteryBars, (pct + 12) / 25));
}

int RoundC10(int c10) { return c10 >= 0 ? (c10 + 5) / 10 : -((-c10 + 5) / 10); }

uint8_t Crc8(const uint8_t* data, int length) {
    uint8_t crc = 0xFF;  // Sensirion CRC-8: poly 0x31, init 0xFF
    for (int i = 0; i < length; ++i) {
        crc = static_cast<uint8_t>(crc ^ data[i]);
        for (int bit = 0; bit < 8; ++bit)
            crc = static_cast<uint8_t>((crc & 0x80) ? (crc << 1) ^ 0x31 : crc << 1);
    }
    return crc;
}

}  // namespace

uint32_t PackStatus(const StatusBar& s) {
    const uint32_t hour = s.hour < 24 ? s.hour : kPackNoHour;
    const uint32_t minute = s.minute < 60 ? s.minute : kPackNoMinute;
    const uint32_t bars = s.battery_bars <= StatusBar::kBatteryBars ? s.battery_bars : kPackNoBattery;
    const uint32_t network = static_cast<uint32_t>(s.network) & 0x03;
    return hour | (minute << 5) | (uint32_t{static_cast<uint8_t>(s.temp_c)} << 11) |
           (bars << 19) | (uint32_t{s.charging} << 22) | (uint32_t{s.quiet} << 23) |
           (network << 24) | (uint32_t{s.parent_request} << 26) |
           (uint32_t{s.link_repair} << 27);
}

StatusBar UnpackStatus(uint32_t bits) {
    StatusBar s;
    const uint32_t hour = bits & 0x1F, minute = (bits >> 5) & 0x3F, bars = (bits >> 19) & 0x07;
    s.hour = hour == kPackNoHour ? StatusBar::kNoTime : static_cast<uint8_t>(hour);
    s.minute = minute == kPackNoMinute ? StatusBar::kNoTime : static_cast<uint8_t>(minute);
    s.temp_c = static_cast<int8_t>(static_cast<uint8_t>((bits >> 11) & 0xFF));
    s.battery_bars = bars == kPackNoBattery ? StatusBar::kNoBattery : static_cast<uint8_t>(bars);
    s.charging = ((bits >> 22) & 1) != 0;
    s.quiet = ((bits >> 23) & 1) != 0;
    s.parent_request = ((bits >> 26) & 1) != 0;
    s.link_repair = ((bits >> 27) & 1) != 0;
    const uint32_t network = (bits >> 24) & 0x03;
    s.network = network <= static_cast<uint32_t>(NetworkState::Online)
                    ? static_cast<NetworkState>(network)
                    : NetworkState::Unknown;
    return s;
}

StatusBar MakeStatusBar(const StatusReadings& in, const StatusBar& previous) {
    StatusBar s;
    s.parent_request = in.parent_request;
    s.link_repair = in.link_repair;
    if (in.time_ok && in.hour < 24 && in.minute < 60) {
        s.hour = in.hour;
        s.minute = in.minute;
    }
    if (in.temp_ok) {
        int whole = RoundC10(in.temp_c10);
        if (previous.temp_c != StatusBar::kNoTemp && whole != previous.temp_c &&
            std::abs(in.temp_c10 - previous.temp_c * 10) <= kTempHoldC10)
            whole = previous.temp_c;
        if (whole >= -9 && whole <= 99) s.temp_c = static_cast<int8_t>(whole);
    }
    if (in.battery_pct >= 0) {
        uint8_t bars = BatteryBars(in.battery_pct);
        const uint8_t prev = previous.battery_bars;
        if (prev <= StatusBar::kBatteryBars && bars != prev &&
            (BatteryBars(in.battery_pct + kBatteryHoldPct) == prev ||
             BatteryBars(in.battery_pct - kBatteryHoldPct) == prev))
            bars = prev;
        s.battery_bars = bars;
        s.charging = in.charging;
    }
    s.quiet = in.quiet;
    s.network = in.network;
    return s;
}

bool Shtc3TempC10(const uint8_t raw[6], int* temp_c10) {
    if (!raw || !temp_c10 || Crc8(raw, 2) != raw[2]) return false;
    const int ticks = (raw[0] << 8) | raw[1];
    *temp_c10 = -450 + (1750 * ticks) / 65536;
    return true;
}

}  // namespace kid_app
