#include "kid_app/freertos_pet_scene_mailbox.h"

namespace kid_app {

bool FreertosPetSceneMailbox::Initialize() {
    if (latest_) return false;
    latest_ = xQueueCreate(1, sizeof(SceneModel));
    return latest_ != nullptr;
}

bool FreertosPetSceneMailbox::Publish(const SceneModel& scene) {
    if (!latest_) return false;
    return xQueueOverwrite(latest_, &scene) == pdPASS;
}

bool FreertosPetSceneMailbox::Receive(SceneModel* scene,
                                     TickType_t timeout_ticks) {
    return latest_ && scene &&
        xQueueReceive(latest_, scene, timeout_ticks) == pdPASS;
}

void FreertosPetSceneMailbox::OnCommit(
    const PetEvent&, PumpResult result, const PetEffects& effects,
    const PetSnapshot& snapshot, void* context) {
    auto* self = static_cast<FreertosPetSceneMailbox*>(context);
    if (!self || result != PumpResult::Committed || !effects.repaint) return;
    self->Publish(PetScenePresenter::Present(snapshot));
}

}  // namespace kid_app
