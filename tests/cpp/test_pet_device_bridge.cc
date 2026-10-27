#include "kid_app/pet_device_bridge.h"
#include "kid_app/pet_local_event_id.h"

#include <cassert>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

using namespace kid_app;
using Bridge = PetDeviceBridge;

Bridge::AckResult Ack(Bridge& bridge, uint64_t seq, uint64_t id,
                      uint64_t server_version, bool accepted, bool duplicate = false) {
    char message_id[96]{};
    const int size = std::snprintf(message_id, sizeof(message_id),
        "pet-unit-001:%016" PRIx64, id);
    assert(size > 0 && static_cast<size_t>(size) < sizeof(message_id));
    return bridge.Acknowledge(
        {message_id, seq, server_version, accepted, duplicate}, "pet-unit-001");
}

class MemoryBridgeStore final : public Bridge::Storage {
public:
    BlobReadStatus Read(unsigned slot, Blob* out) override {
        if (failed_reads) return BlobReadStatus::Error;
        if (slot >= 2 || !out) return BlobReadStatus::Error;
        if (!present[slot]) return BlobReadStatus::NotFound;
        *out = data[slot];
        return BlobReadStatus::Ok;
    }
    bool Write(unsigned slot, const Blob& value) override {
        if (failed_writes || slot >= 2) return false;
        data[slot] = value;
        present[slot] = true;
        return true;
    }
    bool failed_reads = false;
    bool failed_writes = false;
    bool present[2]{};
    Blob data[2]{};
};

void TestQueueAndNetworkRecovery() {
    MemoryBridgeStore nvs;
    Bridge first(nvs);
    assert(first.Load() == Bridge::LoadResult::Empty);
    assert(!first.Peek(nullptr));
    Bridge::Record item{};
    assert(!first.Peek(&item));
    assert(first.NextSequence() == 1);

    const auto id1 = NextLocalEventId(7);
    const auto id2 = NextLocalEventId(8);
    const auto id3 = NextLocalEventId(9);
    assert(first.AppendCommitted({id1, PetAction::Feed}) == Bridge::AppendResult::Queued);
    assert(first.AppendCommitted({id1, PetAction::Feed}) == Bridge::AppendResult::AlreadyPending);
    assert(first.AppendCommitted({id1, PetAction::Pickup}) == Bridge::AppendResult::Unsupported);
    assert(first.AppendCommitted({id2, PetAction::StartStory}) == Bridge::AppendResult::Unsupported);
    assert(first.AppendCommitted({id2, PetAction::Pickup}) == Bridge::AppendResult::Queued);
    assert(first.PendingCount() == 2 && first.NextSequence() == 3);

    // Entire logical HTTP body stays unchanged after a crash/network failure.
    assert(first.Peek(&item));
    assert(item.sequence == 1 && item.event_id == id1);
    char json[Bridge::kJsonMaxBytes]{};
    assert(Bridge::EncodePostBody(item, "pet-unit-001", json, sizeof(json)));
    const std::string body(json);
    assert(body == "{\"events\":[{\"schema_version\":\"1.0\","
                   "\"message_id\":\"pet-unit-001:8000000000000008\","
                   "\"device_id\":\"pet-unit-001\",\"seq\":1,"
                   "\"type\":\"pet.event\",\"payload\":{\"event\":\"PET_FEED\"}}]}");
    assert(!Bridge::EncodePostBody(item, "invalid\"id", json, sizeof(json)));
    assert(!Bridge::EncodePostBody(item, "ok", json, 4));
    assert(!Bridge::EncodePostBody(item, "pet-unit-001", json, 2));

    Bridge restart(nvs);
    assert(restart.Load() == Bridge::LoadResult::Ready);
    assert(restart.Peek(&item));
    assert(item.sequence == 1 && item.event_id == id1);
    assert(Bridge::EncodePostBody(item, "pet-unit-001", json, sizeof(json)));
    assert(body == json);

    // Bad HTTP reply, wrong event and reordered sequence MUST NOT erase data.
    assert(Ack(restart, 1, id1, 0, true, false) ==
           Bridge::AckResult::ServerRejected);
    assert(Ack(restart, 1, id1, 1, false, false) ==
           Bridge::AckResult::ServerRejected);
    assert(Ack(restart, 2, id1, 1, true, false) ==
           Bridge::AckResult::Mismatch);
    assert(Ack(restart, 1, id2, 1, true, false) ==
           Bridge::AckResult::Mismatch);
    assert(restart.Acknowledge({"pet-unit-OTHER:8000000000000008", 1, 1, true, false},
                               "pet-unit-001") == Bridge::AckResult::Mismatch);
    assert(restart.PendingCount() == 2);

    // Losing local NVS ACK write must NOT remove the pending head.
    nvs.failed_writes = true;
    assert(Ack(restart, 1, id1, 1, true, false) ==
           Bridge::AckResult::StorageError);
    assert(restart.Peek(&item) && item.sequence == 1);
    nvs.failed_writes = false;
    assert(Ack(restart, 1, id1, 1, true, false) ==
           Bridge::AckResult::Confirmed);
    // Simulate lost HTTP ACK in the following request: duplicate is accepted
    // by the server with the original version (not a new reward).
    assert(Ack(restart, 2, id2, 2, true, true) ==
           Bridge::AckResult::Confirmed);
    assert(restart.PendingCount() == 0 && restart.NextSequence() == 3);
    assert(Ack(restart, 1, id1, 1, true, true) ==
           Bridge::AckResult::NoPending);

    Bridge after_ack(nvs);
    assert(after_ack.Load() == Bridge::LoadResult::Ready);
    assert(after_ack.PendingCount() == 0 && after_ack.NextSequence() == 3);
    assert(after_ack.AppendCommitted({id3, PetAction::QuietBegin}) ==
           Bridge::AppendResult::Queued);
    assert(after_ack.Peek(&item) && item.sequence == 3);
    assert(Bridge::EncodePostBody(item, "pet-unit-001", json, sizeof(json)));
    assert(std::strstr(json, "QUIET_HOURS_START") != nullptr);
}

