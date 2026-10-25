#pragma once

#include <cstdint>

namespace kid_app {

// Reserve bit 63 for device-local input IDs, lower 63 bits for an increasing
// generation tied to the durable checkpoint. External/network IDs must keep
// bit 63 clear and preserve their original ID across retries.
constexpr uint64_t kLocalEventBit = uint64_t{1} << 63;
constexpr uint64_t kLocalMaxGeneration = kLocalEventBit - 2;

constexpr uint64_t NextLocalEventId(uint64_t committed_generation) {
    if (committed_generation > kLocalMaxGeneration) return 0;
    return kLocalEventBit | (committed_generation + 1);
}

constexpr bool IsExternalEventId(uint64_t id) {
    return id != 0 && (id & kLocalEventBit) == 0;
}

}  // namespace kid_app
