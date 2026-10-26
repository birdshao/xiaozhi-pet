#include "kid_app/freertos_pet_display_worker.h"

#include "esp_log.h"
#include "esp_timer.h"

namespace kid_app {
namespace {
constexpr char kTag[] = "kid_epd";
constexpr TickType_t kPollTicks = pdMS_TO_TICKS(100);
constexpr TickType_t kFailureBackoffTicks = pdMS_TO_TICKS(1000);
constexpr uint32_t kWorkerStackBytes = 8192;
}  // namespace

bool FreertosPetDisplayWorker::Start(PetRenderCallback renderer, void* context) {
    if (worker_ || !renderer) return false;
    renderer_ = renderer;
    context_ = context;
    if (xTaskCreate(&TaskEntry, "kid_epd", kWorkerStackBytes,
                    this, 2, &worker_) != pdPASS) {
        worker_ = nullptr;
        renderer_ = nullptr;
        context_ = nullptr;
        return false;
    }
    return true;
}

void FreertosPetDisplayWorker::TaskEntry(void* arg) {
    static_cast<FreertosPetDisplayWorker*>(arg)->TaskLoop();
}

void FreertosPetDisplayWorker::TaskLoop() {
    for (;;) {
        SceneModel next{};
        if (source_.Receive(&next, kPollTicks)) gate_.Request(next);

        SceneModel requested{};
        const auto now_ms = static_cast<uint64_t>(esp_timer_get_time() / 1000);
        if (!gate_.Acquire(now_ms, &requested)) continue;

        // This is the ONLY place that may block on a slow ePaper operation.
        // The renderer must take the board's LVGL/EPD lock and must not call
        // back into PetController / audio / NVS. It must return completion.
        const bool success = renderer_(requested, context_);
        const auto completed_ms = static_cast<uint64_t>(esp_timer_get_time() / 1000);
        gate_.Complete(success, completed_ms);
        if (!success) {
            ESP_LOGW(kTag, "Physical ePaper refresh failed; backing off");
            vTaskDelay(kFailureBackoffTicks);
        }
    }
}

}  // namespace kid_app
