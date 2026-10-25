#include "kid_app/pet_event_executor.h"

#include <cassert>
#include <iostream>
using namespace kid_app;

struct MemoryNvs : PetBlobStore {
    PetCheckpointCodec::Blob values[2]{};
    bool present[2]{false, false};
    bool fail = false;
    BlobReadStatus Read(unsigned slot, PetCheckpointCodec::Blob* dst) override {
        if (slot >= 2) return BlobReadStatus::Error;
        if (!present[slot]) return BlobReadStatus::NotFound;
        *dst = values[slot];
        return BlobReadStatus::Ok;
    }
    bool Write(unsigned slot, const PetCheckpointCodec::Blob& v) override {
        if (fail) return false;
        values[slot] = v; present[slot] = true; return true;
    }
};

int main() {
    MemoryNvs nvs;
    PetController pet;
    PetPersistence persistence(nvs);
    PetEventExecutor executor(pet, persistence);
    PetEffects effects{};
    nvs.fail = true;
    assert(executor.ApplyDurably({100, PetAction::Feed}, &effects) == CommitResult::StorageError);
    assert(pet.Snapshot().interaction_count == 0);
    assert(!effects.persist && !effects.play_positive_sound);
    assert(persistence.Generation() == 0);
    nvs.fail = false;
    assert(executor.ApplyDurably({100, PetAction::Feed}, &effects) == CommitResult::Committed);
    assert(effects.persist && effects.play_positive_sound);
    assert(persistence.Generation() == 1);
    assert(executor.ApplyDurably({100, PetAction::Feed}, &effects) == CommitResult::Duplicate);
    assert(!effects.persist && !effects.play_positive_sound);
    assert(persistence.Generation() == 1);
    assert(executor.ApplyDurably({101, PetAction::QuietBegin}, &effects) == CommitResult::Committed);
    assert(pet.Snapshot().quiet);
    assert(executor.ApplyDurably({102, PetAction::Feed}, &effects) == CommitResult::Rejected);

    PetController rebooted;
    PetPersistence loaded(nvs);
    assert(loaded.Load(&rebooted) == PetLoadResult::Restored);
    assert(rebooted.Snapshot().interaction_count == 1 && rebooted.Snapshot().quiet);
    PetEventExecutor resumed(rebooted, loaded);
    assert(resumed.ApplyDurably({100, PetAction::Feed}, &effects) == CommitResult::Duplicate);
    assert(resumed.ApplyDurably({101, PetAction::QuietBegin}, &effects) == CommitResult::Duplicate);
    assert(rebooted.Snapshot().interaction_count == 1);
    std::cout << "PASS: durable event only ACKed/rewarded after dual-slot commit\n";
}
