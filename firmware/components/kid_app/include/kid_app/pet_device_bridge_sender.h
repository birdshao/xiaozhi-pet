#pragma once

#include <cstddef>
#include <cstdint>

#include "kid_app/pet_device_bridge.h"

namespace kid_app {

// Pure coordination loop. Does not open a socket, own TLS secrets, or block
// on a FreeRTOS/EPD/audio callback. To be owned by the networking task.
class DeviceBridgeTransport {
public:
    virtual ~DeviceBridgeTransport() = default;
    // Must return true only if HTTP request completed AND a per-event ACK
    // was parsed into ack. message_id pointer remains valid through Poll().
    virtual bool PostJson(const char* body, size_t size,
                          PetDeviceBridge::ServerAck* ack) = 0;
};

enum class BridgePollResult : uint8_t {
    Idle, Waiting, Confirmed, RetryScheduled, StorageBlocked, InvalidDevice
};

class PetDeviceBridgeSender {
public:
    PetDeviceBridgeSender(PetDeviceBridge& queue, DeviceBridgeTransport& transport,
                          const char* device_id)
        : queue_(queue), transport_(transport), device_id_(device_id) {}

    // Caller supplies monotonic milliseconds. Exactly ONE pending event may
    // be attempted per call. The same JSON is retried; no new seq allocated.
    BridgePollResult Poll(uint64_t now_ms);
    uint32_t RetryDelayMs() const { return retry_ms_; }

private:
    BridgePollResult Retry(uint64_t now_ms);
    PetDeviceBridge& queue_;
    DeviceBridgeTransport& transport_;
    const char* device_id_;  // Immutable, stable for sender lifetime.
    uint64_t next_attempt_ms_ = 0;
    uint32_t retry_ms_ = 1000;
};

}  // namespace kid_app
