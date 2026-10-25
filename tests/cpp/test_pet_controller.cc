#include "kid_app/pet_controller.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>

using namespace kid_app;

static void expect(PetController& pet, uint64_t id, PetAction action, PetResult result) {
    PetEffects fx{};
    assert(pet.Apply(PetEvent{id, action}, &fx) == result);
}

int main() {
    PetController pet;
    PetEffects fx{};
    assert(pet.Snapshot().schema_version == 1);
    assert(pet.Apply(PetEvent{0, PetAction::Feed}, &fx) == PetResult::Rejected);
    assert(pet.Apply(PetEvent{1, PetAction::Feed}, nullptr) == PetResult::Rejected);

    expect(pet, 1, PetAction::Feed, PetResult::Applied);
    assert(pet.Snapshot().interaction_count == 1);
    expect(pet, 1, PetAction::Feed, PetResult::Duplicate);
    assert(pet.Snapshot().interaction_count == 1);

    expect(pet, 2, PetAction::StartStory, PetResult::Applied);
    assert(pet.Snapshot().mode == PetMode::Story);
    expect(pet, 3, PetAction::NextPage, PetResult::Applied);
    assert(pet.Snapshot().active_page == 2);
    expect(pet, 4, PetAction::StopStory, PetResult::Applied);
    expect(pet, 5, PetAction::NextPage, PetResult::Rejected);
    assert(pet.Snapshot().active_page == 2);

    expect(pet, 6, PetAction::VoiceBegin, PetResult::Applied);
    expect(pet, 7, PetAction::VoiceReplyBegin, PetResult::Applied);
    assert(pet.Snapshot().mode == PetMode::Speaking);
    expect(pet, 8, PetAction::QuietBegin, PetResult::Applied);
    assert(pet.Snapshot().quiet);
    assert(pet.Snapshot().mode == PetMode::Quiet);
    expect(pet, 9, PetAction::Feed, PetResult::Rejected);
    expect(pet, 10, PetAction::VoiceBegin, PetResult::Rejected);
    expect(pet, 11, PetAction::QuietEnd, PetResult::Applied);
    assert(!pet.Snapshot().quiet);
    expect(pet, 12, PetAction::Pickup, PetResult::Applied);
    assert(pet.Snapshot().mood == PetMood::Happy);

    // Rejected event IDs do NOT poison later valid events.
    expect(pet, 13, PetAction::NextPage, PetResult::Rejected);
    expect(pet, 13, PetAction::Feed, PetResult::Applied);
    assert(pet.Snapshot().interaction_count == 2);

    // FIFO bounded de-dup window; older IDs may be reprocessed after 128 new
    // events. Persistent dedup across reboot will be implemented in P2-05.
    pet.ResetForTest();
    for (uint64_t id=100; id<100 + PetController::kSeenCapacity; ++id)
        expect(pet, id, PetAction::Feed, PetResult::Applied);
    assert(pet.Snapshot().interaction_count == PetController::kSeenCapacity);
    expect(pet, 100, PetAction::Feed, PetResult::Duplicate);
    expect(pet, 229, PetAction::Feed, PetResult::Applied);
    expect(pet, 100, PetAction::Feed, PetResult::Applied);

    std::cout << "PASS: deterministic pet state, quiet mode, voice, story and bounded dedup\n";
}
