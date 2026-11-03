#include "kid_app/refresh_planner.h"

#include <cassert>
#include <iostream>

using namespace kid_app;

int main() {
    const SceneModel home_story{SceneKind::Home, Emotion::Neutral, 1, 1};
    const SceneModel home_feed{SceneKind::Home, Emotion::Neutral, 1, 3};
    const SceneModel home_happy{SceneKind::Home, Emotion::Happy, 1, 3};
    const SceneModel page1{SceneKind::Reader, Emotion::Curious, 1, 1};
    const SceneModel page2{SceneKind::Reader, Emotion::Curious, 2, 1};

    // Nothing shown yet: the first frame is always a full refresh.
    {
        RefreshPlanner planner(3);
        assert(planner.Plan(home_story) == RefreshMode::Full);
        planner.Completed(home_story, RefreshMode::Full, true);
        // Highlight and expression changes on the same scene are partial.
        assert(planner.Plan(home_feed) == RefreshMode::Partial);
        planner.Completed(home_feed, RefreshMode::Partial, true);
        assert(planner.Plan(home_happy) == RefreshMode::Partial);
        planner.Completed(home_happy, RefreshMode::Partial, true);
        assert(planner.Plan(home_feed) == RefreshMode::Partial);
        planner.Completed(home_feed, RefreshMode::Partial, true);
        // After max_partials a full refresh clears accumulated ghosting.
        assert(planner.PartialsSinceFull() == 3);
        assert(planner.Plan(home_story) == RefreshMode::Full);
        planner.Completed(home_story, RefreshMode::Full, true);
        assert(planner.PartialsSinceFull() == 0);
        assert(planner.Plan(home_feed) == RefreshMode::Partial);
    }

    // Scene changes and every book page are full refreshes (V1: 4.3 s,
    // no ghosting); a failed refresh forces the next one to be full.
    {
        RefreshPlanner planner;
        planner.Completed(home_story, RefreshMode::Full, true);
        assert(planner.Plan(page1) == RefreshMode::Full);
        planner.Completed(page1, RefreshMode::Full, true);
        assert(planner.Plan(page2) == RefreshMode::Full);
        planner.Completed(page2, RefreshMode::Full, true);
        assert(planner.Plan(home_story) == RefreshMode::Full);
        planner.Completed(home_story, RefreshMode::Full, true);
        assert(planner.Plan(home_feed) == RefreshMode::Partial);
        planner.Completed(home_feed, RefreshMode::Partial, false);
        assert(planner.Plan(home_feed) == RefreshMode::Full);
    }

    // When another owner (XiaoZhi LVGL) drew in between, start over.
    {
        RefreshPlanner planner;
        planner.Completed(home_story, RefreshMode::Full, true);
        planner.ForceFull();
        assert(planner.Plan(home_feed) == RefreshMode::Full);
    }

    std::cout << "PASS: refresh planner picks partial for highlights, full otherwise\n";
}
