#include "kid_app/nvs_bridge_blob_store.h"

#include "esp_err.h"
#include "nvs.h"

namespace kid_app {
namespace {
constexpr const char* kNamespace = "kid_bridge";
constexpr const char* kKeys[2] = {"outbox_a", "outbox_b"};
}  // namespace

BlobReadStatus NvsBridgeBlobStore::Read(unsigned slot, Blob* out) {
    if (!out || slot > 1) return BlobReadStatus::Error;
    nvs_handle_t handle;
    const esp_err_t opened = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (opened == ESP_ERR_NVS_NOT_FOUND) return BlobReadStatus::NotFound;
    if (opened != ESP_OK) return BlobReadStatus::Error;
    size_t length = 0;
    const esp_err_t queried = nvs_get_blob(handle, kKeys[slot], nullptr, &length);
    if (queried == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return BlobReadStatus::NotFound;
    }
    if (queried != ESP_OK) {
        nvs_close(handle);
        return BlobReadStatus::Error;
    }
    if (length != out->size()) {
        out->fill(0);  // Load() recognizes corruption and checks the other slot.
        nvs_close(handle);
        return BlobReadStatus::Ok;
    }
    const esp_err_t read = nvs_get_blob(handle, kKeys[slot], out->data(), &length);
    nvs_close(handle);
    return read == ESP_OK ? BlobReadStatus::Ok : BlobReadStatus::Error;
}

bool NvsBridgeBlobStore::Write(unsigned slot, const Blob& value) {
    if (slot > 1) return false;
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    const esp_err_t set = nvs_set_blob(handle, kKeys[slot], value.data(), value.size());
    const esp_err_t committed = set == ESP_OK ? nvs_commit(handle) : set;
    nvs_close(handle);
    return committed == ESP_OK;
}

}  // namespace kid_app
