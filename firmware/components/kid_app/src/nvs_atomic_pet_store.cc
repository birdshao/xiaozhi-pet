#include "kid_app/nvs_atomic_pet_store.h"

#include "esp_err.h"
#include "nvs.h"

namespace kid_app {
namespace {
constexpr const char* kNamespace = "kid_pet_sync";  // 12 <= NVS max 15 bytes.
constexpr const char* kKeys[2] = {"joint_a", "joint_b"};
}

BlobReadStatus NvsAtomicPetStore::Read(unsigned slot, Blob* out) {
    if (!out || slot>1) return BlobReadStatus::Error;
    nvs_handle_t nvs;
    const esp_err_t opened=nvs_open(kNamespace,NVS_READONLY,&nvs);
    if (opened==ESP_ERR_NVS_NOT_FOUND) return BlobReadStatus::NotFound;
    if (opened!=ESP_OK) return BlobReadStatus::Error;
    size_t len=0;
    const esp_err_t queried=nvs_get_blob(nvs,kKeys[slot],nullptr,&len);
    if (queried==ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(nvs);
        return BlobReadStatus::NotFound;
    }
    if (queried!=ESP_OK) {
        nvs_close(nvs);
        return BlobReadStatus::Error;
    }
    if (len!=out->size()) {
        out->fill(0);  // wrong size -> corrupt; do not erase NVS.
        nvs_close(nvs);
        return BlobReadStatus::Ok;
    }
    const esp_err_t read=nvs_get_blob(nvs,kKeys[slot],out->data(),&len);
    nvs_close(nvs);
    return read==ESP_OK ? BlobReadStatus::Ok : BlobReadStatus::Error;
}

bool NvsAtomicPetStore::Write(unsigned slot, const Blob& blob) {
    if (slot>1) return false;
    nvs_handle_t nvs;
    if (nvs_open(kNamespace,NVS_READWRITE,&nvs)!=ESP_OK) return false;
    const esp_err_t set=nvs_set_blob(nvs,kKeys[slot],blob.data(),blob.size());
    const esp_err_t commit=set==ESP_OK ? nvs_commit(nvs) : set;
    nvs_close(nvs);
    return commit==ESP_OK;
}

}  // namespace kid_app
