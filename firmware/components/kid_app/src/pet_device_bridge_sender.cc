#include "kid_app/pet_device_bridge_sender.h"

#include <cstring>
#include <limits>

namespace kid_app {

BridgePollResult PetDeviceBridgeSender::Retry(uint64_t now_ms) {
    const uint64_t max = std::numeric_limits<uint64_t>::max();
    next_attempt_ms_ = now_ms > max - retry_ms_ ? max : now_ms + retry_ms_;
    if (retry_ms_ < 60000) {
        retry_ms_ = retry_ms_ > 30000 ? 60000 : retry_ms_ * 2;
    }
    return BridgePollResult::RetryScheduled;
}

BridgePollResult PetDeviceBridgeSender::Poll(uint64_t now_ms) {
    PetDeviceBridge::Record record{};
    if (!queue_.Peek(&record)) return BridgePollResult::Idle;
    if (now_ms < next_attempt_ms_) return BridgePollResult::Waiting;

    char body[PetDeviceBridge::kJsonMaxBytes]{};
    if (!PetDeviceBridge::EncodePostBody(record, device_id_, body, sizeof(body)))
        return BridgePollResult::InvalidDevice;

    PetDeviceBridge::ServerAck receipt{};
    if (!transport_.PostJson(body, std::strlen(body), &receipt))
        return Retry(now_ms);

    switch (queue_.Acknowledge(receipt, device_id_)) {
        case PetDeviceBridge::AckResult::Confirmed:
            retry_ms_ = 1000;
            next_attempt_ms_ = 0;
            return BridgePollResult::Confirmed;
        case PetDeviceBridge::AckResult::StorageError:
            // Even a successful remote ACK cannot erase our local head
            // until the A/B NVS snapshot commit succeeds.
            Retry(now_ms);
            return BridgePollResult::StorageBlocked;
        case PetDeviceBridge::AckResult::NoPending:
        case PetDeviceBridge::AckResult::Mismatch:
        case PetDeviceBridge::AckResult::ServerRejected:
            return Retry(now_ms);
    }
    return Retry(now_ms);
}

}  // namespace kid_app
