#include "kid_app/motion_stats.h"

namespace kid_app {

namespace {
uint32_t IntegerSqrt(uint64_t value) {
    uint64_t low = 0, high = 1ULL << 32;
    while (low + 1 < high) {
        const uint64_t mid = (low + high) / 2;
        if (mid * mid <= value) low = mid;
        else high = mid;
    }
    return static_cast<uint32_t>(low);
}
}  // namespace

int32_t MotionStats::ToMilliG(int64_t counts) {
    return static_cast<int32_t>(counts * 1000 / kCountsPerG);
}

void MotionStats::Add(int16_t x, int16_t y, int16_t z) {
    const int64_t sq = static_cast<int64_t>(x) * x + static_cast<int64_t>(y) * y +
                       static_cast<int64_t>(z) * z;
    const int32_t mag = ToMilliG(IntegerSqrt(static_cast<uint64_t>(sq)));
    const uint16_t mag_mg = static_cast<uint16_t>(mag > UINT16_MAX ? UINT16_MAX : mag);
    const int32_t dev = mag - 1000;
    const uint16_t dev_mg = static_cast<uint16_t>(dev < 0 ? -dev : dev);

    sum_x_ += x;
    sum_y_ += y;
    sum_z_ += z;
    if (samples_ < UINT16_MAX) ++samples_;
    if (mag_mg < min_mag_mg_) min_mag_mg_ = mag_mg;
    if (mag_mg > max_mag_mg_) max_mag_mg_ = mag_mg;
    if (dev_mg > max_dev_mg_) max_dev_mg_ = dev_mg;
}

MotionStats::Summary MotionStats::TakeSummary() {
    Summary out;
    if (samples_ > 0) {
        out.samples = samples_;
        out.mean_x_mg = static_cast<int16_t>(ToMilliG(sum_x_ / samples_));
        out.mean_y_mg = static_cast<int16_t>(ToMilliG(sum_y_ / samples_));
        out.mean_z_mg = static_cast<int16_t>(ToMilliG(sum_z_ / samples_));
        out.min_mag_mg = min_mag_mg_;
        out.max_mag_mg = max_mag_mg_;
        out.max_dev_mg = max_dev_mg_;
    }
    *this = MotionStats{};
    return out;
}

}  // namespace kid_app
