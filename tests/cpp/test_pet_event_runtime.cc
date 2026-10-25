#include "kid_app/pet_event_runtime.h"

#include <cassert>
#include <iostream>

using namespace kid_app;

class TestStore : public PetBlobStore {
public:
    BlobReadStatus Read(unsigned, PetCheckpointCodec::Blob*) override {
        return BlobReadStatus::NotFound;
    }
    bool Write(unsigned, const PetCheckpointCodec::Blob&) override {
        return !blocked;
    }
    bool blocked = false;
};

int main() {
    TestStore storage;
    PetController controller;
    PetPersistence persist(storage);
    PetEventExecutor execute(controller, persist);
    PetEventRuntime<2> runtime(execute);
    PetEffects effects{};
    PetEvent finished{};

    assert(runtime.PumpOne(&effects) == PumpResult::Idle);
    assert(!runtime.Enqueue({0, PetAction::Feed}));
    assert(runtime.Enqueue({7, PetAction::Feed}));
    assert(runtime.Enqueue({8, PetAction::Pickup}));
    assert(!runtime.Enqueue({9, PetAction::Feed}));
    assert(runtime.QueuedCount() == 2);

    storage.blocked = true;
    assert(runtime.PumpOne(&effects, &finished) == PumpResult::StorageBlocked);
    assert(runtime.HasPendingRetry());
    assert(runtime.QueuedCount() == 1);
    assert(controller.Snapshot().interaction_count == 0);
    assert(!effects.play_positive_sound);

    // Retry stays ahead of other queued events and keeps exact ID.
    assert(runtime.PumpOne(&effects, &finished) == PumpResult::StorageBlocked);
    assert(runtime.QueuedCount() == 1);
    storage.blocked = false;
    assert(runtime.PumpOne(&effects, &finished) == PumpResult::Committed);
    assert(finished.id == 7 && effects.play_positive_sound);
    assert(controller.Snapshot().interaction_count == 1);
    assert(!runtime.HasPendingRetry());

    assert(runtime.PumpOne(&effects, &finished) == PumpResult::Committed);
    assert(finished.id == 8);
    assert(runtime.QueuedCount() == 0);
    assert(runtime.PumpOne(&effects) == PumpResult::Idle);
    assert(controller.Snapshot().interaction_count == 1);

    assert(runtime.Enqueue({7, PetAction::Feed}));
    assert(runtime.PumpOne(&effects, &finished) == PumpResult::Duplicate);
    assert(!effects.persist && !effects.play_positive_sound);

    // Bounded capacity + ring wrap across repeated pressure.
    for (uint64_t i = 100; i < 140; ++i) {
        assert(runtime.Enqueue({i, PetAction::Feed}));
        assert(runtime.PumpOne(&effects, &finished) == PumpResult::Committed);
        assert(finished.id == i);
    }
    assert(controller.Snapshot().interaction_count == 41);
    std::cout << "PASS: fixed-capacity FIFO, retry blocking, durable ACK and idempotent processing\n";
}
