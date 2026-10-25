#pragma once

#include "kid_app/pet_persistence.h"

namespace kid_app {

// Requires the host app to have initialized ESP-IDF NVS flash first.
// Does not own nvs_flash_init(), deinit(), partition formatting, or erasure.
// Deliberately avoids any runtime interaction with existing XiaoZhi keys.
class NvsPetBlobStore final : public PetBlobStore {
public:
    BlobReadStatus Read(unsigned slot, PetCheckpointCodec::Blob* out) override;
    bool Write(unsigned slot, const PetCheckpointCodec::Blob& value) override;
};

} // namespace kid_app
