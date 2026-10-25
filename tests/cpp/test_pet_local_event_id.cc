#include "kid_app/pet_local_event_id.h"
#include "kid_app/pet_controller.h"
#include "kid_app/pet_persistence.h"

#include <cassert>
#include <iostream>

using namespace kid_app;

class FakeStore : public PetBlobStore {
public:
    BlobReadStatus Read(unsigned slot, PetCheckpointCodec::Blob* out) override {
        if (slot > 1 || !present[slot]) return BlobReadStatus::NotFound;
        *out = bytes[slot]; return BlobReadStatus::Ok;
    }
    bool Write(unsigned slot, const PetCheckpointCodec::Blob& data) override {
        bytes[slot] = data; present[slot] = true; return true;
    }
    bool present[2]{};
    PetCheckpointCodec::Blob bytes[2]{};
};

int main() {
    static_assert(NextLocalEventId(0) == (uint64_t{1} << 63) + 1);
    static_assert(NextLocalEventId(kLocalMaxGeneration) != 0);
    static_assert(NextLocalEventId(kLocalMaxGeneration + 1) == 0);
    assert(!IsExternalEventId(0));
    assert(IsExternalEventId(1234));
    assert(!IsExternalEventId(NextLocalEventId(0)));

    FakeStore store;
    PetController first;
    PetPersistence writer(store);
    PetEffects e{};
    uint64_t id = NextLocalEventId(writer.Generation());
    assert(first.Apply({id, PetAction::Feed}, &e) == PetResult::Applied);
    assert(writer.Save(first));
    assert(writer.Generation() == 1);

    PetController reboot;
    PetPersistence reread(store);
    assert(reread.Load(&reboot) == PetLoadResult::Restored);
    assert(reread.Generation() == 1);
    assert(reboot.Apply({id, PetAction::Feed}, &e) == PetResult::Duplicate);
    uint64_t next = NextLocalEventId(reread.Generation());
    assert(next != id);
    assert(reboot.Apply({next, PetAction::Feed}, &e) == PetResult::Applied);
    assert(reread.Save(reboot));
    assert(reread.Generation() == 2);
    std::cout << "PASS: device-local IDs stay unique across durable generation and reboot\n";
}
