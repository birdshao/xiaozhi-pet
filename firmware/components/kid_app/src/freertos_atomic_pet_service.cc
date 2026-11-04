#include "kid_app/freertos_atomic_pet_service.h"

#include "kid_app/nvs_bridge_blob_store.h"
#include "kid_app/nvs_parent_settings_store.h"
#include "kid_app/nvs_pet_blob_store.h"

#include "esp_log.h"

#include <cinttypes>
#include <cstring>

namespace kid_app {
namespace {
constexpr const char* kTag = "kid_pet_atomic";
constexpr UBaseType_t kInboxCapacity = 24;
// Ledger load/commit keep two 1.8 KB blobs, checkpoints and the decode
// re-encode on the stack (and the migration verifies on this task too).
constexpr uint32_t kStackBytes = 20480;
constexpr TickType_t kPoll = pdMS_TO_TICKS(100);
constexpr TickType_t kRetry = pdMS_TO_TICKS(750);

// The V3-06 board key, read only (migration source, kept as rollback copy).
class NvsParentSource final : public PetLedgerMigration::ParentSource {
public:
    Result Load(ParentSettings* out) override {
        NvsParentSettingsStore store;
        switch (store.Load(out)) {
            case NvsParentSettingsStore::LoadResult::Loaded: return Result::Loaded;
            case NvsParentSettingsStore::LoadResult::NotFound: return Result::NotFound;
            case NvsParentSettingsStore::LoadResult::Corrupt: return Result::Corrupt;
            case NvsParentSettingsStore::LoadResult::StorageError: break;
        }
        return Result::StorageError;
    }
};

const char* ResultName(CommitResult result) {
    switch (result) {
        case CommitResult::Committed: return "committed";
        case CommitResult::Duplicate: return "duplicate";
        case CommitResult::Rejected: return "rejected";
        case CommitResult::StorageError: return "storage";
    }
    return "unknown";
}
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
    if (!started_) started_=xSemaphoreCreateBinary();
    if (!view_mutex_) view_mutex_=xSemaphoreCreateMutex();
    if (!save_mutex_) save_mutex_=xSemaphoreCreateMutex();
    if (!reply_) reply_=xSemaphoreCreateBinary();
    if (!parent_gate_) parent_gate_=xSemaphoreCreateMutex();
    if (!started_ || !view_mutex_ || !save_mutex_ || !reply_ || !parent_gate_) return false;
    messages_=xQueueCreate(kInboxCapacity,sizeof(Message));
    if (!messages_) return false;
    mode_=mode;
    committed_=committed;
    outbound_=outbound;
    context_=context;
    start_ok_=false;
    if (xTaskCreate(&FreertosAtomicPetService::TaskEntry,
            "kid_pet_atomic",kStackBytes,this,2,&worker_)!=pdPASS) {
        worker_=nullptr;
    } else {
        // The worker migrates and loads, then reports (and exits on failure).
        xSemaphoreTake(started_,portMAX_DELAY);
    }
    if (!worker_ || !start_ok_) {
        vQueueDelete(messages_);
        messages_=nullptr;
        worker_=nullptr;
        committed_=nullptr;
        outbound_=nullptr;
        context_=nullptr;
        return false;
    }
    if (initial) *initial=started_snapshot_;
    ready_.store(true);
    ESP_LOGI(kTag,"Atomic Pet worker started");
    return true;
}

bool FreertosAtomicPetService::PrepareLedger() {
    if (mode_==StartMode::MigrateLegacyLocal) {
        NvsPetBlobStore old_pet;
        NvsParentSource old_parent;
        NvsLedgerMigrationMarker marker;
        // Old outbox data is only noted (never sent or erased): read errors
        // count as present.
        NvsBridgeBlobStore old_outbox;
        bool bridge=false;
        for (unsigned slot=0;slot<2;++slot) {
            PetDeviceBridge::Storage::Blob q{};
            if (old_outbox.Read(slot,&q)!=BlobReadStatus::NotFound) bridge=true;
        }
        migration_=PetLedgerMigration::Run(store_,old_pet,old_parent,bridge,marker);
        ESP_LOGI(kTag,"PET_LEDGER_MIGRATION outcome=%s failure=%s flags=0x%02x "
                 "legacy_gen=%" PRIu64 " ledger_gen=%" PRIu64,
                 PetLedgerMigration::OutcomeName(migration_.outcome),
                 PetLedgerMigration::FailureName(migration_.failure),
                 static_cast<unsigned>(migration_.flags),
                 migration_.legacy_generation,migration_.ledger_generation);
        if (migration_.outcome==PetLedgerMigration::Outcome::Failed) return false;
        // No server transport on this device: never fill the outbox.
        ledger_.SetProjection(false);
    }
    const auto loaded=ledger_.Load();
    if (loaded==PetAtomicLedger::LoadResult::Corrupt ||
        loaded==PetAtomicLedger::LoadResult::StorageError) {
        ESP_LOGE(kTag,"Atomic Pet checkpoint unreadable; not erasing");
        return false;
    }
    if (loaded==PetAtomicLedger::LoadResult::Empty) {
        if (mode_!=StartMode::FreshNoLegacy || !NoLegacyPetOrOutbox() ||
            !ledger_.InitializeFresh()) {
            ESP_LOGE(kTag,"Cannot initialize atomic Pet: legacy or invalid storage");
            return false;
        }
    }
    PublishView();
    return true;
}

void FreertosAtomicPetService::PublishView() {
    temperament_.store(static_cast<uint8_t>(ledger_.Memory().GetTemperament()));
    xSemaphoreTake(view_mutex_,portMAX_DELAY);
    parent_view_=ledger_.Parent();
    memory_view_=ledger_.Memory();
    generation_view_=ledger_.Generation();
    xSemaphoreGive(view_mutex_);
}

ParentSettings FreertosAtomicPetService::ParentNow() const {
    if (!view_mutex_) return ParentSettings{};
    xSemaphoreTake(view_mutex_,portMAX_DELAY);
    const ParentSettings copy=parent_view_;
    xSemaphoreGive(view_mutex_);
    return copy;
}

MemoryStore FreertosAtomicPetService::MemoryNow() const {
    if (!view_mutex_) return MemoryStore{};
    xSemaphoreTake(view_mutex_,portMAX_DELAY);
    const MemoryStore copy=memory_view_;
    xSemaphoreGive(view_mutex_);
    return copy;
}

MemoryStore::Day FreertosAtomicPetService::TodayNow() const {
    if (!view_mutex_) return MemoryStore::Day{};
    xSemaphoreTake(view_mutex_,portMAX_DELAY);
    const MemoryStore::Day copy=memory_view_.Today();
    xSemaphoreGive(view_mutex_);
    return copy;
}

uint64_t FreertosAtomicPetService::GenerationNow() const {
    if (!view_mutex_) return 0;
    xSemaphoreTake(view_mutex_,portMAX_DELAY);
    const uint64_t copy=generation_view_;
    xSemaphoreGive(view_mutex_);
    return copy;
}

bool FreertosAtomicPetService::Enqueue(const Message& msg) {
    // Counted before the send: the worker can take the message the moment it
    // is queued, and Settled() must not read true while it commits.
    in_flight_.fetch_add(1);
    if (xQueueSend(messages_,&msg,0)==pdTRUE) return true;
    in_flight_.fetch_sub(1);
    return false;
}

bool FreertosAtomicPetService::PostExternal(const PetEvent& event) {
    if (!ready_.load() || !IsExternalEventId(event.id)) return false;
    Message msg{};
    msg.kind=Kind::External;
    msg.event=event;
    return Enqueue(msg);
}

bool FreertosAtomicPetService::PostLocal(PetAction action) {
    if (!ready_.load()) return false;
    Message msg{};
    msg.kind=Kind::Local;
    msg.event.action=action;
    return Enqueue(msg);
}

bool FreertosAtomicPetService::PostMemory(const MemoryEvent& event) {
    if (!ready_.load()) return false;
    Message msg{};
    msg.kind=Kind::Memory;
    msg.memory=event;
    return Enqueue(msg);
}

CommitResult FreertosAtomicPetService::SaveParentSettings(
        const ParentSettings& settings, TickType_t wait) {
    if (!ready_.load()) return CommitResult::StorageError;
    const TickType_t begun=xTaskGetTickCount();
    if (xSemaphoreTake(save_mutex_,wait)!=pdTRUE) return CommitResult::StorageError;
    Message msg{};
    msg.kind=Kind::Parent;
    msg.parent=settings;
    msg.ticket=++ticket_;
    if (!msg.ticket) msg.ticket=++ticket_;
    xSemaphoreTake(reply_,0);  // drop a reply to an earlier, abandoned save
    CommitResult result=CommitResult::StorageError;
    if (Enqueue(msg)) {
        bool answered=false;
        for (;;) {
            const TickType_t spent=xTaskGetTickCount()-begun;
            if (spent>=wait || xSemaphoreTake(reply_,wait-spent)!=pdTRUE) break;
            if (reply_ticket_.load()==msg.ticket) {
                result=static_cast<CommitResult>(reply_result_.load());
                answered=true;
                break;
            }
        }
        if (!answered) {
            // Timed out (e.g. behind a pet event that is retrying storage).
            // Under the gate the worker either already answered (take that
            // answer) or will skip this save: what the caller reports is
            // always what the ledger ends up holding.
            xSemaphoreTake(parent_gate_,portMAX_DELAY);
            if (xSemaphoreTake(reply_,0)==pdTRUE && reply_ticket_.load()==msg.ticket)
                result=static_cast<CommitResult>(reply_result_.load());
            else
                abandoned_ticket_.store(msg.ticket);
            xSemaphoreGive(parent_gate_);
        }
    }
    xSemaphoreGive(save_mutex_);
    return result;
}

bool FreertosAtomicPetService::SubmitAck(
        const PetDeviceBridge::ServerAck& ack) {
    if (!ready_.load() || !ack.message_id || !ack.sequence) return false;
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
    return Enqueue(msg);
}

void FreertosAtomicPetService::NotifyHead() {
    PetDeviceBridge::Record event{};
    if (outbound_ && ledger_.Peek(&event)) outbound_(event,context_);
}

void FreertosAtomicPetService::TaskEntry(void* context) {
    static_cast<FreertosAtomicPetService*>(context)->Run();
}

void FreertosAtomicPetService::Run() {
    start_ok_=PrepareLedger();
    if (start_ok_) started_snapshot_=ledger_.Snapshot();
    const bool ok=start_ok_;
    xSemaphoreGive(started_);
    if (!ok) {
        vTaskDelete(nullptr);  // Start cleans up; nothing was erased
        return;
    }
    ESP_LOGI(kTag,"PET_LEDGER_STACK free=%u",
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    NotifyHead(); // after reboot resume the original pending sequence.
    Message pending{};
    bool has_retry=false;
    uint32_t storage_failures=0;
    for (;;) {
        if (!has_retry && xQueueReceive(messages_,&pending,kPoll)!=pdTRUE) continue;
        // On a storage failure the message stays at the head; no later
        // message is processed until it commits.
        has_retry=Process(pending,&storage_failures);
        if (has_retry) {
            vTaskDelay(kRetry);
            continue;
        }
        storage_failures=0;
        in_flight_.fetch_sub(1);  // done: committed, duplicate or rejected
    }
}

bool FreertosAtomicPetService::Process(const Message& pending, uint32_t* storage_failures) {
    if (pending.kind==Kind::Ack) {
        const PetDeviceBridge::ServerAck ack{
            pending.message_id,pending.seq,pending.version,
            pending.accepted,pending.duplicate
        };
        const auto result=ledger_.Acknowledge(ack,device_id_);
        if (result==PetAtomicLedger::AckResult::StorageError) return true;
        if (result==PetAtomicLedger::AckResult::Confirmed)
            NotifyHead(); // now let transport process the next entry
        // Wrong or rejected receipts leave the head unmodified. Network
        // worker must retry its original immutable event, not forge ACK.
        return false;
    }

    if (pending.kind==Kind::Parent) {
        // Not retried: the parent menu shows the failure and keeps the
        // committed settings (a later retry would surprise the parent).
        xSemaphoreTake(parent_gate_,portMAX_DELAY);
        if (pending.ticket<=abandoned_ticket_.load()) {
            xSemaphoreGive(parent_gate_);
            ESP_LOGW(kTag,"PET_LEDGER_PARENT result=abandoned");
            return false;
        }
        const uint64_t id=NextLocalEventId(ledger_.Generation());
        const CommitResult result=id ? ledger_.SetParentSettings(id,pending.parent)
                                     : CommitResult::Rejected;
        if (result==CommitResult::Committed) PublishView();
        reply_result_.store(static_cast<uint8_t>(result));
        reply_ticket_.store(pending.ticket);
        xSemaphoreGive(reply_);
        xSemaphoreGive(parent_gate_);
        ESP_LOGI(kTag,"PET_LEDGER_PARENT result=%s",ResultName(result));
        return false;
    }

    if (pending.kind==Kind::Memory) {
        MemoryEvent memory=pending.memory;
        if (!memory.id) memory.id=NextLocalEventId(ledger_.Generation());
        if (!memory.day) memory.day=Day();
        const CommitResult result=memory.id ? ledger_.RecordMemory(memory)
                                            : CommitResult::Rejected;
        if (result==CommitResult::StorageError) {
            // Same id on retry: the generation did not move.
            if (++*storage_failures==1 || *storage_failures%60==0)
                ESP_LOGW(kTag,"Atomic Pet memory persist blocked; retry later");
            return true;
        }
        if (result==CommitResult::Committed) PublishView();
        ESP_LOGI(kTag,"PET_MEMORY_COMMIT kind=%u src=%u result=%s",
                 static_cast<unsigned>(memory.kind),static_cast<unsigned>(memory.source),
                 ResultName(result));
        return false;
    }

    PetEvent event=pending.event;
    if (pending.kind==Kind::Local) {
        event.id=NextLocalEventId(ledger_.Generation());
        if (!event.id) {
            ESP_LOGE(kTag,"Local Pet event ID exhausted");
            return false;
        }
    }

    PetEffects effects{};
    const bool was_empty=ledger_.PendingCount()==0;
    const CommitResult result=ledger_.ApplyDurably(event,&effects,Day());
    if (result==CommitResult::StorageError) {
        if (++*storage_failures==1 || *storage_failures%60==0)
            ESP_LOGW(kTag,"Atomic Pet persist blocked; retry later");
        return true;
    }
    if (result==CommitResult::Committed) PublishView();
    if (committed_) committed_(event,result,effects,ledger_.Snapshot(),context_);
    if (result==CommitResult::Committed && was_empty) NotifyHead();
    return false;
}

}  // namespace kid_app
