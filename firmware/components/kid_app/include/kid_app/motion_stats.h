#pragma once

#include <cstdint>

namespace kid_app {

// Windowed accelerometer summary for V1 gesture-threshold measurements.
// Input: QMI8658 raw counts at +/-4 g (8192 LSB per g). No raw samples
// are retained beyond the running sums of the current window.
class MotionStats {
public:
    static constexpr int32_t kCountsPerG = 8192;

    struct Summary {
        uint16_t samples = 0;
        int16_t mean_x_mg = 0;
        int16_t mean_y_mg = 0;
        int16_t mean_z_mg = 0;
        uint16_t min_mag_mg = 0;
        uint16_t max_mag_mg = 0;
        uint16_t max_dev_mg = 0;  // max | |a| - 1 g |
    };

    void Add(int16_t x, int16_t y, int16_t z);
    // Returns the current window and starts a new one.
    Summary TakeSummary();

private:
    static int32_t ToMilliG(int64_t counts);
    int64_t sum_x_ = 0;
    int64_t sum_y_ = 0;
    int64_t sum_z_ = 0;
    uint16_t samples_ = 0;
    uint16_t min_mag_mg_ = UINT16_MAX;
    uint16_t max_mag_mg_ = 0;
    uint16_t max_dev_mg_ = 0;
};

}  // namespace kid_app
