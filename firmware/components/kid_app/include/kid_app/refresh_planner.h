#pragma once

#include "kid_app/scene_model.h"

#include <cstdint>

namespace kid_app {

// Full = full waveform, both controller RAMs rewritten (~4.3 s on V1
// hardware, no ghosting). Partial = partial waveform on the whole buffer,
// used for highlight and expression changes on the same scene.
enum class RefreshMode : uint8_t { Full, Partial };

// Single owner: the ePaper worker task. No hardware access.
class RefreshPlanner {
public:
    static constexpr uint8_t kDefaultMaxPartials = 8;

    explicit RefreshPlanner(uint8_t max_partials = kDefaultMaxPartials)
        : max_partials_(max_partials) {}

    RefreshMode Plan(const SceneModel& next) const;
    void Completed(const SceneModel& shown, RefreshMode mode, bool success);
    // Another owner (XiaoZhi LVGL) drew on the panel: start over with full.
    void ForceFull() { has_shown_ = false; }
    uint8_t PartialsSinceFull() const { return partials_; }

private:
    uint8_t max_partials_;
    uint8_t partials_ = 0;
    bool has_shown_ = false;
    SceneModel shown_{};
};

}  // namespace kid_app
