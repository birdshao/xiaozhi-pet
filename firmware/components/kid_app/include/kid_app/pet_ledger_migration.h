#pragma once

#include "kid_app/parent_settings.h"
#include "kid_app/pet_atomic_ledger.h"
#include "kid_app/pet_persistence.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace kid_app {

// One-time board migration into PetAtomicLedger from the stores the board
// used before it switched to FreertosAtomicPetService: the legacy kid_pet
// checkpoint (PetPersistence A/B) and the V3-06 board-only parent key
// kid_parent/settings. Pure logic over storage ports (host-tested with
// simulated NVS, including power loss at every step).
//
//   read old (never written) -> write the first ledger blob -> read it back
//   and load it -> only then write the "migrated" marker (own NVS key).
//
// The legacy keys are never overwritten. They are released (erased) only
// once the marker says the ledger was verified and the ledger still loads:
// the 16 KiB NVS partition cannot hold the old copy next to the two ledger
// slots plus the new value of a slot being rewritten (ADR-014 addendum).
// Until then they are the source a resumed migration verifies against.
// Idempotent: every boot runs Run(); with a valid
// marker and a loadable ledger it changes nothing. A power cut before the
// marker is written leaves either no ledger (start again), our exact first
// blob (verified, then the marker is written) or a torn/unverified blob in
// slot 0 that only this migration can have written (rewritten from the
// legacy source, which is still intact). Anything else fails closed: no
// erase, no guess, the pet stays off until a person looks at it.
class PetLedgerMigration {
public:
    // Read-only view of the V3-06 board key (NvsParentSettingsStore).
    class ParentSource {
    public:
        enum class Result : uint8_t { Loaded, NotFound, Corrupt, StorageError };
        virtual ~ParentSource() = default;
        // *out is only written when Loaded.
        virtual Result Load(ParentSettings* out) = 0;
    };
    // The "migrated" marker: one key, written after the ledger is verified.
    class MarkerStore {
    public:
        static constexpr size_t kBytes = 32;
        using Blob = std::array<uint8_t, kBytes>;
        virtual ~MarkerStore() = default;
        // On Ok, *length is the stored size; any size but kBytes is corrupt.
        virtual BlobReadStatus Read(Blob* out, size_t* length) = 0;
        virtual bool Write(const Blob& value) = 0;
    };
    // Erases the legacy keys (kid_pet slots, kid_parent/settings). Called
    // only after a verified, marked migration; must be idempotent (nothing
    // left = true) and must not touch any other key.
    class LegacyRelease {
    public:
        virtual ~LegacyRelease() = default;
        virtual bool Release() = 0;
    };

    enum class Outcome : uint8_t {
        AlreadyMigrated,  // marker + loadable ledger: normal boot
        Migrated,         // ledger written and verified now, marker written
        Resumed,          // a cut after the ledger write: verified, marker written
        Failed,           // see Failure; nothing was erased
    };
    enum class Failure : uint8_t {
        None,
        MarkerUnreadable, MarkerCorrupt, MarkerWithoutLedger,
        LedgerUnreadable, LedgerCorrupt, LedgerMismatch,
        LegacyUnreadable, LegacyCorrupt, ParentUnreadable,
        LedgerWriteFailed, VerifyFailed, MarkerWriteFailed,
    };
    // Where the first ledger blob came from (kept in the marker).
    enum Flag : uint8_t {
        kLegacyPet = 1u << 0,            // kid_pet checkpoint imported
        kLegacyParent = 1u << 1,         // kid_parent/settings imported
        kLegacyParentCorrupt = 1u << 2,  // unreadable key: defaults, as before
        kLegacyBridge = 1u << 3,         // old kid_bridge data seen (left as is)
        kRewroteUnverified = 1u << 4,    // replaced our own torn first write
    };
    struct Report {
        Outcome outcome = Outcome::Failed;
        Failure failure = Failure::None;
        uint8_t flags = 0;
        uint64_t legacy_generation = 0;  // kid_pet generation imported, 0 = none
        uint64_t ledger_generation = 0;  // generation of the first ledger blob
        // Release() ran (marker + loadable ledger) and reported success.
        // Not part of the marker; a failed release is retried next boot.
        bool legacy_released = false;
    };

    // legacy_bridge_present: the caller saw (or could not rule out) data in
    // the old kid_bridge outbox. It is never sent or erased here; the flag
    // lets a future server provisioning refuse a silent cursor reset.
    // release (optional): frees the legacy keys after a verified, marked
    // migration; never called on a failure path.
    static Report Run(PetAtomicLedger::Store& ledger, PetBlobStore& legacy_pet,
                      ParentSource& legacy_parent, bool legacy_bridge_present,
                      MarkerStore& marker, LegacyRelease* release = nullptr);

    // Marker codec: 'K' 'P' 'M' 'G', u16 version 1, u8 flags, u8 0,
    // u64 legacy generation, u64 ledger generation, u32 0, CRC-32 (LE).
    static bool EncodeMarker(const Report& report, MarkerStore::Blob* out);
    static bool DecodeMarker(const MarkerStore::Blob& blob, size_t length, Report* out);

    static const char* OutcomeName(Outcome outcome);
    static const char* FailureName(Failure failure);
};

}  // namespace kid_app
