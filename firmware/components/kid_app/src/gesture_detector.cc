#include "kid_app/gesture_detector.h"

namespace kid_app {

namespace {
int Abs(int v) { return v < 0 ? -v : v; }
}  // namespace

void GestureDetector::SetEnabled(bool enabled) {
    if (enabled && !enabled_) Reset();
    enabled_ = enabled;
}

void GestureDetector::Reset() {
    const bool enabled = enabled_;
    const GestureConfig config = config_;
    *this = GestureDetector(config);
    enabled_ = enabled;
}

bool GestureDetector::CooledDown(uint32_t last_ms, bool ever, uint32_t cooldown,
                                 uint32_t now) const {
    return !ever || now - last_ms >= cooldown;
}

Gesture GestureDetector::OnWindow(const MotionStats::Summary& w, uint32_t now_ms) {
    if (!enabled_ || w.samples == 0) return Gesture::None;
    if (discard_next_) {
        discard_next_ = false;
        return Gesture::None;
    }
    const bool rested = still_count_ >= config_.still_windows;
    const bool still = w.max_dev_mg < config_.still_dev_mg;

    // Bookkeeping that every branch below relies on.
    if (w.mean_z_mg >= config_.face_z_mg) {
        if (down_count_ < UINT8_MAX) ++down_count_;
    } else {
        down_count_ = 0;
    }
    if (w.mean_z_mg <= -config_.face_z_mg) {
        if (up_count_ < UINT8_MAX) ++up_count_;
    } else {
        up_count_ = 0;
    }
    const bool gentle_window = w.max_dev_mg >= config_.gentle_dev_mg &&
                               w.max_dev_mg < config_.hard_dev_mg;
    gentle_count_ = gentle_window ? static_cast<uint8_t>(gentle_count_ + 1) : 0;
    const bool tilted = Abs(w.mean_x_mg - rest_x_) > config_.pickup_tilt_mg ||
                        Abs(w.mean_y_mg - rest_y_) > config_.pickup_tilt_mg ||
                        Abs(w.mean_z_mg - rest_z_) > config_.pickup_tilt_mg;
    if (still) {
        if (still_count_ < UINT8_MAX) ++still_count_;
        rest_x_ = w.mean_x_mg;
        rest_y_ = w.mean_y_mg;
        rest_z_ = w.mean_z_mg;
    } else {
        still_count_ = 0;
    }

    // Priority: hard shake, face down/up, gentle shake, pickup.
    if (w.max_dev_mg >= config_.hard_dev_mg) {
        if (!CooledDown(hard_ms_, hard_ever_, config_.shake_cooldown_ms, now_ms))
            return Gesture::None;
        hard_ever_ = true;
        hard_ms_ = now_ms;
        return Gesture::HardShake;
    }
    if (!face_down_ && down_count_ >= config_.face_windows) {
        face_down_ = true;
        return Gesture::FaceDown;
    }
    if (face_down_) {
        if (up_count_ >= config_.face_windows) {
            face_down_ = false;
            return Gesture::FaceUp;
        }
        return Gesture::None;  // nothing else while lying face down
    }
    if (gentle_count_ >= config_.gentle_windows &&
        CooledDown(gentle_ms_, gentle_ever_, config_.shake_cooldown_ms, now_ms)) {
        gentle_ever_ = true;
        gentle_ms_ = now_ms;
        return Gesture::GentleShake;
    }
    if (rested && !still && w.mean_z_mg <= config_.pickup_max_z_mg &&
        (w.max_dev_mg >= config_.pickup_dev_mg || tilted) &&
        CooledDown(pickup_ms_, pickup_ever_, config_.pickup_cooldown_ms, now_ms)) {
        pickup_ever_ = true;
        pickup_ms_ = now_ms;
        return Gesture::Pickup;
    }
    return Gesture::None;
}

}  // namespace kid_app
