#include "kid_app/pet_persistence.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>

using namespace kid_app;

struct SimulatedNvs : PetBlobStore {
    PetCheckpointCodec::Blob slot[2]{};
    bool present[2]{false, false};
    bool fail_write = false;
    bool fail_read_slot_one = false;

    BlobReadStatus Read(unsigned i, PetCheckpointCodec::Blob* dst) override {
        if (i > 1 || !dst || (fail_read_slot_one && i == 1)) return BlobReadStatus::Error;
        if (!present[i]) return BlobReadStatus::NotFound;
        *dst = slot[i];
        return BlobReadStatus::Ok;
    }
    bool Write(unsigned i, const PetCheckpointCodec::Blob& blob) override {
        if (i > 1 || fail_write) return false;  // Torn write never replaces old slot.
        slot[i] = blob;
        present[i] = true;
        return true;
    }
};

static void action(PetController& pet, uint64_t id, PetAction a) {
    PetEffects effects{};
    assert(pet.Apply({id, a}, &effects) == PetResult::Applied);
    assert(effects.persist);
}

int main() {
    static_assert(PetCheckpointCodec::kBytes == 1064);
    SimulatedNvs nvs;
    PetController pet;
    PetPersistence store(nvs);
    assert(store.Load(&pet) == PetLoadResult::NotFound);

    // Save A: one food event and persistent event ID.
    action(pet, 10, PetAction::Feed);
    assert(store.Save(pet));
    assert(store.Generation() == 1 && store.ActiveSlot() == 0);
    PetCheckpoint decoded{};
    uint64_t generation = 0;
    assert(PetCheckpointCodec::Decode(nvs.slot[0], &decoded, &generation));
    assert(generation == 1);
    assert(decoded.snapshot.interaction_count == 1);
    assert(decoded.seen[0] == 10);

    // Save B after another event, never overwrite last confirmed A.
    action(pet, 11, PetAction::Feed);
    assert(store.Save(pet));
    assert(store.Generation() == 2 && store.ActiveSlot() == 1);
    assert(nvs.present[0] && nvs.present[1]);

    // Two-slot recovery from reset: later valid B wins; replay ID 10/11 blocked.
    PetController restarted;
    PetPersistence reread(nvs);
    assert(reread.Load(&restarted) == PetLoadResult::Restored);
    assert(restarted.Snapshot().interaction_count == 2);
    PetEffects effect{};
    assert(restarted.Apply({10, PetAction::Feed}, &effect) == PetResult::Duplicate);
    assert(restarted.Apply({11, PetAction::Feed}, &effect) == PetResult::Duplicate);
    assert(!effect.persist);
    assert(restarted.Snapshot().interaction_count == 2);

    // Corrupt newer B; falls back to prior A without changing unrelated flash.
    nvs.slot[1][75] ^= 0x80;
    PetController fallback;
    PetPersistence recovered(nvs);
    assert(recovered.Load(&fallback) == PetLoadResult::Restored);
    assert(recovered.Generation() == 1 && recovered.ActiveSlot() == 0);
    assert(fallback.Snapshot().interaction_count == 1);
    assert(fallback.Apply({10, PetAction::Feed}, &effect) == PetResult::Duplicate);
    // Re-commits into B, keeping A as old good slot.
    assert(recovered.Save(fallback));
    assert(recovered.Generation() == 2);
    assert(PetCheckpointCodec::Decode(nvs.slot[1], &decoded, &generation));

    // Simulate a power cut before commit: old B survives and generation unchanged.
    action(fallback, 12, PetAction::Feed);
    nvs.fail_write = true;
    assert(!recovered.Save(fallback));
    assert(recovered.Generation() == 2);
    PetController after_failed_commit;
    PetPersistence read_again(nvs);
    assert(read_again.Load(&after_failed_commit) == PetLoadResult::Restored);
    assert(after_failed_commit.Snapshot().interaction_count == 1);
    nvs.fail_write = false;
    assert(recovered.Save(fallback));
    assert(recovered.Generation() == 3);

    // Counter errors vs corruption vs absent: don't overwrite state on failure.
    SimulatedNvs broken;
    PetController unchanged;
    action(unchanged, 20, PetAction::Feed);
    PetPersistence empty(broken);
    assert(empty.Load(&unchanged) == PetLoadResult::NotFound);
    assert(unchanged.Snapshot().interaction_count == 1);
    broken.present[0] = true;
    broken.slot[0].fill(0);
    assert(empty.Load(&unchanged) == PetLoadResult::Corrupt);
    assert(unchanged.Snapshot().interaction_count == 1);
    broken.fail_read_slot_one = true;
    assert(empty.Load(&unchanged) == PetLoadResult::StorageError);

    // Valid CRC does not make unsupported versions or enum values acceptable.
    auto bad = nvs.slot[0];
    bad[4] = 2;
    assert(!PetCheckpointCodec::Decode(bad, &decoded, &generation));
    bad = nvs.slot[0];
    bad[40] ^= 0x01;
    assert(!PetCheckpointCodec::Decode(bad, &decoded, &generation));
    assert(!PetCheckpointCodec::Decode(nvs.slot[0], nullptr, &generation));
    assert(!PetCheckpointCodec::Decode(nvs.slot[0], &decoded, nullptr));

    // Voice sessions never re-open automatically across reboot.
    PetController in_voice;
    action(in_voice, 100, PetAction::VoiceBegin);
    SimulatedNvs voice_nvs;
    PetPersistence voice_store(voice_nvs);
    assert(voice_store.Save(in_voice));
    PetController voice_restored;
    PetPersistence voice_reader(voice_nvs);
    assert(voice_reader.Load(&voice_restored) == PetLoadResult::Restored);
    assert(voice_restored.Snapshot().mode == PetMode::Idle);
    assert(voice_restored.Snapshot().mood == PetMood::Neutral);
    assert(voice_restored.Apply({100, PetAction::VoiceBegin}, &effect) == PetResult::Duplicate);

    // Normal noncritical updates can be throttled, forced critical ones cannot.
    SimulatedNvs timer_nvs;
    PetPersistence timed(timer_nvs);
    assert(timed.SaveIfDue(pet, 1000, 5000, false));
    assert(!timed.SaveIfDue(pet, 2000, 5000, false));
    assert(timed.Generation() == 1);
    assert(timed.SaveIfDue(pet, 2001, 5000, true));
    assert(timed.Generation() == 2);
    assert(timed.SaveIfDue(pet, 7002, 5000, false));

    // 128-ID replay window is preserved across wraparound of the ring.
    PetController window;
    for (uint64_t i = 1; i <= 137; ++i) action(window, i, PetAction::Feed);
    SimulatedNvs window_nvs;
    PetPersistence window_store(window_nvs);
    assert(window_store.Save(window));
    PetController window_restored;
    PetPersistence window_reader(window_nvs);
    assert(window_reader.Load(&window_restored) == PetLoadResult::Restored);
    assert(window_restored.Apply({10, PetAction::Feed}, &effect) == PetResult::Duplicate);
    assert(window_restored.Apply({1, PetAction::Feed}, &effect) == PetResult::Applied);

    std::cout << "PASS: CRC, version, dual-slot fallback, torn commit, idempotency, quiet/voice-safe reboot, throttle\n";
}
