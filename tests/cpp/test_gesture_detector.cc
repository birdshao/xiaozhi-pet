#include "kid_app/gesture_detector.h"

#include <cassert>
#include <iostream>
#include <vector>

using namespace kid_app;

namespace {
// 500 ms windows shaped like the V1 round 3 measurements (screen up:
// z ~ -983 mg; see docs/hardware/V1_HARDWARE_ACCEPTANCE.md).
MotionStats::Summary W(int16_t x, int16_t y, int16_t z, uint16_t dev) {
    MotionStats::Summary s;
    s.samples = 25;
    s.mean_x_mg = x;
    s.mean_y_mg = y;
    s.mean_z_mg = z;
    s.max_dev_mg = dev;
    return s;
}
const MotionStats::Summary kStill = W(16, 55, -983, 17);

std::vector<Gesture> Run(GestureDetector& d, const std::vector<MotionStats::Summary>& ws,
                         uint32_t start_ms = 0) {
    std::vector<Gesture> out;
    uint32_t t = start_ms;
    for (const auto& w : ws) {
        const Gesture g = d.OnWindow(w, t);
        if (g != Gesture::None) out.push_back(g);
        t += 500;
    }
    return out;
}

std::vector<MotionStats::Summary> Still(int n) { return std::vector<MotionStats::Summary>(n, kStill); }

std::vector<MotionStats::Summary> operator+(std::vector<MotionStats::Summary> a,
                                            const std::vector<MotionStats::Summary>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}
}  // namespace

int main() {
    // Lying still for a minute: nothing (V1: static noise 16-19 mg).
    {
        GestureDetector d;
        assert(Run(d, Still(120)).empty());
    }

    // The first window after (re)configuration is discarded (V1 saw one
    // 6,863 mg outlier right after enabling the accelerometer).
    {
        GestureDetector d;
        assert(Run(d, std::vector<MotionStats::Summary>{W(-137, -96, -1099, 5863)} + Still(10)).empty());
    }

    // Picked up after resting: one Pickup; holding it is not another one.
    {
        GestureDetector d;
        const auto events = Run(d, Still(6) + std::vector<MotionStats::Summary>{
                                       W(39, 206, -960, 233), W(49, 552, -784, 239),
                                       W(-113, 537, -838, 13), W(-110, 546, -835, 27)});
        assert(events == std::vector<Gesture>{Gesture::Pickup});
    }

    // A slow tilt with little jerk still counts as a pickup.
    {
        GestureDetector d;
        const auto events = Run(d, Still(6) + std::vector<MotionStats::Summary>{
                                       W(16, 420, -890, 90)});
        assert(events == std::vector<Gesture>{Gesture::Pickup});
    }

    // Gentle shake (V1: 370-420 mg) after resting: Pickup, then GentleShake.
    {
        GestureDetector d;
        const auto events = Run(d, Still(6) + std::vector<MotionStats::Summary>{
                                       W(-132, 128, -1005, 420), W(-182, 155, -898, 366),
                                       W(-183, 134, -991, 383), W(-204, 173, -956, 69)});
        assert((events == std::vector<Gesture>{Gesture::Pickup, Gesture::GentleShake}));
    }

    // Hard shake (V1: 1330-1570 mg) wins over everything, and is rate limited.
    {
        GestureDetector d;
        const auto events = Run(d, Still(6) + std::vector<MotionStats::Summary>{
                                       W(4, 222, -1117, 1569), W(88, 208, -1101, 1329),
                                       W(88, 50, -968, 141)});
        assert(events == std::vector<Gesture>{Gesture::HardShake});
        // After the cooldown a new hard shake is reported again.
        const auto later = Run(d, Still(6) + std::vector<MotionStats::Summary>{
                                      W(4, 222, -1117, 1500)}, 10000);
        assert(later == std::vector<Gesture>{Gesture::HardShake});
    }

    // Face down for 1 s (z ~ +1010), then turned back: FaceDown, FaceUp.
    // No pickup is reported while lying face down.
    {
        GestureDetector d;
        const auto events = Run(d, Still(6) + std::vector<MotionStats::Summary>{
                                       W(-44, 408, 807, 247), W(3, 26, 1010, 20),
                                       W(7, 19, 1010, 12),   W(7, 18, 1010, 12),
                                       W(-133, 527, 751, 144), W(98, 324, -934, 56),
                                       W(17, 119, -966, 30), W(13, 50, -983, 18)});
        assert((events == std::vector<Gesture>{Gesture::FaceDown, Gesture::FaceUp}));
    }

    // Disabled by a parent: nothing at all.
    {
        GestureDetector d;
        d.SetEnabled(false);
        assert(Run(d, Still(6) + std::vector<MotionStats::Summary>{W(4, 222, -1117, 1569)}).empty());
    }

    // Empty windows (I2C read failures) are ignored.
    {
        GestureDetector d;
        MotionStats::Summary empty;
        assert(d.OnWindow(empty, 0) == Gesture::None);
    }

    std::cout << "PASS: gestures from V1 accelerometer windows (pickup, shake, face down)\n";
}
