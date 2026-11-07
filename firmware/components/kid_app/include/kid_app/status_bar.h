#pragma once

#include <cstdint>

namespace kid_app {

// V2-05 / PRD 5.1: the top status band on the pet home screen. Values are
// what the panel shows (already rounded and quantized), so "changed" means
// "looks different". Each field has an unknown value and is then hidden.
enum class NetworkState : uint8_t { Unknown, Offline, Online };

struct StatusBar {
    static constexpr uint8_t kNoTime = 0xFF;
    static constexpr int8_t kNoTemp = INT8_MIN;
    static constexpr uint8_t kNoBattery = 0xFF;
    static constexpr uint8_t kBatteryBars = 4;

    uint8_t hour = kNoTime;  // 0..23; hidden unless minute is valid too
    uint8_t minute = kNoTime;
    int8_t temp_c = kNoTemp;  // whole degrees; drawn -9..99
    uint8_t battery_bars = kNoBattery;  // 0..kBatteryBars
    bool charging = false;  // bolt next to a known battery
    bool quiet = false;     // quiet hours: small moon
    NetworkState network = NetworkState::Unknown;
    // D5: a request from the parent panel waits for the parent (envelope).
    bool parent_request = false;
};

inline bool operator==(const StatusBar& a, const StatusBar& b) {
    return a.hour == b.hour && a.minute == b.minute && a.temp_c == b.temp_c &&
           a.battery_bars == b.battery_bars && a.charging == b.charging &&
           a.quiet == b.quiet && a.network == b.network &&
           a.parent_request == b.parent_request;
}
inline bool operator!=(const StatusBar& a, const StatusBar& b) { return !(a == b); }

// One-word snapshot shared between board tasks (atomic<uint32_t>), next to
// the packed scene: bits 0-4 hour (31 = none), 5-10 minute (63 = none),
// 11-18 temperature (int8), 19-21 battery bars (7 = none), 22 charging,
// 23 quiet, 24-25 network, 26 parent request. Out-of-range values pack
// as unknown.
uint32_t PackStatus(const StatusBar& status);
StatusBar UnpackStatus(uint32_t bits);

// Raw readings from the board; MakeStatusBar() rounds and quantizes them
// with a little hysteresis so sensor jitter does not cost ePaper refreshes.
struct StatusReadings {
    bool time_ok = false;
    uint8_t hour = 0;
    uint8_t minute = 0;
    bool temp_ok = false;
    int temp_c10 = 0;  // 0.1 degC
    int battery_pct = -1;  // < 0: no PMIC reading
    bool charging = false;
    bool quiet = false;
    NetworkState network = NetworkState::Unknown;
    bool parent_request = false;
};

// previous: the last value this function returned (hysteresis).
StatusBar MakeStatusBar(const StatusReadings& in, const StatusBar& previous);

// SHTC3 normal-mode measurement (T first): 6 bytes T, T, CRC, RH, RH, CRC.
// False when the temperature CRC does not match.
bool Shtc3TempC10(const uint8_t raw[6], int* temp_c10);

// When the status band may be redrawn on its own: only when a shown value
// changed and at most once a minute. Single owner: the ePaper task.
class StatusSchedule {
public:
    static constexpr uint32_t kMinIntervalMs = 60000;

    bool Due(const StatusBar& shown, const StatusBar& latest, uint32_t now_ms) const {
        if (shown == latest) return false;
        return !drawn_ || static_cast<uint32_t>(now_ms - last_ms_) >= kMinIntervalMs;
    }
    // A status refresh ran (a failed one retries a minute later). Full
    // refreshes that pick up new values on the way do not count, so they
    // never delay the next clock tick.
    void Drawn(uint32_t now_ms) {
        drawn_ = true;
        last_ms_ = now_ms;
    }

private:
    bool drawn_ = false;
    uint32_t last_ms_ = 0;
};

}  // namespace kid_app
