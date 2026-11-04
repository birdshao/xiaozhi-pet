#pragma once

#include "kid_app/memory_store.h"
#include "kid_app/parent_settings.h"
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
// slot intact. Schema v3 appends the V3-06 parent settings (day plan,
// volume cap, motion switch) the same way: v1/v2 blobs load with the default
// settings and the next commit writes v3. Not the legacy kid_pet or
// kid_bridge NVS format.
// One task must own this object (do not call from HTTP/ISR callbacks).
class PetAtomicLedger {
public:
    static constexpr size_t kV1Bytes = 28 + PetCheckpointCodec::kBytes +
                                       PetDeviceBridge::kCapacity * 24 + 4;
    static constexpr size_t kV2Bytes = kV1Bytes + MemoryStore::kBytes;
    static constexpr size_t kBytes = kV2Bytes + ParentSettingsCodec::kBytes;
    class Store {
    public:
        using Blob = std::array<uint8_t, kBytes>;
        virtual ~Store() = default;
        // On Ok, *length is the stored size: kBytes (v3), kV2Bytes or kV1Bytes
        // (older, in the prefix of *out). Any other size fails decode as corrupt.
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
    // V3-06 parent menu: replaces the parent settings in one commit. Shares
    // the Pet replay-id window like RecordMemory (a replayed id is
    // Duplicate); invalid settings are Rejected. Never a pet interaction.
    CommitResult SetParentSettings(uint64_t id, const ParentSettings& settings);
    const ParentSettings& Parent() const { return parent_; }
    bool Peek(PetDeviceBridge::Record* out) const;
    AckResult Acknowledge(const PetDeviceBridge::ServerAck& ack,
                          const char* device_id);
    const PetSnapshot& Snapshot() const { return controller_.Snapshot(); }
    uint64_t Generation() const { return generation_; }
    uint64_t NextSequence() const { return queue_.next_sequence; }
    size_t PendingCount() const { return queue_.count; }

private:
    // next_parent null keeps the current parent settings.
    bool Commit(const PetCheckpoint& next_pet,
                const PetDeviceBridge::State& next_queue,
                const MemoryStore& next_memory,
                const ParentSettings* next_parent = nullptr);
    // version 3 is written; versions 1 and 2 are only produced for the
    // canonical check of an older blob being migrated.
    static bool Encode(const PetCheckpoint& pet,
                       const PetDeviceBridge::State& queue,
                       const MemoryStore& memory, const ParentSettings& parent,
                       uint16_t version, uint64_t generation, Store::Blob* blob);
    static bool Decode(const Store::Blob& blob, size_t length, PetCheckpoint* pet,
                       PetDeviceBridge::State* queue, MemoryStore* memory,
                       ParentSettings* parent, uint64_t* generation);
    static bool IsProjectable(PetAction action);
    static bool ValidId(const char* device_id);

    Store& store_;
    PetController controller_{};
    PetDeviceBridge::State queue_{};
    MemoryStore memory_{};
    ParentSettings parent_{};
    uint64_t generation_ = 0;
    int active_slot_ = -1;
    bool initialized_ = false;
    bool storage_empty_ = false;
};

}  // namespace kid_app
