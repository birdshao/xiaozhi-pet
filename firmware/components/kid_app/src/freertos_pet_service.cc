#include "kid_app/freertos_pet_service.h"
#include "kid_app/pet_local_event_id.h"

#include "esp_log.h"

namespace kid_app {
namespace {
constexpr char kTag[] = "kid_pet";
constexpr UBaseType_t kInboxCapacity = 24;
constexpr uint32_t kStackBytes = 8192;  // ESP-IDF xTaskCreate stack in bytes.
constexpr TickType_t kIdlePollTicks = pdMS_TO_TICKS(100);
constexpr TickType_t kStorageRetryTicks = pdMS_TO_TICKS(750);
}  // namespace

FreertosPetService::FreertosPetService()
    : persistence_(blob_), executor_(controller_, persistence_), runtime_(executor_) {}

bool FreertosPetService::Start(PetCommitCallback callback, void* context,
                               PetSnapshot* initial_snapshot) {
    if (worker_ != nullptr || inbox_ != nullptr) return false;
    const PetLoadResult loaded = persistence_.Load(&controller_);
    if (loaded == PetLoadResult::Corrupt || loaded == PetLoadResult::StorageError) {
        ESP_LOGE(kTag, "Pet NVS load failed; refusing to start (will NOT erase)");
        return false;
    }

    if (initial_snapshot) *initial_snapshot = controller_.Snapshot();
    inbox_ = xQueueCreate(kInboxCapacity, sizeof(PetEvent));
    if (inbox_ == nullptr) {
        ESP_LOGE(kTag, "Pet event queue allocation failed");
        return false;
    }

    callback_ = callback;
    callback_context_ = context;
    const BaseType_t status = xTaskCreate(&FreertosPetService::TaskEntry,
        "kid_pet", kStackBytes, this, 2, &worker_);
    if (status != pdPASS) {
        vQueueDelete(inbox_);
        inbox_ = nullptr;
        worker_ = nullptr;
        callback_ = nullptr;
        callback_context_ = nullptr;
        ESP_LOGE(kTag, "Pet task start failed");
        return false;
    }
    ESP_LOGI(kTag, "Pet task running: load_status=%d", static_cast<int>(loaded));
    return true;
}

bool FreertosPetService::Post(const PetEvent& event) {
    if (!inbox_ || !IsExternalEventId(event.id)) return false;
    return xQueueSend(inbox_, &event, 0) == pdTRUE;
}

bool FreertosPetService::PostLocal(PetAction action) {
    if (!inbox_) return false;
    // id==0 is only a private FreeRTOS-inbox sentinel, never accepted by
    // PetEventRuntime or PetController. Single worker assigns its durable ID.
    const PetEvent pending{0, action};
    return xQueueSend(inbox_, &pending, 0) == pdTRUE;
}

bool FreertosPetService::PostFromISR(const PetEvent& event,
                                    BaseType_t* higher_priority_woken) {
    if (!inbox_ || !IsExternalEventId(event.id) || !higher_priority_woken) return false;
    return xQueueSendFromISR(inbox_, &event, higher_priority_woken) == pdTRUE;
}

void FreertosPetService::TaskEntry(void* context) {
    static_cast<FreertosPetService*>(context)->TaskLoop();
}

void FreertosPetService::TaskLoop() {
    uint32_t failures = 0;
    while (true) {
        // On a previous storage failure the event stays at the head.
        // No later event is processed until the failed transaction commits.
        if (!runtime_.HasPendingRetry()) {
            PetEvent event{};
            if (xQueueReceive(inbox_, &event, kIdlePollTicks) == pdTRUE) {
                if (event.id == 0) {
                    event.id = NextLocalEventId(persistence_.Generation());
                    if (event.id == 0) {
                        ESP_LOGE(kTag, "Pet local event generation exhausted");
                        continue;
                    }
                }
                // A single-worker runtime, capacity 1: should always succeed.
                if (!runtime_.Enqueue(event)) {
                    ESP_LOGE(kTag, "Pet runtime invariant broken: event rejected");
                    continue;
                }
            }
        }

        PetEffects effects{};
        PetEvent completed{};
        const PumpResult outcome = runtime_.PumpOne(&effects, &completed);
        if (outcome == PumpResult::Idle) continue;
        if (outcome == PumpResult::StorageBlocked) {
            // Rate-limited warning without dumping child events or NVS contents.
            if (++failures == 1 || failures % 60 == 0)
                ESP_LOGW(kTag, "Pet persistence blocked; retrying same event (failures=%lu)",
                         static_cast<unsigned long>(failures));
            // Avoid wearing out NVS or spinning endlessly on full partitions.
            // Inbox remains bounded; producers see backpressure on overflow.
            vTaskDelay(kStorageRetryTicks);
            continue;
        }
        failures = 0;
        // No callback is emitted for uncommitted events.
        if (callback_ != nullptr) {
            callback_(completed, outcome, effects, controller_.Snapshot(),
                      callback_context_);
        }
    }
}

}  // namespace kid_app