void TestFullAndDurableWriteErrors() {
    MemoryBridgeStore store;
    Bridge queue(store);
    assert(queue.Load() == Bridge::LoadResult::Empty);
    store.failed_writes = true;
    assert(queue.AppendCommitted({111, PetAction::Feed}) ==
           Bridge::AppendResult::StorageError);
    assert(queue.NextSequence() == 1 && queue.PendingCount() == 0);
    store.failed_writes = false;
    for (uint64_t i = 1; i <= Bridge::kCapacity; ++i) {
        assert(queue.AppendCommitted({100 + i, PetAction::Feed}) ==
               Bridge::AppendResult::Queued);
    }
    assert(queue.PendingCount() == Bridge::kCapacity);
    assert(queue.AppendCommitted({999, PetAction::Feed}) == Bridge::AppendResult::Full);
    assert(queue.NextSequence() == Bridge::kCapacity + 1);

    Bridge restart(store);
    assert(restart.Load() == Bridge::LoadResult::Ready);
    assert(restart.PendingCount() == Bridge::kCapacity);
    for (uint64_t i = 1; i <= Bridge::kCapacity; ++i) {
        Bridge::Record r{};
        assert(restart.Peek(&r) && r.sequence == i);
        assert(Ack(restart, i, 100 + i, i, true, false) ==
               Bridge::AckResult::Confirmed);
    }
    assert(restart.PendingCount() == 0);
}

void TestCorruptedNewSlotFallbackAndUnknownReadFailure() {
    MemoryBridgeStore store;
    Bridge first(store);
    assert(first.Load() == Bridge::LoadResult::Empty);
    assert(first.AppendCommitted({101, PetAction::Feed}) == Bridge::AppendResult::Queued);
    assert(first.AppendCommitted({102, PetAction::Pickup}) == Bridge::AppendResult::Queued);
    // Slot 1 was newest; corruption falls back to slot 0, which still
    // contains the prior durable item (never invent a successful ACK).
    store.data[1][70] ^= 0x44;
    Bridge fallback(store);
    assert(fallback.Load() == Bridge::LoadResult::Ready);
    assert(fallback.PendingCount() == 1);
    Bridge::Record item{};
    assert(fallback.Peek(&item) && item.event_id == 101);
    store.failed_reads = true;
    Bridge blocked(store);
    assert(blocked.Load() == Bridge::LoadResult::StorageError);
    assert(blocked.AppendCommitted({103, PetAction::Feed}) == Bridge::AppendResult::StorageError);
    store.failed_reads = false;

    MemoryBridgeStore corrupt;
    corrupt.present[0] = true;
    corrupt.data[0].fill(0);
    Bridge refuses(corrupt);
    assert(refuses.Load() == Bridge::LoadResult::Corrupt);
}

int main() {
    TestQueueAndNetworkRecovery();
    TestFullAndDurableWriteErrors();
    TestCorruptedNewSlotFallbackAndUnknownReadFailure();
    std::cout << "PASS: durable Pet DeviceBridge A/B outbox, offline replay and ACK discipline\n";
}
