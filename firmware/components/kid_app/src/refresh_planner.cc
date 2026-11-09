#include "kid_app/refresh_planner.h"

namespace kid_app {

bool RefreshPlanner::TouchesRail(const SceneModel& a, const SceneModel& b) {
    return a.home_selection != b.home_selection ||
           a.game.RailDiffers(b.game) ||  // game answer cards share the rail budget
           a.care.RailDiffers(b.care);    // so do the V8 care cards
}

bool RefreshPlanner::TouchesPet(const SceneModel& a, const SceneModel& b) {
    return a.emotion != b.emotion || a.pose != b.pose || a.book_page != b.book_page ||
           a.parent != b.parent ||  // parent menu rows share the pet budget
           a.game.QuestionDiffers(b.game) ||  // so does the game question area
           a.care.TopDiffers(b.care) ||       // and the V8 care picture
           a.look != b.look;                  // and the V8 home look (clothes, needs)
}

bool RefreshPlanner::TouchesStatus(const SceneModel& a, const SceneModel& b) {
    return a.status != b.status;
}

RefreshMode RefreshPlanner::Plan(const SceneModel& next) const {
    if (!has_shown_ || next.kind != shown_.kind) return RefreshMode::Full;
    // Book pages are content: always the clean full waveform.
    if (next.kind == SceneKind::Reader) return RefreshMode::Full;
    // V9 bookshelf: moving the ring is partial, the next row of covers is clean.
    if (next.kind == SceneKind::Shelf && next.shelf.first != shown_.shelf.first)
        return RefreshMode::Full;
    if (TouchesRail(shown_, next) && rail_ >= hard_) return RefreshMode::Full;
    if (TouchesPet(shown_, next) && pet_ >= hard_) return RefreshMode::Full;
    if (TouchesStatus(shown_, next) && status_ >= status_hard_) return RefreshMode::Full;
    return RefreshMode::Partial;
}

void RefreshPlanner::Completed(const SceneModel& shown, RefreshMode mode, bool success) {
    if (!success) {
        // Panel contents are unknown after a failed update.
        has_shown_ = false;
        rail_ = pet_ = status_ = 0;
        return;
    }
    if (mode == RefreshMode::Full) {
        rail_ = pet_ = status_ = 0;
    } else if (has_shown_) {
        if (TouchesRail(shown_, shown) && rail_ < UINT8_MAX) ++rail_;
        if (TouchesPet(shown_, shown) && pet_ < UINT8_MAX) ++pet_;
        if (TouchesStatus(shown_, shown) && status_ < UINT8_MAX) ++status_;
    }
    has_shown_ = true;
    shown_ = shown;
}

bool RefreshPlanner::NeedsCleanup() const {
    return has_shown_ && (rail_ >= soft_ || pet_ >= soft_ || status_ >= status_soft_);
}

}  // namespace kid_app
