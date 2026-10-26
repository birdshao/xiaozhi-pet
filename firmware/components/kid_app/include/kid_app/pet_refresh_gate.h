#pragma once

#include <cstdint>
#include "kid_app/scene_model.h"

namespace kid_app {

// Single-owner scheduler. No actual LCD/EPD calls from event callback.
class PetRefreshGate {
public:
    explicit PetRefreshGate(uint64_t minimum_interval_ms = 5000)
        : interval_ms_(minimum_interval_ms) {}
    bool Request(const SceneModel& scene);
    bool Acquire(uint64_t now_ms, SceneModel* scene);
    bool Complete(bool success, uint64_t finished_at_ms);
    // Single owner only. Reset when an external LVGL page replaces pet EPD.
    void Reset();

    bool HasPending() const { return pending_; }
    bool Busy() const { return busy_; }
    bool HasDisplayed() const { return has_displayed_; }
    const SceneModel& LastDisplayed() const { return displayed_; }

private:
    static bool Equal(const SceneModel& left, const SceneModel& right);
    uint64_t interval_ms_ = 5000;
    uint64_t last_success_ms_ = 0;
    bool has_displayed_ = false;
    bool pending_ = false;
    bool busy_ = false;
    SceneModel requested_{};
    SceneModel in_flight_{};
    SceneModel displayed_{};
};

}  // namespace kid_app
