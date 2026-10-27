#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "kid_app/pet_controller.h"
#include "kid_app/pet_persistence.h"

namespace kid_app {

// An offline-capable, fixed-memory outbox for server projections ONLY.
// Local PetController remains authoritative for device-side immediate effects.
// This class must be owned by one task (not ISR/network callback concurrently).
class PetDeviceBridge {
public:
    static constexpr size_t kCapacity = 16;
    static constexpr size_t kJsonMaxBytes = 384;

    struct Record {
        uint64_t event_id = 0;
        uint64_t sequence = 0;
        PetAction action = PetAction::Pickup;
    };
    struct State {
        uint64_t next_sequence = 1;
        uint16_t count = 0;
        std::array<Record, kCapacity> pending{};
    };

    // Separate 'kid_bridge' A/B NVS blobs. Every queue mutation is
    // persisted before it is reported to the caller as successful.
    class Storage {
    public:
        static constexpr size_t kBytes = 28 + kCapacity * 24 + 4;
        using Blob = std::array<uint8_t, kBytes>;
        virtual ~Storage() = default;
        virtual BlobReadStatus Read(unsigned slot, Blob* out) = 0;
        virtual bool Write(unsigned slot, const Blob& blob) = 0;
    };

    enum class LoadResult : uint8_t { Ready, Empty, Corrupt, StorageError };
    enum class AppendResult : uint8_t {
        Queued, AlreadyPending, Unsupported, Full, StorageError
    };
    enum class AckResult : uint8_t {
        Confirmed, NoPending, Mismatch, ServerRejected, StorageError
    };
    struct ServerAck {
        // Exact fields supplied by PetLearning API's per-event receipt.
        const char* message_id = nullptr;
        uint64_t sequence = 0;
        uint64_t server_version = 0;
        bool accepted = false;
        bool duplicate = false;  // server reason_code == DUPLICATE
    };

    explicit PetDeviceBridge(Storage& storage) : storage_(storage) {}

    LoadResult Load();
    AppendResult AppendCommitted(const PetEvent& event);
    bool Peek(Record* output) const;
    AckResult Acknowledge(const ServerAck& ack, const char* device_id);
    size_t PendingCount() const { return state_.count; }
    uint64_t NextSequence() const { return state_.next_sequence; }

    // Produces EXACT per-event JSON required by the Python FastAPI endpoint.
    // Does not transmit, fetch auth credentials or put private data in logs.
    static bool EncodePostBody(const Record& record, const char* device_id,
                               char* output, size_t capacity);

private:
    bool Save(const State& next);
    static bool Encode(const State& state, uint64_t generation, Storage::Blob* out);
    static bool Decode(const Storage::Blob& blob, State* out, uint64_t* generation);
    static const char* ServerAction(PetAction action);

    Storage& storage_;
    State state_{};
    uint64_t generation_ = 0;
    int active_slot_ = -1;
    bool loaded_ = false;
};

}  // namespace kid_app
