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

    // Status band (V2-05): its own counter and a larger default budget (the
    // clock ticks every minute); status changes count against nothing else.
    {
        static_assert(RefreshPlanner::kDefaultStatusSoftPartials >=
                          RefreshPlanner::kDefaultSoftPartials, "about an hour of clock ticks");
        static_assert(RefreshPlanner::kDefaultStatusHardPartials >
                          RefreshPlanner::kDefaultStatusSoftPartials, "hard budget above soft");
        RefreshPlanner planner(2, 4, 3, 5);
        SceneModel shown = home_story;
        planner.Completed(shown, RefreshMode::Full, true);
        auto tick = [&](uint8_t minute) {
            SceneModel next = shown;
            next.status.hour = 7;
            next.status.minute = minute;
            assert(planner.Plan(next) == RefreshMode::Partial);
            planner.Completed(next, RefreshMode::Partial, true);
            shown = next;
        };
        tick(0);
        tick(1);
        assert(planner.StatusPartials() == 2 && !planner.NeedsCleanup());
        tick(2);
        assert(planner.StatusPartials() == 3 && planner.NeedsCleanup());
        assert(planner.RailPartials() == 0 && planner.PetPartials() == 0);
        // Rail and pet changes under an unchanged status leave it alone.
        SceneModel feed = shown;
        feed.home_selection = 3;
        planner.Completed(feed, RefreshMode::Partial, true);
        assert(planner.StatusPartials() == 3 && planner.RailPartials() == 1);
        shown = feed;
        tick(3);
        tick(4);
        assert(planner.StatusPartials() == 5);
        // Hard budget: the next status change is full; the pet's is not.
        SceneModel next = shown;
        next.status.minute = 5;
        assert(planner.Plan(next) == RefreshMode::Full);
        SceneModel happy = shown;
        happy.emotion = Emotion::Happy;
        assert(planner.Plan(happy) == RefreshMode::Partial);
        planner.Completed(next, RefreshMode::Full, true);
        assert(planner.StatusPartials() == 0 && planner.RailPartials() == 0);
        assert(!planner.NeedsCleanup());
        // A failed refresh starts every region over.
        shown = next;
        tick(6);
        SceneModel failed = shown;
        failed.status.minute = 7;
        planner.Completed(failed, RefreshMode::Partial, false);
        assert(planner.StatusPartials() == 0 && planner.Plan(failed) == RefreshMode::Full);
        // Default budgets.
        RefreshPlanner defaults;
        defaults.Completed(home_story, RefreshMode::Full, true);
        SceneModel clock = home_story;
        for (int i = 0; i < RefreshPlanner::kDefaultStatusSoftPartials; ++i) {
            clock.status.minute = static_cast<uint8_t>(i % 2);
            defaults.Completed(clock, RefreshMode::Partial, true);
        }
        assert(defaults.NeedsCleanup() && defaults.Plan(home_feed) == RefreshMode::Partial);
    }

    // V3-06 parent menu: entering is full, row changes are partial and use
    // the pet budget; leaving to Home is full again.
    {
        RefreshPlanner planner(2, 3);
        SceneModel menu{SceneKind::Parent};
        assert(planner.Plan(menu) == RefreshMode::Full);
        planner.Completed(menu, RefreshMode::Full, true);
        SceneModel moved = menu;
        moved.parent.cursor = 1;
        assert(planner.Plan(moved) == RefreshMode::Partial);
        planner.Completed(moved, RefreshMode::Partial, true);
        assert(planner.PetPartials() == 1);
        SceneModel edited = moved;
        edited.parent.editing = true;
        planner.Completed(edited, RefreshMode::Partial, true);
        assert(planner.PetPartials() == 2 && planner.NeedsCleanup());
        SceneModel saved = edited;
        saved.parent.settings.volume_cap_pct = 90;
        planner.Completed(saved, RefreshMode::Partial, true);
        SceneModel again = saved;
        again.parent.cursor = 2;
        assert(planner.Plan(again) == RefreshMode::Full);  // hard budget
        assert(planner.Plan(SceneModel{}) == RefreshMode::Full);
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
