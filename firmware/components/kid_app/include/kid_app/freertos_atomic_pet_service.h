#pragma once

#include "kid_app/nvs_atomic_pet_store.h"
#include "kid_app/pet_local_event_id.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

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

// Opt-in replacement for legacy FreertosPetService. This service does NOT
// automatically migrate old 'kid_pet' and 'kid_bridge' storage. Exactly one
// Pet service must own the device. Must outlive its worker task.
class FreertosAtomicPetService {
public:
    enum class StartMode : uint8_t { ExistingOnly, FreshNoLegacy };
    FreertosAtomicPetService() : ledger_(store_) {}

    FreertosAtomicPetService(const FreertosAtomicPetService&) = delete;
    FreertosAtomicPetService& operator=(const FreertosAtomicPetService&) = delete;

    bool Start(StartMode mode, const char* device_id,
               AtomicPetCommitCallback committed,
               AtomicPetOutboundCallback outbound, void* context,
               PetSnapshot* initial = nullptr);

    bool PostExternal(const PetEvent& event);
    bool PostLocal(PetAction action);

    // Called by networking on its own task after receiving a parsed HTTP ACK.
    // Copies the message_id and queues the receipt; the Pet task itself
    // validates it and commits any dequeue with the pet state.
    bool SubmitAck(const PetDeviceBridge::ServerAck& ack);

private:
    enum class Kind : uint8_t { External, Local, Ack };
    struct Message {
        Kind kind = Kind::External;
        PetEvent event{};
        uint64_t seq = 0;
        uint64_t version = 0;
        bool accepted = false;
        bool duplicate = false;
        char message_id[96]{};
    };

    bool NoLegacyPetOrOutbox() const;
    void NotifyHead();
    static void TaskEntry(void* context);
    void Run();

    NvsAtomicPetStore store_;
    PetAtomicLedger ledger_;
    QueueHandle_t messages_ = nullptr;
    TaskHandle_t worker_ = nullptr;
    AtomicPetCommitCallback committed_ = nullptr;
    AtomicPetOutboundCallback outbound_ = nullptr;
    void* context_ = nullptr;
    char device_id_[64]{};  // Immutable, validated ASCII, not a secret.
};

}  // namespace kid_app
