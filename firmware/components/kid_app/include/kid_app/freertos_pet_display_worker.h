#pragma once

#include "kid_app/freertos_pet_scene_mailbox.h"
#include "kid_app/pet_refresh_gate.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <atomic>

namespace kid_app {

// Called ONLY on the dedicated display worker. May block on LVGL + EPD BUSY.
// Return true only after actual physical refresh is complete.
using PetRenderCallback = bool (*)(const SceneModel& scene, void* context);

// Owns the refresh gate and consumes last-wins scene messages.
// It never allocates a 48KiB frame; the board adapter owns any PSRAM buffers.
// Start once after mailbox.Initialize(); lifetime extends until reboot.
class FreertosPetDisplayWorker {
public:
    explicit FreertosPetDisplayWorker(FreertosPetSceneMailbox& source,
                                     uint64_t min_refresh_interval_ms = 5000)
        : source_(source), gate_(min_refresh_interval_ms) {}
    FreertosPetDisplayWorker(const FreertosPetDisplayWorker&) = delete;
    FreertosPetDisplayWorker& operator=(const FreertosPetDisplayWorker&) = delete;

    bool Start(PetRenderCallback renderer, void* context);
    // Enabled only in interactive pet mode. Default disabled.
    void SetEnabled(bool enabled) {
        if (enabled) reset_before_next_frame_.store(true);
        enabled_.store(enabled);
    }

private:
    static void TaskEntry(void* context);
    void TaskLoop();
    FreertosPetSceneMailbox& source_;
    PetRefreshGate gate_;
    PetRenderCallback renderer_ = nullptr;
    void* context_ = nullptr;
    TaskHandle_t worker_ = nullptr;
    std::atomic<bool> enabled_{false};
    std::atomic<bool> reset_before_next_frame_{false};
};

}  // namespace kid_app
