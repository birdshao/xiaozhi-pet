#pragma once

#include "kid_app/motion_stats.h"

#include <cstdint>

namespace kid_app {

// Discrete motion events for the pet (PRD v0.2 §3, ADR-011). Hard shakes
// are reported so the pet can react gently; they must never be rewarded.
// TiltLeft/TiltRight/Level only come from TiltTracker (motion_game.h)
// while the V4-07 ball game runs; GestureDetector never reports them.
enum class Gesture : uint8_t {
    None, Pickup, GentleShake, HardShake, FaceDown, FaceUp, TiltLeft, TiltRight, Level
};

// Thresholds from the V1 round 3 accelerometer measurements (500 ms
// windows, +/-4 g; screen up reads z ~ -983 mg on this board).
struct GestureConfig {
    uint16_t still_dev_mg = 40;          // static noise was 16-19 mg
    uint8_t still_windows = 4;           // 2 s resting before a pickup counts
    uint16_t pickup_dev_mg = 150;        // lift transitions: 230-240 mg
    uint16_t pickup_tilt_mg = 300;       // or a slow tilt of the resting pose
    int16_t pickup_max_z_mg = -300;      // screen roughly up (not a flip)
    uint16_t gentle_dev_mg = 250;        // gentle shake: 370-420 mg
    uint8_t gentle_windows = 2;
    uint16_t hard_dev_mg = 1000;         // hard shake: 1330-1570 mg
    int16_t face_z_mg = 800;             // face down: z ~ +1010 mg
    uint8_t face_windows = 2;            // held 1 s
    uint32_t pickup_cooldown_ms = 5000;
    uint32_t shake_cooldown_ms = 2000;
};

// Single owner; at most one gesture per window; uint32 ms, wrap-safe.
class GestureDetector {
public:
    explicit GestureDetector(GestureConfig config = {}) : config_(config) {}

    Gesture OnWindow(const MotionStats::Summary& window, uint32_t now_ms);
    void SetEnabled(bool enabled);
    void Reset();

private:
    bool CooledDown(uint32_t last_ms, bool ever, uint32_t cooldown, uint32_t now) const;

    GestureConfig config_;
    bool enabled_ = true;
    bool discard_next_ = true;  // first window after (re)start is unreliable
    uint8_t still_count_ = 0;
    int16_t rest_x_ = 0, rest_y_ = 0, rest_z_ = 0;
    uint8_t gentle_count_ = 0;
    uint8_t down_count_ = 0, up_count_ = 0;
    bool face_down_ = false;
    bool pickup_ever_ = false, gentle_ever_ = false, hard_ever_ = false;
    uint32_t pickup_ms_ = 0, gentle_ms_ = 0, hard_ms_ = 0;
};

}  // namespace kid_app
