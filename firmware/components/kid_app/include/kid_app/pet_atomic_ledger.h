#pragma once

#include "kid_app/memory_store.h"
#include "kid_app/pet_controller.h"
#include "kid_app/pet_device_bridge.h"
#include "kid_app/pet_event_executor.h"
#include "kid_app/pet_persistence.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace kid_app {

// A single durable checkpoint is the authority for BOTH pet effects and
// outbound notifications. A crash cannot commit the pet effect without its
// matching notification, or dequeue an ACK without persisting it.
//
// Schema v2 adds the V5 MemoryStore (today / likes / growth / recent books)
// to the same blob, so memory commits in the same transaction as the Pet
// checkpoint and outbox. v1 blobs (no memory) still load and migrate to an
// empty memory; the next commit writes v2 to the other slot and leaves the v1
// slot intact. Not the legacy kid_pet or kid_bridge NVS format.
// One task must own this object (do not call from HTTP/ISR callbacks).
class PetAtomicLedger {
public:
    static constexpr size_t kV1Bytes = 28 + PetCheckpointCodec::kBytes +
                                       PetDeviceBridge::kCapacity * 24 + 4;
    static constexpr size_t kBytes = kV1Bytes + MemoryStore::kBytes;
    class Store {
    public:
        using Blob = std::array<uint8_t, kBytes>;
        virtual ~Store() = default;
        // On Ok, *length is the stored size: kBytes (v2) or kV1Bytes (v1, in
        // the prefix of *out). Any other size fails decode as corrupt.
        virtual BlobReadStatus Read(unsigned slot, Blob* out, size_t* length) = 0;
        virtual bool Write(unsigned slot, const Blob& value) = 0;
    };
    enum class LoadResult : uint8_t { Ready, Empty, Corrupt, StorageError };
    enum class AckResult : uint8_t {
        Confirmed, Empty, Mismatch, Rejected, StorageError
    };

    explicit PetAtomicLedger(Store& storage) : store_(storage) {}

    // Load never creates a default checkpoint. Fresh initialization or
    // legacy migration requires an explicit separate decision.
    LoadResult Load();
    bool InitializeFresh();
    // Caller MUST have established legacy Pet state, empty legacy bridge
    // outbox and an unprovisioned remote server with cursor 0. Never auto-run.
    bool ImportLegacyForFreshServer(const PetCheckpoint& checkpoint,
                                    uint64_t legacy_generation);

    // `day` (local civil day number, 0 = unknown) dates today's feed /
    // greeting memory counters committed with the event.
    CommitResult ApplyDurably(const PetEvent& event, PetEffects* effects,
                              uint32_t day = 0);
    // V5 structured memory event (choice answer, finished book/game, parent
    // wipe or temperament). Shares the Pet replay-id window, so a replayed
    // id is Duplicate and never double-counts. Guard rules: MemoryStore::Apply.
    CommitResult RecordMemory(const MemoryEvent& event);
    const MemoryStore& Memory() const { return memory_; }
    bool Peek(PetDeviceBridge::Record* out) const;
    AckResult Acknowledge(const PetDeviceBridge::ServerAck& ack,
                          const char* device_id);
    const PetSnapshot& Snapshot() const { return controller_.Snapshot(); }
    uint64_t Generation() const { return generation_; }
    uint64_t NextSequence() const { return queue_.next_sequence; }
    size_t PendingCount() const { return queue_.count; }

private:
    bool Commit(const PetCheckpoint& next_pet,
                const PetDeviceBridge::State& next_queue,
                const MemoryStore& next_memory);
    // version 2 is written; version 1 is only produced for the canonical
    // check of a v1 blob being migrated.
    static bool Encode(const PetCheckpoint& pet,
                       const PetDeviceBridge::State& queue,
                       const MemoryStore& memory, uint16_t version,
                       uint64_t generation, Store::Blob* blob);
    static bool Decode(const Store::Blob& blob, size_t length, PetCheckpoint* pet,
                       PetDeviceBridge::State* queue, MemoryStore* memory,
                       uint64_t* generation);
    static bool IsProjectable(PetAction action);
    static bool ValidId(const char* device_id);

    Store& store_;
    PetController controller_{};
    PetDeviceBridge::State queue_{};
    MemoryStore memory_{};
    uint64_t generation_ = 0;
    int active_slot_ = -1;
    bool initialized_ = false;
    bool storage_empty_ = false;
};

}  // namespace kid_app
