#pragma once

#include "kid_app/pet_device_bridge.h"

namespace kid_app {

// Independent of the pet's 1064-byte checkpoint and official XiaoZhi keys.
// Requires host nvs_flash_init() to succeed; never formats or erases NVS.
class NvsBridgeBlobStore final : public PetDeviceBridge::Storage {
public:
    BlobReadStatus Read(unsigned slot, Blob* out) override;
    bool Write(unsigned slot, const Blob& value) override;
};

}  // namespace kid_app
