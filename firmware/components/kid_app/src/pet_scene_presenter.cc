#include "kid_app/pet_scene_presenter.h"

namespace kid_app {

SceneModel PetScenePresenter::Present(const PetSnapshot& state) {
    SceneModel scene{};
    // Reader content is a separate scene. This method never changes pet
    // progress or tries to open books/SD files.
    scene.kind = (state.mode == PetMode::Story) ?
        SceneKind::Reader : SceneKind::Home;
    scene.book_page = state.active_page == 0 ? 1 : state.active_page;

    if (state.quiet || state.mode == PetMode::Quiet) {
        scene.emotion = Emotion::Sleepy;
        return scene;
    }
    switch (state.mood) {
        case PetMood::Happy: scene.emotion = Emotion::Happy; break;
        case PetMood::Curious: scene.emotion = Emotion::Curious; break;
        case PetMood::Thinking: scene.emotion = Emotion::Curious; break;
        case PetMood::Sleepy: scene.emotion = Emotion::Sleepy; break;
        case PetMood::Neutral:
        default: scene.emotion = Emotion::Neutral; break;
    }
    return scene;
}

}  // namespace kid_app
