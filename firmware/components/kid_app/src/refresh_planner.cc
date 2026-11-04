#include "kid_app/refresh_planner.h"

namespace kid_app {

bool RefreshPlanner::TouchesRail(const SceneModel& a, const SceneModel& b) {
    return a.home_selection != b.home_selection;
}

bool RefreshPlanner::TouchesPet(const SceneModel& a, const SceneModel& b) {
    return a.emotion != b.emotion || a.pose != b.pose || a.book_page != b.book_page;
}

RefreshMode RefreshPlanner::Plan(const SceneModel& next) const {
    if (!has_shown_ || next.kind != shown_.kind) return RefreshMode::Full;
    // Book pages are content: always the clean full waveform.
    if (next.kind == SceneKind::Reader) return RefreshMode::Full;
    if (TouchesRail(shown_, next) && rail_ >= hard_) return RefreshMode::Full;
    if (TouchesPet(shown_, next) && pet_ >= hard_) return RefreshMode::Full;
    return RefreshMode::Partial;
}

void RefreshPlanner::Completed(const SceneModel& shown, RefreshMode mode, bool success) {
    if (!success) {
        // Panel contents are unknown after a failed update.
        has_shown_ = false;
        rail_ = pet_ = 0;
        return;
    }
    if (mode == RefreshMode::Full) {
        rail_ = pet_ = 0;
    } else if (has_shown_) {
        if (TouchesRail(shown_, shown) && rail_ < UINT8_MAX) ++rail_;
        if (TouchesPet(shown_, shown) && pet_ < UINT8_MAX) ++pet_;
    }
    has_shown_ = true;
    shown_ = shown;
}

bool RefreshPlanner::NeedsCleanup() const {
    return has_shown_ && (rail_ >= soft_ || pet_ >= soft_);
}

}  // namespace kid_app
