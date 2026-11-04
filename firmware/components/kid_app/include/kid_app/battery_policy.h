#pragma once

#include <cstdint>

namespace kid_app {

// V3-05 (PRD §3, TG28 row): low battery and charging. Pure logic, fed with
// raw PMIC readings every couple of seconds; the board acts on the events.
//
//   on battery:  Normal --< 20 %--> Low --< 8 %--> Critical
//                (back to Normal at >= 25 %, Critical -> Low at >= 11 %)
//   USB power:   Charging, or Full when the PMIC stopped charging
//
// The PMIC percentage jitters by a point or two and the charge/discharge
// flags flip for a sample around a plug-in: a power-source change needs
// kSourceReadings agreeing readings in a row, a level band change
// kLevelReadings, and the bands overlap (hysteresis). The first reading
// after boot sets the source silently (no "plugged" on a USB boot).
//
// Critical means a safe shutdown: the board lets queued pet events commit,
// shows the sleeping picture and powers off through the sleep flow, and
// arms no RTC wake alarm (waking into a dead battery would only drain it
// further). The event repeats every kCriticalRetryMs while still critical
// (the child cancelled the sleep, or the PMIC did not cut power).
//
// Reactions (UI only): TuanTuan "eats electricity" right after a plug-in
// and every kEatEveryMs while charging; yawns with a "please charge me"
// bubble on entering Low and every kYawnEveryMs while low.
//
// Single owner (board input task); uint32 ms clock, wrap-safe.
struct BatteryReading {
    int pct = -1;             // PMIC level; outside 0..100: no reading
    bool charging = false;    // PMIC IsCharging()
    bool on_battery = false;  // PMIC IsDischarging(): no external power
};

enum class BatteryState : uint8_t { Unknown, Normal, Low, Critical, Charging, Full };
enum class BatteryEvent : uint8_t {
    None,
    Plugged,     // external power came (charging or full)
    Unplugged,   // back on battery
    Full,        // charging finished, still plugged in
    EnteredLow,  // on battery, below kLowPct
    Critical,    // on battery, below kCriticalPct: safe shutdown (repeats)
};
enum class BatteryReaction : uint8_t { None, Eat, Yawn };

class BatteryPolicy {
public:
    static constexpr int kLowPct = 20;
    static constexpr int kLowClearPct = 25;
    static constexpr int kCriticalPct = 8;
    static constexpr int kCriticalClearPct = 11;
    static constexpr uint8_t kSourceReadings = 2;
    static constexpr uint8_t kLevelReadings = 3;
    static constexpr uint32_t kCriticalRetryMs = 60u * 1000;
    static constexpr uint32_t kYawnEveryMs = 5u * 60 * 1000;
    static constexpr uint32_t kEatEveryMs = 15u * 60 * 1000;

    // One reading; at most one event per call (a pending band event
    // follows on the next reading).
    BatteryEvent Update(const BatteryReading& reading, uint32_t now_ms);
    // The reaction that is due, if `allowed` (Pet home, awake, no voice,
    // not going to sleep); it then waits a full period. A reaction that
    // comes due while not allowed waits until it is.
    BatteryReaction Reaction(uint32_t now_ms, bool allowed);

    BatteryState State() const;
    // Low or Critical, on battery: PowerManager naps and sleeps sooner.
    bool LowPower() const {
        const BatteryState s = State();
        return s == BatteryState::Low || s == BatteryState::Critical;
    }
    // On USB power (charging or full): never an idle power-off.
    bool External() const { return source_ == Source::Charging || source_ == Source::Full; }
    int Percent() const { return pct_; }  // last valid reading, -1 if none

private:
    enum class Source : uint8_t { Unknown, Battery, Charging, Full };
    enum class Band : uint8_t { Normal, Low, Critical };

    static bool Reached(uint32_t now_ms, uint32_t at_ms) {
        return static_cast<int32_t>(now_ms - at_ms) >= 0;
    }
    Band Wanted(int pct) const;
    void Schedule(BatteryReaction kind, uint32_t due_ms);

    Source source_ = Source::Unknown;
    Source source_candidate_ = Source::Unknown;
    uint8_t source_count_ = 0;
    Band band_ = Band::Normal;
    Band band_candidate_ = Band::Normal;
    uint8_t band_count_ = 0;
    Band announced_ = Band::Normal;  // last band an event was sent for
    bool have_level_ = false;
    int pct_ = -1;
    uint32_t critical_ms_ = 0;

    BatteryReaction next_ = BatteryReaction::None;
    uint32_t due_ms_ = 0;
};

const char* BatteryStateName(BatteryState state);
const char* BatteryEventName(BatteryEvent event);
const char* BatteryReactionName(BatteryReaction reaction);

// One word for board tasks (atomic<uint32_t>): bits 0-7 percent (0xFF =
// none), bit 8 charging, bit 9 on battery, bit 10 always set (a packed
// reading is never 0, so 0 can mean "no reading"). Bits 16-31 are free.
uint32_t PackBatteryReading(const BatteryReading& reading);
BatteryReading UnpackBatteryReading(uint32_t bits);

}  // namespace kid_app
