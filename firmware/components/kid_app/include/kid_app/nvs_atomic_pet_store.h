#pragma once

#include "kid_app/pet_atomic_ledger.h"

namespace kid_app {

// Separate opt-in A/B NVS namespace for the combined Pet/outbox transaction.
// Does not erase or silently migrate legacy 'kid_pet' / 'kid_bridge' keys.
class NvsAtomicPetStore final : public PetAtomicLedger::Store {
public:
    BlobReadStatus Read(unsigned slot, Blob* out) override;
    bool Write(unsigned slot, const Blob& blob) override;
};

}  // namespace kid_app
