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
    std::cout << "PASS: ePaper cooldown, latest-scene wins, BUSY and retry\n";
}
