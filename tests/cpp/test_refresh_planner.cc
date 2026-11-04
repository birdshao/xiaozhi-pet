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

    static_assert(RefreshPlanner::kDefaultSoftPartials == 40, "5x the V2 budget of 8");
    static_assert(RefreshPlanner::kDefaultHardPartials > RefreshPlanner::kDefaultSoftPartials,
                  "hard budget above soft");

    // Nothing shown yet: the first frame is always a full refresh.
    {
        RefreshPlanner planner(2, 4);
        assert(planner.Plan(home_story) == RefreshMode::Full);
        planner.Completed(home_story, RefreshMode::Full, true);
        assert(!planner.NeedsCleanup());
        // Highlight and expression changes on the same scene are partial.
        assert(planner.Plan(home_feed) == RefreshMode::Partial);
        planner.Completed(home_feed, RefreshMode::Partial, true);
        assert(planner.RailPartials() == 1 && planner.PetPartials() == 0);
        assert(planner.Plan(home_happy) == RefreshMode::Partial);
        planner.Completed(home_happy, RefreshMode::Partial, true);
        assert(planner.RailPartials() == 1 && planner.PetPartials() == 1);
        assert(!planner.NeedsCleanup());
        // Past the soft budget a region asks for cleanup, but changes stay
        // partial: no 4 s flash while a child turns the dial.
        planner.Completed(home_story, RefreshMode::Partial, true);
        assert(planner.RailPartials() == 2 && planner.NeedsCleanup());
        assert(planner.Plan(home_feed) == RefreshMode::Partial);
        planner.Completed(home_feed, RefreshMode::Partial, true);
        planner.Completed(home_story, RefreshMode::Partial, true);
        assert(planner.RailPartials() == 4);
        // Hard budget: the next rail change is a full refresh ...
        assert(planner.Plan(home_feed) == RefreshMode::Full);
        // ... but the pet region has its own budget.
        const SceneModel story_happy{SceneKind::Home, Emotion::Happy, 1, 1};
        assert(planner.Plan(story_happy) == RefreshMode::Partial);
        // A re-draw of the same picture touches neither region.
        planner.Completed(home_story, RefreshMode::Partial, true);
        assert(planner.RailPartials() == 4 && planner.PetPartials() == 2);
        // A full refresh clears every region.
        planner.Completed(home_feed, RefreshMode::Full, true);
        assert(planner.RailPartials() == 0 && planner.PetPartials() == 0);
        assert(!planner.NeedsCleanup());
        assert(planner.Plan(home_story) == RefreshMode::Partial);
    }

    // Animation poses count against the pet region only.
    {
        RefreshPlanner planner(3, 5);
        planner.Completed(home_happy, RefreshMode::Full, true);
        SceneModel frame = home_happy;
        for (ScenePose pose : {ScenePose::Hearts1, ScenePose::Hearts2, ScenePose::Still}) {
            frame.pose = pose;
            assert(planner.Plan(frame) == RefreshMode::Partial);
            planner.Completed(frame, RefreshMode::Partial, true);
        }
        assert(planner.PetPartials() == 3 && planner.RailPartials() == 0);
        assert(planner.NeedsCleanup());
        // Cleanup: the owner forces the next refresh of the same picture full.
        planner.ForceFull();
        assert(planner.Plan(frame) == RefreshMode::Full);
        planner.Completed(frame, RefreshMode::Full, true);
        assert(!planner.NeedsCleanup());
        // Sleep: Zzz1 -> Zzz2 on the sleep scene is a partial refresh.
        SceneModel sleep{SceneKind::Sleep, Emotion::Sleepy, 7, 1, ScenePose::Zzz1};
        assert(planner.Plan(sleep) == RefreshMode::Full);
        planner.Completed(sleep, RefreshMode::Full, true);
        sleep.pose = ScenePose::Zzz2;
        assert(planner.Plan(sleep) == RefreshMode::Partial);
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
        assert(!planner.NeedsCleanup());
    }

    // When another owner (XiaoZhi LVGL) drew in between, start over.
    {
        RefreshPlanner planner;
        planner.Completed(home_story, RefreshMode::Full, true);
        planner.ForceFull();
        assert(planner.Plan(home_feed) == RefreshMode::Full);
    }

    std::cout << "PASS: refresh planner keeps per-region budgets and defers cleanup\n";
}
