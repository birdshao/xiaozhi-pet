#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "kid_app/pet_controller.h"

namespace kid_app {

// Stable, byte-exact LE checkpoint encoding. No raw struct memcpy/padding.
// 16 byte header + 20 byte state + 128x8 replay IDs + 4 byte CRC32.
// CRC is corruption detection only, not a cryptographic signature.
class PetCheckpointCodec {
public:
    static constexpr size_t kBytes = 1064;
    using Blob = std::array<uint8_t, kBytes>;

    static bool Encode(const PetCheckpoint& checkpoint, uint64_t generation, Blob* out);
    static bool Decode(const Blob& blob, PetCheckpoint* out, uint64_t* generation);
};

enum class BlobReadStatus : uint8_t { Ok, NotFound, Error };

// Implementations must commit each key atomically or preserve its previous
// valid value on failed writes. NVS set_blob + nvs_commit is our ESP-IDF adapter.
class PetBlobStore {
public:
    virtual ~PetBlobStore() = default;
    virtual BlobReadStatus Read(unsigned slot, PetCheckpointCodec::Blob* out) = 0;
    virtual bool Write(unsigned slot, const PetCheckpointCodec::Blob& value) = 0;
};

enum class PetLoadResult : uint8_t { Restored, NotFound, Corrupt, StorageError };

class PetPersistence {
public:
    explicit PetPersistence(PetBlobStore& storage) : storage_(storage) {}
    PetLoadResult Load(PetController* pet);

    // Save to the opposite slot, keeping the previously valid slot intact.
    // The caller owns event ordering and should save before ACKing an event
    // that must survive a power loss.
    bool Save(const PetController& pet);

    // Coalesce noncritical saves. Caller supplies a monotonic timestamp and
    // forces a save for critical transitions (feeding, parental quiet mode).
    bool SaveIfDue(const PetController& pet, uint64_t now_ms,
                   uint64_t minimum_interval_ms, bool force);

    uint64_t Generation() const { return generation_; }
    int ActiveSlot() const { return active_slot_; }

private:
    PetBlobStore& storage_;
    uint64_t generation_ = 0;
    int active_slot_ = -1;
    uint64_t last_saved_ms_ = 0;
    bool has_saved_time_ = false;
};

} // namespace kid_app
