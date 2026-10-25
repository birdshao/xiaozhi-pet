#pragma once

#include "kid_app/pet_controller.h"
#include "kid_app/scene_model.h"

namespace kid_app {

// Pure state-to-visual projection. Neither MCU peripherals nor LLM owns this.
class PetScenePresenter {
public:
    static SceneModel Present(const PetSnapshot& state);
};

}  // namespace kid_app
