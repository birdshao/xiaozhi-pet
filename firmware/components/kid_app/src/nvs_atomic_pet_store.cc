#include "kid_app/nvs_atomic_pet_store.h"

#include "esp_err.h"
#include "nvs.h"

namespace kid_app {
namespace {
constexpr const char* kNamespace = "kid_pet_sync";  // 12 <= NVS max 15 bytes.
constexpr const char* kKeys[2] = {"joint_a", "joint_b"};
}

BlobReadStatus NvsAtomicPetStore::Read(unsigned slot, Blob* out,
                                       size_t* length) {
    if (!out || !length || slot>1) return BlobReadStatus::Error;
    nvs_handle_t nvs;
    const esp_err_t opened=nvs_open(kNamespace,NVS_READONLY,&nvs);
    if (opened==ESP_ERR_NVS_NOT_FOUND) return BlobReadStatus::NotFound;
    if (opened!=ESP_OK) {
        last_error_=opened;
        return BlobReadStatus::Error;
    }
    size_t len=0;
    const esp_err_t queried=nvs_get_blob(nvs,kKeys[slot],nullptr,&len);
    if (queried==ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(nvs);
        return BlobReadStatus::NotFound;
    }
    if (queried!=ESP_OK) {
        nvs_close(nvs);
        last_error_=queried;
        return BlobReadStatus::Error;
    }
    // v3, or a v2/v1 blob awaiting migration (read into the prefix).
    if (len!=out->size() && len!=PetAtomicLedger::kV2Bytes &&
        len!=PetAtomicLedger::kV1Bytes) {
        out->fill(0);  // wrong size -> corrupt; do not erase NVS.
        *length=out->size();
        nvs_close(nvs);
        return BlobReadStatus::Ok;
    }
    out->fill(0);
    const esp_err_t read=nvs_get_blob(nvs,kKeys[slot],out->data(),&len);
    nvs_close(nvs);
    *length=len;
    if (read!=ESP_OK) last_error_=read;
    return read==ESP_OK ? BlobReadStatus::Ok : BlobReadStatus::Error;
}

// Returns ESP_OK or the first failing call's error (open, set, commit).
static esp_err_t SetBlob(const char* ns, const char* key, const void* data, size_t size) {
    nvs_handle_t nvs;
    const esp_err_t opened=nvs_open(ns,NVS_READWRITE,&nvs);
    if (opened!=ESP_OK) return opened;
    const esp_err_t set=nvs_set_blob(nvs,key,data,size);
    const esp_err_t commit=set==ESP_OK ? nvs_commit(nvs) : set;
    nvs_close(nvs);
    return commit;
}

bool NvsAtomicPetStore::Write(unsigned slot, const Blob& blob) {
    if (slot>1) return false;
    // e.g. ESP_ERR_NVS_NOT_ENOUGH_SPACE: the new value of a slot needs
    // NvsBlobEntries(kBytes) free entries while the old value still exists.
    const esp_err_t result=SetBlob(kNamespace,kKeys[slot],blob.data(),blob.size());
    if (result!=ESP_OK) last_error_=result;
    return result==ESP_OK;
}

}  // namespace kid_app

namespace kid_app {
namespace {
constexpr const char* kMarkerKey = "migrated";
}

BlobReadStatus NvsLedgerMigrationMarker::Read(Blob* out, size_t* length) {
    if (!out || !length) return BlobReadStatus::Error;
    nvs_handle_t nvs;
    const esp_err_t opened=nvs_open(kNamespace,NVS_READONLY,&nvs);
    if (opened==ESP_ERR_NVS_NOT_FOUND) return BlobReadStatus::NotFound;
    if (opened!=ESP_OK) {
        last_error_=opened;
        return BlobReadStatus::Error;
    }
    size_t len=0;
    esp_err_t result=nvs_get_blob(nvs,kMarkerKey,nullptr,&len);
    out->fill(0);
    // Wrong size: report it (corrupt marker, fail closed); never erase.
    if (result==ESP_OK && len==out->size())
        result=nvs_get_blob(nvs,kMarkerKey,out->data(),&len);
    nvs_close(nvs);
    if (result==ESP_ERR_NVS_NOT_FOUND) return BlobReadStatus::NotFound;
    if (result!=ESP_OK) {
        last_error_=result;
        return BlobReadStatus::Error;
    }
    *length=len;
    return BlobReadStatus::Ok;
}

bool NvsLedgerMigrationMarker::Write(const Blob& value) {
    const esp_err_t result=SetBlob(kNamespace,kMarkerKey,value.data(),value.size());
    if (result!=ESP_OK) last_error_=result;
    return result==ESP_OK;
}

// Erases the listed keys of one namespace; a missing namespace or key is
// already released. Returns ESP_OK or the first error.
static esp_err_t EraseKeys(const char* ns, const char* const* keys, size_t count) {
    nvs_handle_t nvs;
    // Probe read-only first: opening for writing would create the namespace.
    esp_err_t result=nvs_open(ns,NVS_READONLY,&nvs);
    if (result==ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (result!=ESP_OK) return result;
    nvs_close(nvs);
    result=nvs_open(ns,NVS_READWRITE,&nvs);
    if (result!=ESP_OK) return result;
    bool erased=false;
    for (size_t i=0;i<count && result==ESP_OK;++i) {
        const esp_err_t one=nvs_erase_key(nvs,keys[i]);
        if (one==ESP_OK) erased=true;
        else if (one!=ESP_ERR_NVS_NOT_FOUND) result=one;
    }
    if (result==ESP_OK && erased) result=nvs_commit(nvs);
    nvs_close(nvs);
    return result;
}

bool NvsLegacyRelease::Release() {
    static const char* const kPetKeys[] = {"pet_a", "pet_b"};  // NvsPetBlobStore
    static const char* const kParentKeys[] = {"settings"};     // NvsParentSettingsStore
    esp_err_t result=EraseKeys("kid_pet",kPetKeys,2);
    if (result==ESP_OK) result=EraseKeys("kid_parent",kParentKeys,1);
    if (result!=ESP_OK) last_error_=result;
    return result==ESP_OK;
}

static size_t NamespaceEntries(const char* ns) {
    nvs_handle_t nvs;
    if (nvs_open(ns,NVS_READONLY,&nvs)!=ESP_OK) return 0;
    size_t used=0;
    if (nvs_get_used_entry_count(nvs,&used)!=ESP_OK) used=0;
    nvs_close(nvs);
    return used;
}

bool ReadNvsUsage(NvsUsage* out) {
    if (!out) return false;
    nvs_stats_t stats{};
    if (nvs_get_stats(nullptr,&stats)!=ESP_OK) return false;
    out->used=stats.used_entries;
    out->free=stats.free_entries;
    out->total=stats.total_entries;
    out->namespaces=stats.namespace_count;
    out->ledger=NamespaceEntries(kNamespace);
    out->legacy_pet=NamespaceEntries("kid_pet");
    out->legacy_parent=NamespaceEntries("kid_parent");
    return true;
}

const char* NvsErrorName(int err) {
    return err==ESP_OK ? "none" : esp_err_to_name(static_cast<esp_err_t>(err));
}

}  // namespace kid_app
