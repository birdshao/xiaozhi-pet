#include "kid_app/prompt_scheduler.h"

namespace kid_app {

void PromptScheduler::OnSelect(uint8_t index, uint32_t now_ms) {
    if (!enabled_) return;
    index_ = index;
    due_ms_ = now_ms + kSettleMs;  // restart on every move
    pending_ = true;
}

void PromptScheduler::SetEnabled(bool enabled) {
    enabled_ = enabled;
    if (!enabled) pending_ = false;
}

bool PromptScheduler::Tick(uint32_t now_ms, PromptRequest* out) {
    if (!pending_ || !out || static_cast<int32_t>(now_ms - due_ms_) < 0) return false;
    pending_ = false;
    *out = {index_, ++token_, 0};  // an option name, not a pet line
    return true;
}

}  // namespace kid_app
