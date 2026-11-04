#pragma once

#include "kid_app/pet_atomic_ledger.h"
#include "kid_app/pet_ledger_migration.h"

namespace kid_app {

// Separate opt-in A/B NVS namespace for the combined Pet/outbox transaction.
// Does not erase or silently migrate legacy 'kid_pet' / 'kid_bridge' keys.
class NvsAtomicPetStore final : public PetAtomicLedger::Store {
public:
    BlobReadStatus Read(unsigned slot, Blob* out, size_t* length) override;
    bool Write(unsigned slot, const Blob& blob) override;
};

// PetLedgerMigration's "migrated" marker: key 'migrated' in the same
// kid_pet_sync namespace, written once after the ledger was verified.
// NVS keeps the previous value (none) if power fails during set+commit.
class NvsLedgerMigrationMarker final : public PetLedgerMigration::MarkerStore {
public:
    BlobReadStatus Read(Blob* out, size_t* length) override;
    bool Write(const Blob& value) override;
};

}  // namespace kid_app
