#pragma once

#include <cstdint>

namespace kid_app {

// V4-03 (PRD §5.1): when the room gets hot or cold the pet says one short,
// child-friendly life tip (drink water / put on a coat). Pure logic, fed
// with SHTC3 temperature readings; the board shows the bubble and plays
// the reviewed Personality line (PhraseContext::HotTip / ColdTip).
//
//   Normal --held >= kHoldMs at >= 28.0 C--> Hot  (Hot -> Normal below 26.0)
//   Normal --held >= kHoldMs at <= 16.0 C--> Cold (Cold -> Normal above 18.0)
//
// The bands overlap (hysteresis) and a crossing must hold for kHoldMs of
// readings, so sensor noise and a warm hand on the board never trigger a
// tip. The reading is the room temperature: the SHTC3 sits next to a
// charging board, so `offset_c10` (default 0: the V1-07 warm-up deviation
// table is still to be measured) is subtracted before the bands apply.
//
// A tip is due once on entering a zone and again every kRepeatMs while the
// zone lasts; never in quiet hours (a tip that waited longer than
// kPendingMs is dropped, so there is no stale "hot!" at breakfast), never
// more than kDailyLimit tips per local day (any kind), and only when the
// board says the moment is right (`allowed`: Pet home, awake, no voice).
//
// Single owner (board input task); uint32 ms clock, wrap-safe. Timers go
// through SelfTestClock::Scaled so a scripted trial can run them faster.
enum class EnvTip : uint8_t { None, Hot, Cold };
enum class EnvZone : uint8_t { Unknown, Normal, Hot, Cold };

class EnvTipPolicy {
public:
    static constexpr int kHotOnC10 = 280;
    static constexpr int kHotOffC10 = 260;
    static constexpr int kColdOnC10 = 160;
    static constexpr int kColdOffC10 = 180;
    static constexpr uint32_t kHoldMs = 60u * 1000;
    static constexpr uint32_t kRepeatMs = 2u * 60 * 60 * 1000;
    static constexpr uint32_t kPendingMs = 30u * 60 * 1000;
    static constexpr uint8_t kDailyLimit = 3;

    explicit EnvTipPolicy(int offset_c10 = 0) : offset_c10_(offset_c10) {}

    // One reading (ok=false: the sensor failed; the candidate restarts).
    // Returns the tip that became pending, if any.
    EnvTip Observe(bool ok, int temp_c10, uint32_t now_ms);
    // The tip to give now, if any. `day` is the local civil day number
    // (0 = clock not set: the limit then counts per boot). A tip is
    // consumed when returned. While not allowed it stays pending.
    EnvTip Take(bool quiet, bool allowed, uint32_t day, uint32_t now_ms);

    EnvZone Zone() const { return zone_; }
    EnvTip Pending() const { return pending_; }
    uint8_t TodayCount() const { return today_count_; }
    int RoomC10() const { return room_c10_; }  // last compensated reading

private:
    static bool Reached(uint32_t now_ms, uint32_t at_ms) {
        return static_cast<int32_t>(now_ms - at_ms) >= 0;
    }
    EnvZone Wanted(int room_c10) const;
    void Raise(EnvTip tip, uint32_t now_ms);

    int offset_c10_;
    EnvZone zone_ = EnvZone::Unknown;
    EnvZone candidate_ = EnvZone::Unknown;
    uint32_t candidate_ms_ = 0;
    int room_c10_ = 0;

    EnvTip pending_ = EnvTip::None;
    uint32_t pending_ms_ = 0;
    uint32_t repeat_ms_ = 0;  // next reminder while the zone lasts
    uint32_t day_ = 0;
    uint8_t today_count_ = 0;
};

const char* EnvTipName(EnvTip tip);
const char* EnvZoneName(EnvZone zone);

}  // namespace kid_app
