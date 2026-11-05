#pragma once

#include "kid_app/pet_atomic_ledger.h"
#include "kid_app/pet_ledger_migration.h"

#include <cstddef>

namespace kid_app {

// Separate opt-in A/B NVS namespace for the combined Pet/outbox transaction.
// Does not erase or silently migrate legacy 'kid_pet' / 'kid_bridge' keys.
// The esp_err_t of the last failed NVS call is kept for the logs (ESP_OK =
// nothing failed in NVS since ClearLastError).
class NvsAtomicPetStore final : public PetAtomicLedger::Store {
public:
    BlobReadStatus Read(unsigned slot, Blob* out, size_t* length) override;
    bool Write(unsigned slot, const Blob& blob) override;
    int LastError() const { return last_error_; }
    void ClearLastError() { last_error_ = 0; }

private:
    int last_error_ = 0;  // esp_err_t
};

// PetLedgerMigration's "migrated" marker: key 'migrated' in the same
// kid_pet_sync namespace, written once after the ledger was verified.
// NVS keeps the previous value (none) if power fails during set+commit.
class NvsLedgerMigrationMarker final : public PetLedgerMigration::MarkerStore {
public:
    BlobReadStatus Read(Blob* out, size_t* length) override;
    bool Write(const Blob& value) override;
    int LastError() const { return last_error_; }

private:
    int last_error_ = 0;  // esp_err_t
};

// After a verified, marked migration: erases exactly kid_pet/pet_a,
// kid_pet/pet_b and kid_parent/settings (missing = done). Never opens a
// namespace for writing that does not exist; never touches other keys.
class NvsLegacyRelease final : public PetLedgerMigration::LegacyRelease {
public:
    bool Release() override;
    int LastError() const { return last_error_; }

private:
    int last_error_ = 0;  // esp_err_t
};

// Default NVS partition usage, for the boot log (nvs_get_stats and
// per-namespace entry counts; 0 for a namespace that does not exist).
struct NvsUsage {
    size_t used = 0, free = 0, total = 0, namespaces = 0;
    size_t ledger = 0, legacy_pet = 0, legacy_parent = 0;
};
bool ReadNvsUsage(NvsUsage* out);
const char* NvsErrorName(int err);

}  // namespace kid_app
