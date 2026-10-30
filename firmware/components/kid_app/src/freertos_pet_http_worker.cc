#include "kid_app/freertos_pet_http_worker.h"

#include "esp_log.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace kid_app {
namespace {
constexpr const char* kTag="kid_pet_http";
constexpr uint32_t kStackBytes=8192;
constexpr uint32_t kMaxDelayMs=60000;
}

FreertosPetHttpWorker::FreertosPetHttpWorker(
        FreertosAtomicPetService& pet, const char* origin,
        const char* device_id, const char* token)
    : pet_(pet), transport_(origin,device_id,token) {
    if (transport_.Configured()) {
        const size_t len=std::strlen(device_id); // checked by transport
        std::memcpy(device_id_,device_id,len+1);
    }
}

bool FreertosPetHttpWorker::Start() {
    if (pending_ || task_ || !transport_.Configured()) return false;
    // Exactly one currently pending head can be valid. New notification
    // always supersedes a stale copy; xQueueOverwrite is nonblocking.
    pending_=xQueueCreate(1,sizeof(PetDeviceBridge::Record));
    if (!pending_) return false;
    if (xTaskCreate(&FreertosPetHttpWorker::Entry,"kid_pet_https",
                    kStackBytes,this,2,&task_)!=pdPASS) {
        vQueueDelete(pending_);
        pending_=nullptr;
        task_=nullptr;
        return false;
    }
    return true;
}

void FreertosPetHttpWorker::OnOutbound(
        const PetDeviceBridge::Record& event, void* context) {
    if (!context) return;
    auto* worker=static_cast<FreertosPetHttpWorker*>(context);
    if (worker->pending_) xQueueOverwrite(worker->pending_,&event);
}

void FreertosPetHttpWorker::Entry(void* context) {
    static_cast<FreertosPetHttpWorker*>(context)->Run();
}

void FreertosPetHttpWorker::Run() {
    PetDeviceBridge::Record record{};
    for (;;) {
        if (xQueueReceive(pending_,&record,portMAX_DELAY)!=pdTRUE) continue;
        uint32_t backoff=1000;
        for (;;) {
            char body[PetDeviceBridge::kJsonMaxBytes]{};
            char expected_id[96]{};
            const int len=std::snprintf(expected_id,sizeof(expected_id),
                "%s:%016" PRIx64,device_id_,record.event_id);
            if (len<=0 || static_cast<size_t>(len)>=sizeof(expected_id) ||
                !PetDeviceBridge::EncodePostBody(record,device_id_,body,sizeof(body))) {
                // Invalid config/event; do not discard the local durable head.
                ESP_LOGE(kTag,"Pet outbound record cannot be serialized");
            } else {
                PetDeviceBridge::ServerAck ack{};
                if (transport_.PostJson(body,std::strlen(body),&ack) &&
                    ack.message_id && ack.sequence==record.sequence &&
                    std::strcmp(ack.message_id,expected_id)==0 &&
                    ack.accepted && ack.server_version>0 &&
                    pet_.SubmitAck(ack)) {
                    // Receipt transferred to atomic pet worker (which retries
                    // NVS persistence locally). Do not send next event until
                    // that worker commits and invokes OnOutbound.
                    break;
                }
            }
            ESP_LOGW(kTag,"Pet event sync deferred; will retry same record");
            vTaskDelay(pdMS_TO_TICKS(backoff));
            backoff = backoff>=kMaxDelayMs/2 ? kMaxDelayMs : backoff*2;
        }
    }
}

}  // namespace kid_app
