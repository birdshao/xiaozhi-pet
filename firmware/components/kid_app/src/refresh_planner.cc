#include "kid_app/refresh_planner.h"

namespace kid_app {

RefreshMode RefreshPlanner::Plan(const SceneModel& next) const {
    if (!has_shown_ || next.kind != shown_.kind) return RefreshMode::Full;
    // Book pages are content: always the clean full waveform.
    if (next.kind == SceneKind::Reader) return RefreshMode::Full;
    if (partials_ >= max_partials_) return RefreshMode::Full;
    return RefreshMode::Partial;
}

void RefreshPlanner::Completed(const SceneModel& shown, RefreshMode mode, bool success) {
    if (!success) {
        // Panel contents are unknown after a failed update.
        has_shown_ = false;
        partials_ = 0;
        return;
    }
    has_shown_ = true;
    shown_ = shown;
    if (mode == RefreshMode::Full) partials_ = 0;
    else if (partials_ < UINT8_MAX) ++partials_;
}

}  // namespace kid_app
