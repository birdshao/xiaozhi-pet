#include "kid_app/nvs_parent_settings_store.h"

#include "esp_err.h"
#include "nvs.h"

namespace kid_app {
namespace {
constexpr const char* kNamespace = "kid_parent";  // NVS max 15 characters.
constexpr const char* kKey = "settings";
}  // namespace

NvsParentSettingsStore::LoadResult NvsParentSettingsStore::Load(ParentSettings* out) {
    if (!out) return LoadResult::StorageError;
    nvs_handle_t handle;
    const esp_err_t opened = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (opened == ESP_ERR_NVS_NOT_FOUND) return LoadResult::NotFound;
    if (opened != ESP_OK) return LoadResult::StorageError;
    uint8_t bytes[ParentSettingsCodec::kBytes] = {};
    size_t len = 0;
    esp_err_t result = nvs_get_blob(handle, kKey, nullptr, &len);
    if (result == ESP_OK && len == sizeof(bytes)) result = nvs_get_blob(handle, kKey, bytes, &len);
    nvs_close(handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return LoadResult::NotFound;
    if (result != ESP_OK) return LoadResult::StorageError;
    // Wrong length, CRC or values: keep the defaults, leave NVS as it is.
    if (len != sizeof(bytes) || !ParentSettingsCodec::Decode(bytes, len, out))
        return LoadResult::Corrupt;
    return LoadResult::Loaded;
}

bool NvsParentSettingsStore::Save(const ParentSettings& settings) {
    uint8_t bytes[ParentSettingsCodec::kBytes] = {};
    if (!ParentSettingsCodec::Encode(settings, bytes, sizeof(bytes))) return false;
    nvs_handle_t handle;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    const esp_err_t set = nvs_set_blob(handle, kKey, bytes, sizeof(bytes));
    const esp_err_t committed = set == ESP_OK ? nvs_commit(handle) : set;
    nvs_close(handle);
    return committed == ESP_OK;
}

const char* ParentLoadResultName(NvsParentSettingsStore::LoadResult result) {
    switch (result) {
        case NvsParentSettingsStore::LoadResult::Loaded: return "loaded";
        case NvsParentSettingsStore::LoadResult::NotFound: return "defaults";
        case NvsParentSettingsStore::LoadResult::Corrupt: return "corrupt";
        case NvsParentSettingsStore::LoadResult::StorageError: return "error";
    }
    return "unknown";
}

}  // namespace kid_app
