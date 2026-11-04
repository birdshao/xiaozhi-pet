#include "kid_app/pet_controller.h"
#include "kid_app/pet_scene_presenter.h"

#include <cassert>
#include <iostream>

using namespace kid_app;

int main() {
    PetController pet;
    PetEffects effects;

    auto idle = PetScenePresenter::Present(pet.Snapshot());
    assert(idle.kind == SceneKind::Home);
    assert(idle.emotion == Emotion::Neutral);

    assert(pet.Apply({1, PetAction::Pickup}, &effects) == PetResult::Applied);
    auto happy = PetScenePresenter::Present(pet.Snapshot());
    assert(happy.kind == SceneKind::Home);
    assert(happy.emotion == Emotion::Happy);
    assert(effects.repaint);

    assert(pet.Apply({2, PetAction::StartStory}, &effects) == PetResult::Applied);
    auto reader = PetScenePresenter::Present(pet.Snapshot());
    assert(reader.kind == SceneKind::Reader);
    assert(reader.book_page == 1);
    assert(reader.emotion == Emotion::Curious);

    assert(pet.Apply({3, PetAction::NextPage}, &effects) == PetResult::Applied);
    assert(PetScenePresenter::Present(pet.Snapshot()).book_page == 2);

    assert(pet.Apply({4, PetAction::QuietBegin}, &effects) == PetResult::Applied);
    auto quiet = PetScenePresenter::Present(pet.Snapshot());
    assert(quiet.kind == SceneKind::Home);
    assert(quiet.emotion == Emotion::Sleepy);
    assert(pet.Apply({5, PetAction::Feed}, &effects) == PetResult::Rejected);
    assert(PetScenePresenter::Present(pet.Snapshot()).emotion == Emotion::Sleepy);

    assert(pet.Apply({6, PetAction::QuietEnd}, &effects) == PetResult::Applied);
    assert(PetScenePresenter::Present(pet.Snapshot()).emotion == Emotion::Neutral);

    // V6-01: the durable voice modes show the listening / speaking faces on
    // the home screen, and the normal picture comes back afterwards.
    assert(pet.Apply({7, PetAction::VoiceBegin}, &effects) == PetResult::Applied);
    auto listening = PetScenePresenter::Present(pet.Snapshot());
    assert(listening.kind == SceneKind::Home && listening.emotion == Emotion::Listening);
    assert(pet.Apply({8, PetAction::VoiceReplyBegin}, &effects) == PetResult::Applied);
    auto speaking = PetScenePresenter::Present(pet.Snapshot());
    assert(speaking.kind == SceneKind::Home && speaking.emotion == Emotion::Speaking);
    assert(pet.Apply({9, PetAction::VoiceEnd}, &effects) == PetResult::Applied);
    assert(PetScenePresenter::Present(pet.Snapshot()).emotion == Emotion::Neutral);
    // Quiet wins over a voice mode (no chat faces during quiet time).
    assert(pet.Apply({10, PetAction::VoiceBegin}, &effects) == PetResult::Applied);
    assert(pet.Apply({11, PetAction::QuietBegin}, &effects) == PetResult::Applied);
    assert(PetScenePresenter::Present(pet.Snapshot()).emotion == Emotion::Sleepy);

    // Render into a physical-size black/white buffer without backend services.
    MonoFrame frame;
    SceneComposer::Render(reader, frame);
    assert(frame.IsBlack(38, 30));
    SceneComposer::Render(quiet, frame);
    assert(frame.IsBlack(400, 425));
    std::cout << "PASS: deterministic pet state -> scene -> 1bpp frame\n";
}
