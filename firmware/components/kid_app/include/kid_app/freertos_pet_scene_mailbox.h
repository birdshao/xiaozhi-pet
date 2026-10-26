#pragma once

#include "kid_app/freertos_pet_service.h"
#include "kid_app/pet_scene_presenter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

namespace kid_app {

// Latest scene wins: one descriptor, not a 48KB framebuffer.
// Queue lifetime must outlive all producers and consumers.
class FreertosPetSceneMailbox {
public:
    bool Initialize();
    bool Publish(const SceneModel& scene);
    bool Receive(SceneModel* scene, TickType_t timeout_ticks);

    // Called on the pet task after durable commit. context must be a
    // long-lived FreertosPetSceneMailbox. Never blocks EPD BUSY.
    static void OnCommit(const PetEvent& event, PumpResult result,
                         const PetEffects& effects, const PetSnapshot& snapshot,
                         void* context);
private:
    QueueHandle_t latest_ = nullptr;
};

}  // namespace kid_app
