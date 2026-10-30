#include "kid_app/pet_input_router.h"
#include "kid_app/pet_scene_presenter.h"

#include <cassert>
#include <iostream>

using namespace kid_app;

static PetAction Decision(PetMode mode, PetClick click) {
    const auto result = PetInputRouter::Decide(mode, click);
    assert(result.valid);
    return result.action;
}

int main() {
    assert(Decision(PetMode::Idle, PetClick::Single) == PetAction::Pickup);
    assert(Decision(PetMode::Interact, PetClick::Double) == PetAction::StartStory);
    assert(Decision(PetMode::Idle, PetClick::Triple) == PetAction::Feed);
    assert(Decision(PetMode::Story, PetClick::Single) == PetAction::NextPage);
    assert(Decision(PetMode::Story, PetClick::Double) == PetAction::StopStory);
    assert(!PetInputRouter::Decide(PetMode::Story, PetClick::Triple).valid);
    for (PetMode mode : {PetMode::Quiet, PetMode::Listening, PetMode::Speaking}) {
        for (PetClick click : {PetClick::Single, PetClick::Double, PetClick::Triple})
            assert(!PetInputRouter::Decide(mode, click).valid);
    }

    PetController pet;
    PetEffects effects{};
    assert(pet.Apply({1, Decision(pet.Snapshot().mode, PetClick::Double)},
                     &effects) == PetResult::Applied);
    assert(pet.Snapshot().mode == PetMode::Story);
    assert(PetScenePresenter::Present(pet.Snapshot()).kind == SceneKind::Reader);
    assert(pet.Apply({2, Decision(pet.Snapshot().mode, PetClick::Single)},
                     &effects) == PetResult::Applied);
    assert(pet.Snapshot().active_page == 2);
    assert(pet.Apply({3, Decision(pet.Snapshot().mode, PetClick::Double)},
                     &effects) == PetResult::Applied);
    assert(pet.Snapshot().mode == PetMode::Idle);
    assert(PetScenePresenter::Present(pet.Snapshot()).kind == SceneKind::Home);
    assert(pet.Apply({4, Decision(pet.Snapshot().mode, PetClick::Triple)},
                     &effects) == PetResult::Applied);
    assert(pet.Snapshot().interaction_count == 1);
    std::cout << "PASS: BOOT click routing enters, advances, exits reader and feeds only at home\n";
}
