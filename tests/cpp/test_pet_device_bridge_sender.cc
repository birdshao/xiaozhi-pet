#include "kid_app/pet_device_bridge_sender.h"

#include <cassert>
#include <iostream>
#include <string>

using namespace kid_app;

class MemoryStore final : public PetDeviceBridge::Storage {
public:
    BlobReadStatus Read(unsigned slot, Blob* out) override {
        if (slot >= 2 || !out) return BlobReadStatus::Error;
        if (!exists[slot]) return BlobReadStatus::NotFound;
        *out = snapshots[slot];
        return BlobReadStatus::Ok;
    }
    bool Write(unsigned slot, const Blob& body) override {
        if (fail || slot >= 2) return false;
        exists[slot] = true;
        snapshots[slot] = body;
        return true;
    }
    bool exists[2]{};
    bool fail = false;
    Blob snapshots[2]{};
};

class FakeTransport final : public DeviceBridgeTransport {
public:
    bool PostJson(const char* json, size_t size,
                  PetDeviceBridge::ServerAck* ack) override {
        calls++;
        const std::string body(json, size);
        if (first_body.empty()) first_body = body;
        else assert(body == first_body || alternate);
        if (offline) return false;
        auto start = body.find("\"message_id\":\"");
        assert(start != std::string::npos);
        start += std::string("\"message_id\":\"").size();
        const auto end = body.find('"', start);
        assert(end != std::string::npos);
        last_message_id = body.substr(start, end - start);
        ack->message_id = wrong_message ? "pet-other:1234" : last_message_id.c_str();
        ack->sequence = sequence;
        ack->accepted = accepted;
        ack->server_version = version;
        ack->duplicate = false;
        return true;
    }
    bool offline = true;
    bool wrong_message = false;
    bool accepted = true;
    bool alternate = false;
    unsigned calls = 0;
    uint64_t sequence = 1, version = 1;
    std::string first_body, last_message_id;
};

int main() {
    MemoryStore flash;
    PetDeviceBridge bridge(flash);
    assert(bridge.Load() == PetDeviceBridge::LoadResult::Empty);
    assert(bridge.AppendCommitted({0x8000000000000021ull, PetAction::Feed}) ==
           PetDeviceBridge::AppendResult::Queued);
    FakeTransport network;
    PetDeviceBridgeSender sender(bridge, network, "pet-unit-001");
    assert(sender.Poll(0) == BridgePollResult::RetryScheduled);
    assert(sender.RetryDelayMs() == 2000);
    assert(sender.Poll(999) == BridgePollResult::Waiting);
    assert(sender.Poll(1000) == BridgePollResult::RetryScheduled);
    assert(sender.Poll(2999) == BridgePollResult::Waiting);
    assert(network.calls == 2);
    network.offline = false;
    assert(sender.Poll(3000) == BridgePollResult::Confirmed);
    assert(network.calls == 3 && sender.RetryDelayMs() == 1000);
    assert(bridge.PendingCount() == 0);
    assert(bridge.NextSequence() == 2);
    assert(sender.Poll(3001) == BridgePollResult::Idle);

    // Reboots preserve cursor. Exactly one next sequence is allocated.
    PetDeviceBridge restarted(flash);
    assert(restarted.Load() == PetDeviceBridge::LoadResult::Ready);
    assert(restarted.NextSequence() == 2);
    assert(restarted.AppendCommitted({0x8000000000000022ull, PetAction::Feed}) ==
           PetDeviceBridge::AppendResult::Queued);

    FakeTransport other;
    other.offline = false;
    other.alternate = true;
    other.sequence = 2;
    other.wrong_message = true;
    PetDeviceBridgeSender retry(restarted, other, "pet-unit-001");
    assert(retry.Poll(0) == BridgePollResult::RetryScheduled);
    assert(restarted.PendingCount() == 1);
    other.wrong_message = false;
    other.accepted = false;  // HTTP returned a parsed negative receipt
    assert(retry.Poll(1000) == BridgePollResult::RetryScheduled);
    assert(restarted.PendingCount() == 1);
    other.accepted = true;
    flash.fail = true;  // Remote succeeded but local receipt save fails
    assert(retry.Poll(3000) == BridgePollResult::StorageBlocked);
    assert(restarted.PendingCount() == 1);
    flash.fail = false;
    assert(retry.Poll(7000) == BridgePollResult::Confirmed);
    assert(restarted.PendingCount() == 0);

    // Refuse to send to a malformed device ID.
    assert(restarted.AppendCommitted({0x8000000000000023ull, PetAction::Feed}) ==
           PetDeviceBridge::AppendResult::Queued);
    FakeTransport no_call;
    PetDeviceBridgeSender wrong(restarted, no_call, "not!a valid id");
    assert(wrong.Poll(0) == BridgePollResult::InvalidDevice);
    assert(no_call.calls == 0 && restarted.PendingCount() == 1);

    std::cout << "PASS: DeviceBridge network sender bounded retry, exact ACK and NVS failure\n";
}
