#pragma once

#include "kid_app/scene_model.h"

#include <cstdint>

namespace kid_app {

// Full = full waveform, both controller RAMs rewritten (~4.3 s on V1
// hardware, no ghosting). Partial = partial waveform on the whole buffer,
// used for highlight, expression and animation changes on the same scene.
enum class RefreshMode : uint8_t { Full, Partial };

// Partial refreshes leave ghosting only where pixels change, so the budget
// is kept per screen region: the action rail (selection, y >= 380) and the
// pet area above it (expression and animation poses). SceneComposer tests
// guarantee each kind of change stays inside its region.
//
// Past the soft budget a region "needs cleanup": the owner runs one full
// refresh once nobody is interacting (NeedsCleanup + ForceFull). Only past
// the hard budget does the next change itself become a full refresh, so a
// child using the dial is not interrupted by a 4 s flash.
//
// Single owner: the ePaper worker task. No hardware access.
class RefreshPlanner {
public:
    // V2 used one shared budget of 8; on-device use showed plenty of headroom.
    static constexpr uint8_t kDefaultSoftPartials = 40;
    static constexpr uint8_t kDefaultHardPartials = 60;

    explicit RefreshPlanner(uint8_t soft_partials = kDefaultSoftPartials,
                            uint8_t hard_partials = kDefaultHardPartials)
        : soft_(soft_partials), hard_(hard_partials < soft_partials ? soft_partials : hard_partials) {}

    RefreshMode Plan(const SceneModel& next) const;
    void Completed(const SceneModel& shown, RefreshMode mode, bool success);
    // Another owner (XiaoZhi LVGL) drew on the panel, or a cleanup is due:
    // the next refresh is full.
    void ForceFull() { has_shown_ = false; }
    // Some region passed its soft budget; a full refresh would clear it.
    bool NeedsCleanup() const;
    uint8_t RailPartials() const { return rail_; }
    uint8_t PetPartials() const { return pet_; }

private:
    static bool TouchesRail(const SceneModel& a, const SceneModel& b);
    static bool TouchesPet(const SceneModel& a, const SceneModel& b);
    uint8_t soft_;
    uint8_t hard_;
    uint8_t rail_ = 0;
    uint8_t pet_ = 0;
    bool has_shown_ = false;
    SceneModel shown_{};
};

}  // namespace kid_app
