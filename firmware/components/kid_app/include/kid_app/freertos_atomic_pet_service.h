#pragma once

#include "kid_app/nvs_atomic_pet_store.h"
#include "kid_app/pet_ledger_migration.h"
#include "kid_app/pet_local_event_id.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <atomic>

namespace kid_app {

// Commit notifications are called only on the dedicated Pet worker and only
// AFTER the combined Pet+outbox NVS checkpoint has committed. UI/audio
// callbacks must copy data into their own queues and return promptly.
using AtomicPetCommitCallback = void (*)(
    const PetEvent&, CommitResult, const PetEffects&,
    const PetSnapshot&, void* context);

// Copy-only notification for networking; the callback must not perform HTTP
// or touch PetAtomicLedger. HTTP failures are retried by the network worker.
using AtomicPetOutboundCallback = void (*)(
    const PetDeviceBridge::Record&, void* context);

// Local civil day number (0 = clock unknown) for memory counters; called on
// the Pet worker, so it must only read an atomic.
using AtomicPetDayProvider = uint32_t (*)(void* context);

// Replacement for legacy FreertosPetService. Exactly one Pet service must
// own the device. Must outlive its worker task.
//   ExistingOnly / FreshNoLegacy: never touch old 'kid_pet'/'kid_bridge'.
//   MigrateLegacyLocal (the Waveshare board, no server yet): every Start runs
//   PetLedgerMigration (one-time, verified, idempotent; old keys are only
//   read) and commits child actions without outbox records.
class FreertosAtomicPetService {
public:
    enum class StartMode : uint8_t { ExistingOnly, FreshNoLegacy, MigrateLegacyLocal };
    FreertosAtomicPetService() : ledger_(store_) {}

    FreertosAtomicPetService(const FreertosAtomicPetService&) = delete;
    FreertosAtomicPetService& operator=(const FreertosAtomicPetService&) = delete;

    // Migration and ledger load run on the worker's own (large) stack; the
    // caller waits for the result. A failed Start can be retried (a failed
    // migration changes nothing).
    bool Start(StartMode mode, const char* device_id,
               AtomicPetCommitCallback committed,
               AtomicPetOutboundCallback outbound, void* context,
               PetSnapshot* initial = nullptr);
    // Set before Start; without it events carry day 0 (unknown).
    void SetDayProvider(AtomicPetDayProvider provider, void* context) {
        day_provider_ = provider;
        day_context_ = context;
    }

    bool PostExternal(const PetEvent& event);
    bool PostLocal(PetAction action);
    // V5 structured memory event (likes from choice questions, finished
    // books/games, parent wipe/temperament). id 0 = the worker assigns a
    // local id; day 0 = the day provider's day. Guard rules: MemoryStore.
    bool PostMemory(const MemoryEvent& event);
    // V3-06 parent menu: commits the settings and waits up to `wait` for the
    // result (never call on the Pet worker). Not retried by the worker: on
    // StorageError the caller keeps what ParentNow() says. A timed-out save
    // is abandoned (never committed later), so the result always matches
    // what the ledger ends up holding.
    CommitResult SaveParentSettings(const ParentSettings& settings, TickType_t wait);

    // Called by networking on its own task after receiving a parsed HTTP ACK.
    // Copies the message_id and queues the receipt; the Pet task itself
    // validates it and commits any dequeue with the pet state.
    bool SubmitAck(const PetDeviceBridge::ServerAck& ack);

    // True once every posted message has been committed (or rejected) and
    // the inbox is empty. Used before PMIC power-off; best effort, not a lock.
    bool Settled() const { return in_flight_.load() == 0; }

    // Copies of the last committed state, safe from any task after Start.
    ParentSettings ParentNow() const;
    MemoryStore MemoryNow() const;
    MemoryStore::Day TodayNow() const;  // small: for callers on tight stacks
    Temperament TemperamentNow() const {
        return static_cast<Temperament>(temperament_.load());
    }
    uint64_t GenerationNow() const;
    // Last Start's migration (MigrateLegacyLocal only), also after a failure.
    PetLedgerMigration::Report Migration() const { return migration_; }

private:
    enum class Kind : uint8_t { External, Local, Ack, Memory, Parent };
    struct Message {
        Kind kind = Kind::External;
        PetEvent event{};
        MemoryEvent memory{};
        ParentSettings parent{};
        uint32_t ticket = 0;
        uint64_t seq = 0;
        uint64_t version = 0;
        bool accepted = false;
        bool duplicate = false;
        char message_id[96]{};
    };

    bool NoLegacyPetOrOutbox() const;
    bool PrepareLedger();
    bool Enqueue(const Message& msg);
    // One message; true = storage failed, keep it at the head and retry.
    bool Process(const Message& pending, uint32_t* storage_failures);
    void PublishView();
    void NotifyHead();
    uint32_t Day() const { return day_provider_ ? day_provider_(day_context_) : 0; }
    static void TaskEntry(void* context);
    void Run();

    NvsAtomicPetStore store_;
    PetAtomicLedger ledger_;
    StartMode mode_ = StartMode::ExistingOnly;
    QueueHandle_t messages_ = nullptr;
    TaskHandle_t worker_ = nullptr;
    SemaphoreHandle_t started_ = nullptr;   // worker -> Start
    SemaphoreHandle_t view_mutex_ = nullptr;
    SemaphoreHandle_t save_mutex_ = nullptr;  // one SaveParentSettings at a time
    SemaphoreHandle_t reply_ = nullptr;      // worker -> SaveParentSettings
    std::atomic<uint32_t> reply_ticket_{0};
    std::atomic<uint8_t> reply_result_{0};
    uint32_t ticket_ = 0;                    // under save_mutex_
    SemaphoreHandle_t parent_gate_ = nullptr;  // abandon vs. commit of a save
    std::atomic<uint32_t> abandoned_ticket_{0};  // timed-out saves up to here
    bool start_ok_ = false;
    PetSnapshot started_snapshot_{};       // copied on the worker before Start returns
    std::atomic<bool> ready_{false};       // Post*/Save/SubmitAck accepted
    AtomicPetCommitCallback committed_ = nullptr;
    AtomicPetOutboundCallback outbound_ = nullptr;
    void* context_ = nullptr;
    AtomicPetDayProvider day_provider_ = nullptr;
    void* day_context_ = nullptr;
    std::atomic<uint32_t> in_flight_{0};    // queued or being processed
    // View copies (view_mutex_, except the temperament atomic).
    ParentSettings parent_view_{};
    MemoryStore memory_view_{};
    std::atomic<uint8_t> temperament_{static_cast<uint8_t>(Temperament::Gentle)};
    uint64_t generation_view_ = 0;
    PetLedgerMigration::Report migration_{};
    char device_id_[64]{};  // Immutable, validated ASCII, not a secret.
};

}  // namespace kid_app
