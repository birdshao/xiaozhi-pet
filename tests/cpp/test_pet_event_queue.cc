#include "kid_app/pet_event_queue.h"
#include <cassert>
#include <cstdint>
#include <iostream>

using namespace kid_app;

int main() {
    PetEventQueue<3> queue;
    assert(queue.Empty());
    assert(!queue.Push(PetEvent{0, PetAction::Pickup}));
    assert(queue.Push(PetEvent{1, PetAction::Pickup}));
    assert(queue.Push(PetEvent{2, PetAction::Feed}));
    assert(queue.Push(PetEvent{3, PetAction::StartStory}));
    assert(queue.Full());
    assert(!queue.Push(PetEvent{4, PetAction::NextPage}));

    PetEvent event{};
    assert(queue.Pop(&event) && event.id == 1);
    assert(queue.Pop(&event) && event.id == 2);
    assert(queue.Push(PetEvent{4, PetAction::NextPage}));
    assert(queue.Push(PetEvent{5, PetAction::StopStory}));
    assert(queue.Full());
    assert(queue.Pop(&event) && event.id == 3);
    assert(queue.Pop(&event) && event.id == 4);
    assert(queue.Pop(&event) && event.id == 5);
    assert(queue.Empty());
    assert(!queue.Pop(&event));
    assert(!queue.Pop(nullptr));

    // Serial task ownership: event queue provides order, PetController
    // provides deterministic rules and in-memory idempotency.
    PetEventQueue<4> process;
    PetController controller;
    assert(process.Push(PetEvent{41, PetAction::Feed}));
    assert(process.Push(PetEvent{41, PetAction::Feed}));
    assert(process.Push(PetEvent{42, PetAction::StartStory}));
    while (process.Pop(&event)) {
        PetEffects effects{};
        controller.Apply(event, &effects);
    }
    assert(controller.Snapshot().interaction_count == 1);
    assert(controller.Snapshot().mode == PetMode::Story);
    process.Clear();
    assert(process.Empty());
    std::cout << "PASS: bounded FIFO, wraparound, overflow, task-order and dedup\n";
}
