#include "kid_app/freertos_atomic_pet_service.h"

#include "kid_app/nvs_bridge_blob_store.h"
#include "kid_app/nvs_pet_blob_store.h"

#include "esp_log.h"

#include <cstring>

namespace kid_app {
namespace {
constexpr const char* kTag = "kid_pet_atomic";
constexpr UBaseType_t kInboxCapacity = 24;
constexpr uint32_t kStackBytes = 8192;
constexpr TickType_t kPoll = pdMS_TO_TICKS(100);
constexpr TickType_t kRetry = pdMS_TO_TICKS(750);
}  // namespace

bool FreertosAtomicPetService::NoLegacyPetOrOutbox() const {
    // Fresh initialization requires proof that neither legacy component has
    // previously stored a value. Never silently abandon unsent notifications.
    NvsPetBlobStore old_pet;
    NvsBridgeBlobStore old_outbox;
    for (unsigned slot=0;slot<2;++slot) {
        PetCheckpointCodec::Blob p{};
        PetDeviceBridge::Storage::Blob q{};
        if (old_pet.Read(slot,&p)!=BlobReadStatus::NotFound ||
            old_outbox.Read(slot,&q)!=BlobReadStatus::NotFound)
            return false;
    }
    return true;
}

bool FreertosAtomicPetService::Start(
        StartMode mode, const char* device_id,
        AtomicPetCommitCallback committed,
        AtomicPetOutboundCallback outbound, void* context,
        PetSnapshot* initial) {
    if (messages_ || worker_ || !device_id) return false;
    // Match FastAPI device path requirements before any NVS mutation.
    size_t size=0;
    for (;size<sizeof(device_id_);++size) {
        const char c=device_id[size];
        if (!c) break;
        if (!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||
              (c>='0'&&c<='9')||(size>0&&(c=='_'||c=='-'))))
            return false;
    }
    if (size<3 || size>=sizeof(device_id_)) return false;
    std::memcpy(device_id_,device_id,size);
    device_id_[size]=0;
    const auto loaded=ledger_.Load();
    if (loaded==PetAtomicLedger::LoadResult::Corrupt ||
        loaded==PetAtomicLedger::LoadResult::StorageError) {
        ESP_LOGE(kTag,"Atomic Pet checkpoint unreadable; not erasing");
        return false;
    }
    if (loaded==PetAtomicLedger::LoadResult::Empty) {
        if (mode!=StartMode::FreshNoLegacy || !NoLegacyPetOrOutbox() ||
            !ledger_.InitializeFresh()) {
            ESP_LOGE(kTag,"Cannot initialize atomic Pet: legacy or invalid storage");
            return false;
        }
    }
    if (initial) *initial=ledger_.Snapshot();
    messages_=xQueueCreate(kInboxCapacity,sizeof(Message));
    if (!messages_) return false;
    committed_=committed;
    outbound_=outbound;
    context_=context;
    if (xTaskCreate(&FreertosAtomicPetService::TaskEntry,
            "kid_pet_atomic",kStackBytes,this,2,&worker_)!=pdPASS) {
        vQueueDelete(messages_);
        messages_=nullptr;
        worker_=nullptr;
        committed_=nullptr;
        outbound_=nullptr;
        context_=nullptr;
        return false;
    }
    ESP_LOGI(kTag,"Atomic Pet worker started");
    return true;
}

bool FreertosAtomicPetService::PostExternal(const PetEvent& event) {
    if (!messages_ || !IsExternalEventId(event.id)) return false;
    Message msg{};
    msg.kind=Kind::External;
    msg.event=event;
    return xQueueSend(messages_,&msg,0)==pdTRUE;
}

bool FreertosAtomicPetService::PostLocal(PetAction action) {
    if (!messages_) return false;
    Message msg{};
    msg.kind=Kind::Local;
    msg.event.action=action;
    return xQueueSend(messages_,&msg,0)==pdTRUE;
}

bool FreertosAtomicPetService::SubmitAck(
        const PetDeviceBridge::ServerAck& ack) {
    if (!messages_ || !ack.message_id || !ack.sequence) return false;
    Message msg{};
    msg.kind=Kind::Ack;
    bool terminator=false;
    for (size_t i=0;i<sizeof(msg.message_id);++i) {
        msg.message_id[i]=ack.message_id[i];
        if (ack.message_id[i]=='\0') {
            terminator=true;
            break;
        }
    }
    if (!terminator) return false;
    msg.seq=ack.sequence;
    msg.version=ack.server_version;
    msg.accepted=ack.accepted;
    msg.duplicate=ack.duplicate;
    // No blocking inside network callback; if full, remote should retry the
    // stable event after its own timeout, receiving server DUPLICATE.
    return xQueueSend(messages_,&msg,0)==pdTRUE;
}

void FreertosAtomicPetService::NotifyHead() {
    PetDeviceBridge::Record event{};
    if (outbound_ && ledger_.Peek(&event)) outbound_(event,context_);
}

void FreertosAtomicPetService::TaskEntry(void* context) {
    static_cast<FreertosAtomicPetService*>(context)->Run();
}

void FreertosAtomicPetService::Run() {
    NotifyHead(); // after reboot resume the original pending sequence.
    Message pending{};
    bool has_retry=false;
    uint32_t storage_failures=0;
    for (;;) {
        if (!has_retry && xQueueReceive(messages_,&pending,kPoll)!=pdTRUE)
            continue;

        if (pending.kind==Kind::Ack) {
            const PetDeviceBridge::ServerAck ack{
                pending.message_id,pending.seq,pending.version,
                pending.accepted,pending.duplicate
            };
            const auto result=ledger_.Acknowledge(ack,device_id_);
            if (result==PetAtomicLedger::AckResult::StorageError) {
                has_retry=true;
                vTaskDelay(kRetry);
                continue;
            }
            has_retry=false;
            if (result==PetAtomicLedger::AckResult::Confirmed)
                NotifyHead(); // now let transport process the next entry
            // Wrong or rejected receipts leave the head unmodified. Network
            // worker must retry its original immutable event, not forge ACK.
            continue;
        }

        PetEvent event=pending.event;
        if (pending.kind==Kind::Local) {
            event.id=NextLocalEventId(ledger_.Generation());
            if (!event.id) {
                ESP_LOGE(kTag,"Local Pet event ID exhausted");
                has_retry=false;
                continue;
            }
        }

        PetEffects effects{};
        const bool was_empty=ledger_.PendingCount()==0;
        const CommitResult result=ledger_.ApplyDurably(event,&effects);
        if (result==CommitResult::StorageError) {
            has_retry=true;
            if (++storage_failures==1 || storage_failures%60==0)
                ESP_LOGW(kTag,"Atomic Pet persist blocked; retry later");
            vTaskDelay(kRetry);
            continue;
        }
        has_retry=false;
        storage_failures=0;
        if (committed_) committed_(event,result,effects,ledger_.Snapshot(),context_);
        if (result==CommitResult::Committed && was_empty) NotifyHead();
    }
}

}  // namespace kid_app
