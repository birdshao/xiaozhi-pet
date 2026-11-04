#pragma once

#include "kid_app/parent_settings.h"

#include <cstdint>

namespace kid_app {

// V3-06 parent settings on the board. The atomic ledger (schema v3) carries
// the same ParentSettingsCodec bytes, but the board still runs the legacy
// FreertosPetService, so until it switches to FreertosAtomicPetService the
// settings keep one NVS key of their own ('kid_parent'/'settings'). NVS
// keeps the previous value of a key if power fails during set+commit.
// Never reads or writes XiaoZhi's keys; never erases. Requires nvs_flash_init().
class NvsParentSettingsStore {
public:
    enum class LoadResult : uint8_t { Loaded, NotFound, Corrupt, StorageError };

    // *out is only written when Loaded; otherwise the caller keeps defaults.
    LoadResult Load(ParentSettings* out);
    bool Save(const ParentSettings& settings);
};

const char* ParentLoadResultName(NvsParentSettingsStore::LoadResult result);

}  // namespace kid_app
