#pragma once

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
// This is a separate v1 schema, not the legacy kid_pet or kid_bridge NVS
// format. One task must own this object (do not call from HTTP/ISR callbacks).
class PetAtomicLedger {
public:
    static constexpr size_t kBytes = 28 + PetCheckpointCodec::kBytes +
                                     PetDeviceBridge::kCapacity * 24 + 4;
    class Store {
    public:
        using Blob = std::array<uint8_t, kBytes>;
        virtual ~Store() = default;
        virtual BlobReadStatus Read(unsigned slot, Blob* out) = 0;
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

    CommitResult ApplyDurably(const PetEvent& event, PetEffects* effects);
    bool Peek(PetDeviceBridge::Record* out) const;
    AckResult Acknowledge(const PetDeviceBridge::ServerAck& ack,
                          const char* device_id);
    const PetSnapshot& Snapshot() const { return controller_.Snapshot(); }
    uint64_t Generation() const { return generation_; }
    uint64_t NextSequence() const { return queue_.next_sequence; }
    size_t PendingCount() const { return queue_.count; }

private:
    bool Commit(const PetCheckpoint& next_pet,
                const PetDeviceBridge::State& next_queue);
    static bool Encode(const PetCheckpoint& pet,
                       const PetDeviceBridge::State& queue,
                       uint64_t generation, Store::Blob* blob);
    static bool Decode(const Store::Blob& blob, PetCheckpoint* pet,
                       PetDeviceBridge::State* queue, uint64_t* generation);
    static bool IsProjectable(PetAction action);
    static bool ValidId(const char* device_id);

    Store& store_;
    PetController controller_{};
    PetDeviceBridge::State queue_{};
    uint64_t generation_ = 0;
    int active_slot_ = -1;
    bool initialized_ = false;
    bool storage_empty_ = false;
};

}  // namespace kid_app
