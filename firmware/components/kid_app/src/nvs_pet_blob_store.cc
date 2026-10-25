#include "kid_app/nvs_pet_blob_store.h"

#include "esp_err.h"
#include "nvs.h"

namespace kid_app {
namespace {
constexpr const char* kNamespace = "kid_pet";  // NVS max 15 characters.
constexpr const char* kKeys[2] = {"pet_a", "pet_b"};
}  // namespace

BlobReadStatus NvsPetBlobStore::Read(unsigned slot,
                                    PetCheckpointCodec::Blob* out) {
    if (!out || slot > 1) return BlobReadStatus::Error;
    nvs_handle_t handle;
    const esp_err_t opened = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (opened == ESP_ERR_NVS_NOT_FOUND) return BlobReadStatus::NotFound;
    if (opened != ESP_OK) return BlobReadStatus::Error;

    size_t len = 0;
    const esp_err_t queried = nvs_get_blob(handle, kKeys[slot], nullptr, &len);
    if (queried == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return BlobReadStatus::NotFound;
    }
    if (queried != ESP_OK) {
        nvs_close(handle);
        return BlobReadStatus::Error;
    }
    // Wrong-length entries are corrupt, not I/O errors. Return invalid
    // encoded bytes so Load() can recover from the other valid slot.
    if (len != out->size()) {
        nvs_close(handle);
        out->fill(0);
        return BlobReadStatus::Ok;
    }
    const esp_err_t result = nvs_get_blob(handle, kKeys[slot], out->data(), &len);
    nvs_close(handle);
    if (result != ESP_OK) return BlobReadStatus::Error;
    return BlobReadStatus::Ok;
}

bool NvsPetBlobStore::Write(unsigned slot,
                            const PetCheckpointCodec::Blob& value) {
    if (slot > 1) return false;
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    const esp_err_t set = nvs_set_blob(handle, kKeys[slot],
                                       value.data(), value.size());
    const esp_err_t committed = (set == ESP_OK) ? nvs_commit(handle) : set;
    nvs_close(handle);
    return committed == ESP_OK;
}

}  // namespace kid_app
