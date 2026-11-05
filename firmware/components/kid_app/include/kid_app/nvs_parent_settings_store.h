#pragma once

#include "kid_app/parent_settings.h"

#include <cstdint>

namespace kid_app {

// V3-06 parent settings key ('kid_parent'/'settings') the board used before
// it switched to the atomic ledger (schema v3, same ParentSettingsCodec
// bytes). The board now only reads it, once, as a PetLedgerMigration source
// and erases it once the ledger is verified (NvsLegacyRelease); Save is
// kept for older tools/tests. NVS
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
