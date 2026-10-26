#include "kid_app/pet_refresh_gate.h"

namespace kid_app {

bool PetRefreshGate::Equal(const SceneModel& a, const SceneModel& b) {
    return a.kind == b.kind && a.emotion == b.emotion &&
           a.book_page == b.book_page;
}

bool PetRefreshGate::Request(const SceneModel& scene) {
    requested_ = scene;
    if (busy_) {
        // Latest request equal to in-flight frame cancels extra refresh.
        pending_ = !Equal(requested_, in_flight_);
    } else {
        pending_ = !has_displayed_ || !Equal(requested_, displayed_);
    }
    return pending_;
}

bool PetRefreshGate::Acquire(uint64_t now_ms, SceneModel* scene) {
    if (!scene || busy_ || !pending_) return false;
    if (has_displayed_ &&
        (now_ms < last_success_ms_ ||
         now_ms - last_success_ms_ < interval_ms_)) return false;
    in_flight_ = requested_;
    busy_ = true;
    pending_ = false;
    *scene = in_flight_;
    return true;
}

bool PetRefreshGate::Complete(bool success, uint64_t finished_at_ms) {
    if (!busy_) return false;
    busy_ = false;
    if (success) {
        displayed_ = in_flight_;
        has_displayed_ = true;
        last_success_ms_ = finished_at_ms;
        if (pending_ && Equal(requested_, displayed_)) pending_ = false;
    } else {
        // Failed physical EPD operation never advances cooldown. Requeue.
        if (!pending_) {
            requested_ = in_flight_;
            pending_ = true;
        }
    }
    return true;
}

}  // namespace kid_app
