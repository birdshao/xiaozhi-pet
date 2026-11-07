#include "kid_app/pet_refresh_gate.h"

#include <cassert>
#include <iostream>

using namespace kid_app;

int main() {
    const SceneModel neutral{SceneKind::Home, Emotion::Neutral, 1};
    const SceneModel happy{SceneKind::Home, Emotion::Happy, 1};
    const SceneModel sleepy{SceneKind::Home, Emotion::Sleepy, 1};
    const SceneModel page2{SceneKind::Reader, Emotion::Curious, 2};
    const SceneModel page3{SceneKind::Reader, Emotion::Curious, 3};
    PetRefreshGate gate(5000);
    SceneModel output{};
    assert(!gate.Acquire(0, &output));
    assert(!gate.Complete(true, 0));
    assert(gate.Request(neutral));
    assert(gate.Acquire(0, &output));
    assert(output.emotion == Emotion::Neutral);
    assert(!gate.Acquire(1, &output));
    assert(gate.Complete(true, 1200));
    assert(gate.HasDisplayed() && !gate.HasPending());
    assert(!gate.Request(neutral));
    assert(!gate.Acquire(9999, &output));

    assert(gate.Request(happy));
    assert(gate.Request(page2));
    assert(gate.Request(page3));
    assert(!gate.Acquire(6199, &output));
    assert(gate.Acquire(6200, &output));
    assert(output.kind == SceneKind::Reader && output.book_page == 3);

    assert(gate.Request(sleepy));
    assert(!gate.Request(page3)); // pending update cancelled
    assert(gate.Complete(true, 10000));
    assert(!gate.HasPending());

    assert(gate.Request(sleepy));
    assert(gate.Acquire(15000, &output));
    assert(gate.Request(happy));
    assert(gate.Complete(false, 16000));
    assert(gate.LastDisplayed().book_page == 3);
    assert(gate.Acquire(16000, &output));
    assert(output.emotion == Emotion::Happy);
    assert(gate.Complete(true, 17500));
    assert(!gate.Request(happy));
    assert(gate.Request(sleepy));
    assert(!gate.Acquire(20000, &output));
    assert(!gate.Acquire(100, &output));
    assert(gate.Acquire(22500, &output));
    assert(output.emotion == Emotion::Sleepy);
    assert(gate.Complete(true, 23000));

    // V8 care scenes: a moved card highlight, a reaction (hearts) or a
    // progress step is a different picture although kind / pose / page stay
    // the same. The 2026-11-07 board logs showed none of them being drawn
    // (no PET_DIAG_EPD between care start and care end).
    {
        PetRefreshGate care_gate(0);
        SceneModel care_scene{};
        care_scene.kind = SceneKind::Care;
        care_scene.care.kind = CareKind::Feed;
        care_scene.care.option_count = 3;
        assert(care_gate.Request(care_scene));
        assert(care_gate.Acquire(0, &output));
        assert(care_gate.Complete(true, 10));
        assert(!care_gate.Request(care_scene));
        SceneModel moved = care_scene;
        moved.care.highlight = 1;
        assert(care_gate.Request(moved));
        assert(care_gate.Acquire(20, &output));
        assert(output.care.highlight == 1);
        assert(care_gate.Complete(true, 30));
        SceneModel react = moved;
        react.care.phase = 1;
        react.care.hearts = 3;
        assert(care_gate.Request(react));
        assert(care_gate.Acquire(40, &output));
        assert(care_gate.Complete(true, 50));
        SceneModel step = react;
        step.care.progress = 2;
        assert(care_gate.Request(step));
    }

    // Re-entering pet mode after LVGL has redrawn requires a full paint
    // even if the desired pet scene did not change since prior session.
    gate.Reset();
    assert(!gate.HasDisplayed() && !gate.Busy());
    assert(gate.Request(sleepy));
    assert(gate.Acquire(23001, &output));
    assert(output.emotion == Emotion::Sleepy);
    assert(gate.Complete(true, 24000));

    // A home-rail highlight move is a real scene change even when the
    // pet's expression and page are the same.
    const SceneModel sleepy_feed{SceneKind::Home, Emotion::Sleepy, 1, 3};
    assert(gate.Request(sleepy_feed));
    assert(gate.Acquire(29000, &output));
    assert(output.home_selection == 3);
    assert(gate.Complete(true, 29100));
    assert(!gate.Request(sleepy_feed));

    // Animation pose is part of the scene (falling asleep starts at Zzz1).
    SceneModel zzz = sleepy_feed;
    zzz.pose = ScenePose::Zzz1;
    assert(gate.Request(zzz));
    assert(gate.Acquire(35000, &output));
    assert(output.pose == ScenePose::Zzz1);
    assert(gate.Complete(true, 35100));
    assert(!gate.Request(zzz));

    // The status band is part of the picture too (V2-05; the board fills
    // it in on the ePaper task, so published scenes leave it unknown).
    SceneModel clock = sleepy_feed;
    clock.status.hour = 7;
    clock.status.minute = 42;
    assert(gate.Request(clock));
    assert(gate.Acquire(41000, &output));
    assert(output.status == clock.status);
    assert(gate.Complete(true, 41100));
    assert(!gate.Request(clock));
    assert(gate.Request(sleepy_feed));
    // V3-06: two parent menu frames that differ only in the menu are
    // different scenes (the packed word does not carry the menu).
    {
        kid_app::PetRefreshGate menu_gate(0);
        kid_app::SceneModel a{kid_app::SceneKind::Parent};
        kid_app::SceneModel b = a;
        b.parent.settings.motion_enabled = false;
        assert(menu_gate.Request(a));
        kid_app::SceneModel got{};
        assert(menu_gate.Acquire(0, &got) && menu_gate.Complete(true, 1));
        assert(!menu_gate.Request(a));
        assert(menu_gate.Request(b));
    }

    std::cout << "PASS: ePaper cooldown, latest-scene wins, BUSY, retry and mode re-entry\n";
}
