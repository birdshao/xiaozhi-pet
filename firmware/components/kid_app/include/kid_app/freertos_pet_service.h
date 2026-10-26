#pragma once

#include "kid_app/nvs_pet_blob_store.h"
#include "kid_app/pet_event_runtime.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace kid_app {

// Callback is executed ONLY on the dedicated pet task after durable commit.
// Never call Start/Stop or manipulate the PetController from the callback.
// Copy values into a display/audio queue rather than blocking on ePaper.
using PetCommitCallback = void (*)(const PetEvent& event, PumpResult result,
                                   const PetEffects& effects,
                                   const PetSnapshot& snapshot, void* context);

// Instantiated by the host AFTER nvs_flash_init() has succeeded.
// A successfully started service must outlive its task (until reboot).
// This service doesn't start itself and has no impact on stock XiaoZhi builds
// unless the board/app explicitly constructs and calls Start().
class FreertosPetService {
public:
    FreertosPetService();
    FreertosPetService(const FreertosPetService&) = delete;
    FreertosPetService& operator=(const FreertosPetService&) = delete;

    // Fail closed if NVS contains corrupt data or is unreadable. No auto erase.
    bool Start(PetCommitCallback callback, void* context,
               PetSnapshot* initial_snapshot = nullptr);

    // Safe from normal FreeRTOS tasks. Never blocks a UI or audio callback.
    // External IDs reserve their highest bit as 0 (network retries retain ID).
    bool Post(const PetEvent& event);
    // Local button/IMU action gets a unique ID in the worker, derived from
    // the last durably committed checkpoint generation.
    bool PostLocal(PetAction action);
    // Safe from an ISR. Caller may request a reschedule if woken=true.
    bool PostFromISR(const PetEvent& event, BaseType_t* higher_priority_woken);

private:
    static void TaskEntry(void* context);
    void TaskLoop();

    NvsPetBlobStore blob_;
    PetController controller_;
    PetPersistence persistence_;
    PetEventExecutor executor_;
    PetEventRuntime<1> runtime_;  // Keep at most one unfinished event.
    QueueHandle_t inbox_ = nullptr;
    TaskHandle_t worker_ = nullptr;
    PetCommitCallback callback_ = nullptr;
    void* callback_context_ = nullptr;
};

}  // namespace kid_app
