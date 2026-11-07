#include "kid_app/freertos_server_sync_worker.h"

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"
#include "sdkconfig.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <new>
#include <strings.h>

namespace kid_app {
namespace {
constexpr const char* kTag = "kid_srv";
constexpr const char* kNamespace = "kid_srv";
constexpr uint32_t kStackBytes = 8192;
constexpr UBaseType_t kEventCapacity = 16;
constexpr uint32_t kTickMs = 1000;
constexpr uint32_t kPollMs = 120000;        // GET commands when nothing else is sent
constexpr uint32_t kStaleMs = 60000;        // an outcome that never came back
constexpr uint32_t kConfirmRetryMs = 10000;
constexpr uint32_t kRotateCheckMs = 6 * 3600 * 1000u;
constexpr uint32_t kRotateRetryMs = 3600 * 1000u;
constexpr int64_t kRotateAfterS = 30LL * 24 * 3600;  // S1: rotate about monthly
constexpr uint8_t kAuthFailuresForRepair = 3;
constexpr int kHttpTimeoutMs = 8000;

uint32_t NowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

bool Reached(uint32_t now_ms, uint32_t at_ms) { return static_cast<int32_t>(now_ms - at_ms) >= 0; }

// Where esp_http_client writes one reply (body and the two headers we read).
struct HttpSink {
    char* buffer = nullptr;
    size_t capacity = 0;
    size_t length = 0;
    bool overflow = false;
    uint32_t retry_after_s = 0;
    bool have_date = false;
    int64_t date = 0;
};

esp_err_t OnHttpEvent(esp_http_client_event_t* event) {
    if (!event || !event->user_data) return ESP_OK;
    auto* sink = static_cast<HttpSink*>(event->user_data);
    if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key && event->header_value) {
        if (strcasecmp(event->header_key, "Retry-After") == 0) {
            const long value = std::strtol(event->header_value, nullptr, 10);
            sink->retry_after_s = value > 0 && value <= 3600 ? static_cast<uint32_t>(value) : 0;
        } else if (strcasecmp(event->header_key, "Date") == 0) {
            sink->have_date = ParseHttpDate(event->header_value, &sink->date);
        }
        return ESP_OK;
    }
    if (event->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    if (event->data_len < 0 || (!event->data && event->data_len)) {
        sink->overflow = true;
        return ESP_OK;
    }
    const size_t size = static_cast<size_t>(event->data_len);
    if (sink->overflow || size + 1 > sink->capacity - sink->length) {
        sink->overflow = true;  // keep draining; the reply is not used
        return ESP_OK;
    }
    if (size) std::memcpy(sink->buffer + sink->length, event->data, size);
    sink->length += size;
    sink->buffer[sink->length] = '\0';
    return ESP_OK;
}

bool ReadStr(nvs_handle_t handle, const char* key, char* out, size_t capacity) {
    size_t size = capacity;
    if (nvs_get_str(handle, key, out, &size) != ESP_OK) {
        out[0] = '\0';
        return false;
    }
    return true;
}

const char* StateResultName(StateResult result) {
    switch (result) {
        case StateResult::Accepted: return "accepted";
        case StateResult::Duplicate: return "duplicate";
        case StateResult::Stale: return "stale";
        case StateResult::GenerationConflict: return "generation_conflict";
    }
    return "unknown";
}
}  // namespace

void FreertosServerSyncWorker::Attach() {
    pet_.SetCommandCallback(&FreertosServerSyncWorker::OnCommandOutcome, this);
}

bool FreertosServerSyncWorker::Start(const ServerSyncHooks& hooks, const char* fw_version) {
    if (task_) return false;
    if (!buf_) {
        void* memory = heap_caps_calloc(1, sizeof(Buffers), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!memory) memory = heap_caps_calloc(1, sizeof(Buffers), MALLOC_CAP_8BIT);
        if (!memory) return false;
        buf_ = new (memory) Buffers();
    }
    if (!requests_mutex_) requests_mutex_ = xSemaphoreCreateMutex();
    if (!events_) events_ = xQueueCreate(kEventCapacity, sizeof(Event));
    if (!requests_mutex_ || !events_) return false;
    hooks_ = hooks;
    SanitizeFwVersion(fw_version, fw_version_, sizeof(fw_version_));
    if (xTaskCreate(&FreertosServerSyncWorker::Entry, "kid_srv", kStackBytes, this, 2, &task_) !=
        pdPASS) {
        task_ = nullptr;
        return false;
    }
    return true;
}

void FreertosServerSyncWorker::Entry(void* context) {
    static_cast<FreertosServerSyncWorker*>(context)->Run();
}

void FreertosServerSyncWorker::OnCommandOutcome(const DeviceCommand& command,
                                                CommandOutcome outcome, void* context) {
    auto* self = static_cast<FreertosServerSyncWorker*>(context);
    if (!self || !self->events_) return;
    Event event{};
    event.kind = EventKind::Outcome;
    event.command = command;
    event.outcome = outcome;
    // Never block the Pet worker. A lost outcome is recovered on the next
    // delivery (SweepStale, then the ledger's replay window).
    if (xQueueSend(self->events_, &event, 0) != pdTRUE)
        ESP_LOGW(kTag, "PET_SYNC_OUTCOME_DROPPED");
}

// ---- input task -------------------------------------------------------------

void FreertosServerSyncWorker::BeginPairing() {
    link_bits_.store(PackLinkView(LinkView{LinkState::Connecting, 0}));
    pair_requested_.store(true);
}

size_t FreertosServerSyncWorker::WaitingRequests() const {
    if (!requests_mutex_) return 0;
    xSemaphoreTake(requests_mutex_, portMAX_DELAY);
    const size_t n = requests_.Waiting();
    xSemaphoreGive(requests_mutex_);
    return n;
}

bool FreertosServerSyncWorker::NextRequest(DeviceCommand* out, uint8_t* more) const {
    if (!requests_mutex_) return false;
    xSemaphoreTake(requests_mutex_, portMAX_DELAY);
    const bool found = requests_.Next(out);
    const size_t waiting = requests_.Waiting();
    xSemaphoreGive(requests_mutex_);
    if (more) *more = static_cast<uint8_t>(found && waiting ? waiting - 1 : 0);
    return found;
}

bool FreertosServerSyncWorker::ConfirmRequest(uint64_t id) {
    if (!requests_mutex_) return false;
    DeviceCommand command{};
    bool confirming = false;
    int64_t utc = 0;
    const bool clock = UtcNow(&utc);
    xSemaphoreTake(requests_mutex_, portMAX_DELAY);
    const bool found = requests_.Find(id, &command, &confirming);
    // Expired: the network task reports it; never run it late.
    const bool usable = found && !confirming && !CommandExpired(command, clock, utc);
    if (usable) requests_.SetConfirming(id, true);
    xSemaphoreGive(requests_mutex_);
    if (!usable) return false;
    confirm_posted_ms_.store(NowMs());
    if (pet_.PostCommand(command, true)) return true;
    xSemaphoreTake(requests_mutex_, portMAX_DELAY);
    requests_.SetConfirming(id, false);
    xSemaphoreGive(requests_mutex_);
    return false;
}

bool FreertosServerSyncWorker::RejectRequest(uint64_t id) {
    if (!requests_mutex_ || !events_) return false;
    Event event{};
    event.kind = EventKind::Reject;
    bool confirming = false;
    xSemaphoreTake(requests_mutex_, portMAX_DELAY);
    const bool usable = requests_.Find(id, &event.command, &confirming) && !confirming;
    if (usable) requests_.SetRejecting(id);  // hidden while the refusal is stored
    xSemaphoreGive(requests_mutex_);
    if (!usable) return false;
    confirm_posted_ms_.store(NowMs());
    if (xQueueSend(events_, &event, 0) == pdTRUE) return true;
    xSemaphoreTake(requests_mutex_, portMAX_DELAY);
    requests_.SetConfirming(id, false);
    xSemaphoreGive(requests_mutex_);
    return false;
}

// ---- clock --------------------------------------------------------------------

bool FreertosServerSyncWorker::UtcNow(int64_t* out) const {
    taskENTER_CRITICAL(&clock_lock_);
    const bool known = utc_known_;
    const int64_t base = utc_base_, mono = mono_base_us_;
    taskEXIT_CRITICAL(&clock_lock_);
    if (!known || !out) return false;
    *out = base + (esp_timer_get_time() - mono) / 1000000;
    return true;
}

void FreertosServerSyncWorker::SetUtc(int64_t utc) {
    const int64_t mono = esp_timer_get_time();
    taskENTER_CRITICAL(&clock_lock_);
    utc_known_ = true;
    utc_base_ = utc;
    mono_base_us_ = mono;
    taskEXIT_CRITICAL(&clock_lock_);
}

// ---- NVS ('kid_srv', owned by this task) --------------------------------------

void FreertosServerSyncWorker::LoadNvs() {
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READONLY, &handle) == ESP_OK) {
        ReadStr(handle, "dev_id", device_id_, sizeof(device_id_));
        ReadStr(handle, "token", token_, sizeof(token_));
        uint8_t flag = 0;
        confirmed_ = nvs_get_u8(handle, "confirmed", &flag) == ESP_OK && flag == 1;
        int64_t rotated = 0;
        if (nvs_get_i64(handle, "rot_at", &rotated) == ESP_OK) rotated_at_ = rotated;
        ReadStr(handle, "origin", origin_, sizeof(origin_));
        uint8_t blob[CommandTracker::kRejectedBlobBytes]{};
        size_t size = sizeof(blob);
        if (nvs_get_blob(handle, "rejected", blob, &size) == ESP_OK &&
            !tracker_.DecodeRejected(blob, size))
            ESP_LOGW(kTag, "PET_SYNC_REJECTED_LIST_CORRUPT");
        uint8_t applied[CommandTracker::kAppliedBlobBytes]{};
        size = sizeof(applied);
        if (nvs_get_blob(handle, "applied", applied, &size) == ESP_OK &&
            !tracker_.DecodeApplied(applied, size))
            ESP_LOGW(kTag, "PET_SYNC_APPLIED_LIST_CORRUPT");
        uint64_t seq = 0;
        if (nvs_get_u64(handle, "seq_temp", &seq) == ESP_OK)
            tracker_.SetAppliedSeq(CommandKind::SetTemperament, seq);
        nvs_close(handle);
    }
    have_credentials_ = ValidDeviceId(device_id_) && ValidDeviceToken(token_);
    if (!have_credentials_) {
        std::memset(token_, 0, sizeof(token_));
        confirmed_ = false;
    }
    origin_ok_ = ResolveOrigin();
    ESP_LOGI(kTag, "PET_SYNC_BOOT paired=%d confirmed=%d origin=%s origin_ok=%d",
             have_credentials_ ? 1 : 0, confirmed_ ? 1 : 0, origin_[0] ? origin_ : "-",
             origin_ok_ ? 1 : 0);
}

bool FreertosServerSyncWorker::ResolveOrigin() {
    if (!origin_[0]) {
        // The XiaoZhi OTA URL points at the same gateway (DEPLOY_PET_SERVER).
        char url[200]{};
        nvs_handle_t handle = 0;
        if (nvs_open("wifi", NVS_READONLY, &handle) == ESP_OK) {
            ReadStr(handle, "ota_url", url, sizeof(url));
            nvs_close(handle);
        }
#ifdef CONFIG_OTA_URL
        if (!url[0]) std::snprintf(url, sizeof(url), "%s", CONFIG_OTA_URL);
#endif
        if (!OriginOfUrl(url, origin_, sizeof(origin_))) origin_[0] = '\0';
    }
    const bool ok = AllowedServerOrigin(origin_);
    // Plain http (e.g. a LAN OTA URL) is refused: set kid_srv/origin to the
    // public https origin, or point the OTA URL at it.
    if (origin_[0] && !ok) ESP_LOGW(kTag, "PET_SYNC_ORIGIN_REFUSED origin=%s", origin_);
    return ok;
}

bool FreertosServerSyncWorker::SaveCredentials(const char* device_id, const char* token,
                                               bool confirmed) {
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    const bool ok = nvs_set_str(handle, "dev_id", device_id) == ESP_OK &&
                    nvs_set_str(handle, "token", token) == ESP_OK &&
                    nvs_set_u8(handle, "confirmed", confirmed ? 1 : 0) == ESP_OK &&
                    nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    if (!ok) ESP_LOGW(kTag, "PET_SYNC_NVS_WRITE_FAILED what=credentials");
    return ok;
}

void FreertosServerSyncWorker::EraseCredentials() {
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) == ESP_OK) {
        for (const char* key : {"dev_id", "token", "confirmed", "rot_at"}) nvs_erase_key(handle, key);
        nvs_commit(handle);
        nvs_close(handle);
    }
    std::memset(device_id_, 0, sizeof(device_id_));
    std::memset(token_, 0, sizeof(token_));
    have_credentials_ = false;
    confirmed_ = false;
    rotated_at_ = 0;
    ESP_LOGI(kTag, "PET_SYNC_CREDENTIALS_ERASED");
}

bool FreertosServerSyncWorker::SaveApplied() {
    uint8_t blob[CommandTracker::kAppliedBlobBytes];
    tracker_.EncodeApplied(blob);
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    const bool ok = nvs_set_blob(handle, "applied", blob, sizeof(blob)) == ESP_OK &&
                    nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    if (!ok) ESP_LOGW(kTag, "PET_SYNC_NVS_WRITE_FAILED what=applied");
    return ok;
}

bool FreertosServerSyncWorker::SaveAppliedSeq(uint64_t seq) {
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    const bool ok = nvs_set_u64(handle, "seq_temp", seq) == ESP_OK && nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    if (!ok) ESP_LOGW(kTag, "PET_SYNC_NVS_WRITE_FAILED what=seq");
    return ok;
}

bool FreertosServerSyncWorker::SaveRejected() {
    uint8_t blob[CommandTracker::kRejectedBlobBytes];
    tracker_.EncodeRejected(blob);
    nvs_handle_t handle = 0;
    if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    const bool ok = nvs_set_blob(handle, "rejected", blob, sizeof(blob)) == ESP_OK &&
                    nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return ok;
}

// ---- HTTP -----------------------------------------------------------------------

int FreertosServerSyncWorker::Request(const char* method, const char* path, const char* body,
                                      size_t size, const char* bearer, Http* out) {
    *out = Http{};
    char url[256];
    const int url_len = std::snprintf(url, sizeof(url), "%s%s", origin_, path);
    if (!origin_ok_ || url_len <= 0 || static_cast<size_t>(url_len) >= sizeof(url)) return 0;
    const bool https = std::strncmp(origin_, "https://", 8) == 0;
#if !defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE) || !CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    if (https) return 0;  // never TLS without a trusted chain
#endif
    HttpSink sink{};
    sink.buffer = buf_->reply;
    sink.capacity = sizeof(buf_->reply);
    buf_->reply[0] = '\0';
    esp_http_client_config_t config{};
    config.url = url;
    config.method = std::strcmp(method, "PUT") == 0   ? HTTP_METHOD_PUT
                    : std::strcmp(method, "GET") == 0 ? HTTP_METHOD_GET
                                                      : HTTP_METHOD_POST;
    config.event_handler = &OnHttpEvent;
    config.user_data = &sink;
    config.timeout_ms = kHttpTimeoutMs;
    config.disable_auto_redirect = true;
    config.max_authorization_retries = -1;
#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE) && CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    if (https) {
        config.crt_bundle_attach = esp_crt_bundle_attach;
        config.skip_cert_common_name_check = false;
    }
#endif
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return 0;
    char auth[kTokenMax + 8];
    bool setup = true;
    if (body) {
        setup = esp_http_client_set_header(client, "Content-Type", "application/json") == ESP_OK &&
                esp_http_client_set_post_field(client, body, static_cast<int>(size)) == ESP_OK;
    }
    if (bearer) {
        std::snprintf(auth, sizeof(auth), "Bearer %s", bearer);
        setup = setup && esp_http_client_set_header(client, "Authorization", auth) == ESP_OK;
    }
    if (setup && esp_http_client_perform(client) == ESP_OK)
        out->status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    std::memset(auth, 0, sizeof(auth));
    out->length = sink.length;
    out->overflow = sink.overflow;
    out->retry_after_s = sink.retry_after_s;
    out->have_date = sink.have_date;
    out->date = sink.date;
    if (out->status && sink.have_date) SetUtc(sink.date);
    return out->status;
}

// Error replies carry {"detail": "<CODE>"} (never secrets): log the code.
void FreertosServerSyncWorker::LogDetail(const char* what, const Http& http) {
    char detail[48] = "-";
    if (http.length && !http.overflow && buf_->doc.Parse(buf_->reply, http.length))
        buf_->doc.String(buf_->doc.Get(buf_->doc.Root(), "detail"), detail, sizeof(detail));
    ESP_LOGW(kTag, "PET_SYNC_HTTP_FAILED what=%s status=%d detail=%s", what, http.status, detail);
}

// ---- main loop ------------------------------------------------------------------

void FreertosServerSyncWorker::Run() {
    // Timer targets start "now": a zero target would read as "not reached"
    // after 24.8 days of uptime (int32 wrap).
    next_rotate_check_ms_ = NowMs();
    next_confirm_ms_ = NowMs();
    LoadNvs();
    applied_saved_rev_ = tracker_.Revision();
    PublishLink();
    Event event{};
    for (;;) {
        if (xQueueReceive(events_, &event, pdMS_TO_TICKS(kTickMs)) == pdTRUE) {
            HandleEvent(event, NowMs());
            while (xQueueReceive(events_, &event, 0) == pdTRUE) HandleEvent(event, NowMs());
        }
        const uint32_t now = NowMs();
        if (pair_requested_.exchange(false)) {
            if (!origin_ok_) origin_ok_ = ResolveOrigin();
            if (origin_ok_) {
                uint8_t mac[6]{};
                esp_read_mac(mac, ESP_MAC_WIFI_STA);
                // A fresh id per pairing: S1 never re-pairs a known (or revoked) id.
                std::snprintf(pending_id_, sizeof(pending_id_), "tt-%02x%02x%02x%02x%02x%02x-%04x",
                              mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                              static_cast<unsigned>(esp_random() & 0xFFFF));
                pairing_.Begin(now);
                ESP_LOGI(kTag, "PET_PAIR_BEGIN device=%s", pending_id_);
            } else {
                ESP_LOGW(kTag, "PET_PAIR_NO_SERVER");
            }
            PublishLink();
        }
        if (pairing_.Active()) {
            // Deadlines run offline too, so the pairing page cannot hang.
            pairing_.Tick(now);
            if (!pairing_.Active()) {
                if (pairing_.DiscardStored()) EraseCredentials();
                std::memset(pending_token_, 0, sizeof(pending_token_));
                PublishLink();
            }
        }
        const bool online = hooks_.online && hooks_.online(hooks_.context);
        if (!online || !origin_ok_) {
            was_online_ = false;
            continue;
        }
        if (!was_online_) {
            was_online_ = true;
            schedule_.OnConnected(now);
            next_poll_ms_ = now + kPollMs;
        }
        if (pairing_.Active()) {
            RunPairing(now);
            continue;
        }
        if (!have_credentials_ || needs_repair_) continue;
        if (!confirmed_) {
            ConfirmStored(now);
            continue;
        }
        SweepStale(now);
        int64_t utc = 0;
        const bool clock = UtcNow(&utc);
        uint64_t expired[ParentRequestQueue::kCapacity]{};
        xSemaphoreTake(requests_mutex_, portMAX_DELAY);
        const size_t expired_count =
            requests_.RemoveExpired(clock, utc, expired, ParentRequestQueue::kCapacity);
        const uint32_t requests_rev = requests_.Revision();
        xSemaphoreGive(requests_mutex_);
        for (size_t i = 0; i < expired_count; ++i) {
            tracker_.AddReceipt(expired[i], ReceiptResult::Expired);
            ESP_LOGI(kTag, "PET_SYNC_REQUEST_EXPIRED id=%" PRIu64, expired[i]);
        }
        if (tracker_.Revision() != applied_saved_rev_) {
            applied_saved_rev_ = tracker_.Revision();
            SaveApplied();
        }
        const uint64_t generation = pet_.GenerationNow();
        if (generation != seen_generation_ || tracker_.Revision() != seen_receipts_rev_ ||
            requests_rev != seen_requests_rev_) {
            seen_generation_ = generation;
            seen_receipts_rev_ = tracker_.Revision();
            seen_requests_rev_ = requests_rev;
            schedule_.Trigger(now);
        }
        MaybeRotate(now);
        if (needs_repair_) continue;
        const bool new_generation = generation != accepted_generation_;
        if (schedule_.Due(now, new_generation)) {
            Upload(now);
            next_poll_ms_ = NowMs() + kPollMs;
        } else if (Reached(now, next_poll_ms_)) {
            PollCommands(now);
            next_poll_ms_ = NowMs() + kPollMs;
        }
    }
}

void FreertosServerSyncWorker::HandleEvent(const Event& event, uint32_t now_ms) {
    const uint64_t id = event.command.id;
    if (event.kind == EventKind::Reject) {
        // Stored first (S2): after a reboot a redelivery is refused at once.
        tracker_.AddRejected(id);
        if (!SaveRejected()) ESP_LOGW(kTag, "PET_SYNC_NVS_WRITE_FAILED what=rejected");
        xSemaphoreTake(requests_mutex_, portMAX_DELAY);
        requests_.Remove(id);
        xSemaphoreGive(requests_mutex_);
        tracker_.AddReceipt(id, ReceiptResult::Rejected);
        ESP_LOGI(kTag, "PET_SYNC_REQUEST_REJECTED id=%" PRIu64, id);
        return;
    }
    tracker_.ClearInFlight(id);
    ReceiptResult receipt = ReceiptResult::Rejected;
    uint64_t dropped[ParentRequestQueue::kCapacity + 1]{};
    size_t dropped_count = 0;
    xSemaphoreTake(requests_mutex_, portMAX_DELAY);
    if (ReceiptForOutcome(event.outcome, &receipt)) {
        requests_.Remove(id);
        // A newer command of this class was applied: older waiting requests
        // can never be confirmed any more (the parent would undo the newer one).
        if (event.outcome == CommandOutcome::Applied &&
            tracker_.NoteApplied(event.command.kind, event.command.seq)) {
            dropped_count = requests_.DropStale(event.command.kind, event.command.seq, dropped,
                                                ParentRequestQueue::kCapacity);
            SaveAppliedSeq(event.command.seq);
        }
    } else if (event.outcome == CommandOutcome::NeedsConfirm) {
        uint64_t replaced = 0;
        if (tracker_.Stale(event.command)) {
            dropped[dropped_count++] = id;  // older than what was applied
        } else {
            switch (requests_.Offer(event.command, &replaced)) {
                case ParentRequestQueue::OfferResult::Replaced:
                    dropped[dropped_count++] = replaced;
                    break;
                case ParentRequestQueue::OfferResult::Stale:
                    dropped[dropped_count++] = id;  // a newer request is already waiting
                    break;
                case ParentRequestQueue::OfferResult::Refused:
                    ESP_LOGW(kTag, "PET_SYNC_REQUESTS_FULL");
                    break;
                case ParentRequestQueue::OfferResult::Added:
                case ParentRequestQueue::OfferResult::Known:
                    break;
            }
        }
    } else {
        requests_.SetConfirming(id, false);  // storage failed: ask again later
    }
    xSemaphoreGive(requests_mutex_);
    if (ReceiptForOutcome(event.outcome, &receipt)) tracker_.AddReceipt(id, receipt);
    for (size_t i = 0; i < dropped_count; ++i) {
        // Superseded: refused for good (stored, so a redelivery is not asked).
        tracker_.AddRejected(dropped[i]);
        tracker_.AddReceipt(dropped[i], ReceiptResult::Rejected);
        ESP_LOGI(kTag, "PET_SYNC_REQUEST_SUPERSEDED id=%" PRIu64, dropped[i]);
    }
    if (dropped_count && !SaveRejected()) ESP_LOGW(kTag, "PET_SYNC_NVS_WRITE_FAILED what=rejected");
    ESP_LOGI(kTag, "PET_SYNC_COMMAND id=%" PRIu64 " kind=%u outcome=%s", id,
             static_cast<unsigned>(event.command.kind), CommandOutcomeName(event.outcome));
    (void)now_ms;
}

void FreertosServerSyncWorker::SweepStale(uint32_t now_ms) {
    if (tracker_.InFlightCount() && now_ms - last_post_ms_ > kStaleMs) {
        ESP_LOGW(kTag, "PET_SYNC_INFLIGHT_RESET n=%u", static_cast<unsigned>(tracker_.InFlightCount()));
        tracker_.ClearAllInFlight();
    }
    const uint32_t posted = confirm_posted_ms_.load();
    if (posted && now_ms - posted > kStaleMs) {
        // The outcome never came back (a full event queue). Post the
        // confirmed commands again: the ledger answers a repeat as already
        // applied, so nothing runs twice and nothing is reported "expired".
        DeviceCommand again[ParentRequestQueue::kCapacity]{};
        xSemaphoreTake(requests_mutex_, portMAX_DELAY);
        const size_t n = requests_.Confirming(again, ParentRequestQueue::kCapacity);
        xSemaphoreGive(requests_mutex_);
        for (size_t i = 0; i < n; ++i) {
            if (pet_.PostCommand(again[i], true)) confirm_posted_ms_.store(now_ms ? now_ms : 1);
        }
        if (n) ESP_LOGW(kTag, "PET_SYNC_CONFIRM_REPOSTED n=%u", static_cast<unsigned>(n));
    }
}

void FreertosServerSyncWorker::RouteCommands(const DeviceCommand* commands, size_t count,
                                             uint32_t now_ms) {
    int64_t utc = 0;
    const bool clock = UtcNow(&utc);
    for (size_t i = 0; i < count; ++i) {
        const DeviceCommand& command = commands[i];
        xSemaphoreTake(requests_mutex_, portMAX_DELAY);
        const bool awaiting = requests_.Contains(command.id);
        xSemaphoreGive(requests_mutex_);
        const CommandDecision decision = RouteCommand(command, tracker_, awaiting, clock, utc);
        switch (decision.route) {
            case CommandRoute::Skip:
                break;
            case CommandRoute::Answer:
                tracker_.AddReceipt(command.id, decision.receipt);
                ESP_LOGI(kTag, "PET_SYNC_COMMAND id=%" PRIu64 " kind=%u answer=%s", command.id,
                         static_cast<unsigned>(command.kind), ReceiptName(decision.receipt));
                break;
            case CommandRoute::ToPet:
                if (!tracker_.MarkInFlight(command.id)) break;
                if (pet_.PostCommand(command, false)) {
                    last_post_ms_ = now_ms;
                } else {
                    tracker_.ClearInFlight(command.id);
                    ESP_LOGW(kTag, "PET_SYNC_COMMAND_INBOX_FULL");
                }
                break;
        }
    }
}

void FreertosServerSyncWorker::OnAuthResult(int status) {
    if (status == 200) {
        auth_failures_ = 0;
        return;
    }
    if (status != 401) return;
    if (++auth_failures_ >= kAuthFailuresForRepair && !needs_repair_) {
        // S1: revoked or unknown token. Show "pair again"; never re-pair alone.
        needs_repair_ = true;
        ESP_LOGW(kTag, "PET_SYNC_AUTH_REFUSED needs_repair=1");
        PublishLink();
    }
}

void FreertosServerSyncWorker::Upload(uint32_t now_ms) {
    PetSyncView& view = buf_->view;
    if (!pet_.SyncViewNow(&view) || !view.generation) return;
    StateSnapshotInput& in = buf_->input;
    in = StateSnapshotInput{};
    in.generation = view.generation;
    in.fw_version = fw_version_;
    in.rtc_day = hooks_.local_day ? hooks_.local_day(hooks_.context) : 0;
    in.pet = view.pet;
    in.memory = view.memory;
    in.parent = view.parent;
    if (hooks_.battery) hooks_.battery(hooks_.context, &in.battery_pct, &in.charging);
    if (in.battery_pct > 100) in.battery_pct = 100;
    xSemaphoreTake(requests_mutex_, portMAX_DELAY);
    in.awaiting_count = static_cast<uint8_t>(requests_.Ids(in.awaiting, kMaxAwaitingConfirm));
    const uint32_t requests_rev = requests_.Revision();
    xSemaphoreGive(requests_mutex_);
    in.receipt_count = static_cast<uint8_t>(tracker_.Receipts(in.receipts, kMaxReceipts));
    const size_t size = EncodeStateSnapshot(in, buf_->body, sizeof(buf_->body));
    const bool new_generation = view.generation != accepted_generation_;
    if (!size) {
        ESP_LOGE(kTag, "PET_SYNC_SNAPSHOT_ENCODE_FAILED gen=%" PRIu64, view.generation);
        schedule_.OnUploaded(now_ms, false, new_generation, 0);
        return;
    }
    char path[96];
    std::snprintf(path, sizeof(path), "/v1/devices/%s/state", device_id_);
    Http http{};
    Request("PUT", path, buf_->body, size, token_, &http);
    StateReply& reply = buf_->state;
    if (http.status == 200 && !http.overflow &&
        ParseStateReply(buf_->reply, http.length, &buf_->doc, &reply)) {
        OnAuthResult(200);
        if (reply.result == StateResult::Accepted || reply.result == StateResult::Duplicate) {
            tracker_.OnAcked(reply.acked, reply.acked_count);
            accepted_generation_ = view.generation;
        }
        // The upload's own bookkeeping is not a reason to upload again.
        seen_receipts_rev_ = tracker_.Revision();
        seen_requests_rev_ = requests_rev;
        schedule_.OnUploaded(NowMs(), true, new_generation, 0);
        ESP_LOGI(kTag, "PET_SYNC_STATE result=%s gen=%" PRIu64 " bytes=%u receipts=%u acked=%u "
                 "awaiting=%u commands=%u",
                 StateResultName(reply.result), view.generation, static_cast<unsigned>(size),
                 static_cast<unsigned>(in.receipt_count), static_cast<unsigned>(reply.acked_count),
                 static_cast<unsigned>(in.awaiting_count),
                 static_cast<unsigned>(reply.command_count));
        RouteCommands(reply.commands, reply.command_count, NowMs());
        return;
    }
    OnAuthResult(http.status);
    LogDetail("state", http);
    // 413 / 422: the server will refuse this exact content again; wait for
    // the next change instead of hammering it.
    const bool permanent = http.status == 413 || http.status == 422;
    schedule_.OnUploaded(NowMs(), permanent, permanent ? false : new_generation,
                         http.retry_after_s);
}

void FreertosServerSyncWorker::PollCommands(uint32_t now_ms) {
    char path[96];
    std::snprintf(path, sizeof(path), "/v1/devices/%s/commands", device_id_);
    Http http{};
    Request("GET", path, nullptr, 0, token_, &http);
    uint8_t count = 0;
    if (http.status == 200 && !http.overflow &&
        ParseCommandsReply(buf_->reply, http.length, &buf_->doc, buf_->polled,
                           kMaxCommandsPerReply, &count)) {
        OnAuthResult(200);
        if (count) ESP_LOGI(kTag, "PET_SYNC_POLL commands=%u", static_cast<unsigned>(count));
        RouteCommands(buf_->polled, count, now_ms);
        return;
    }
    OnAuthResult(http.status);
    LogDetail("commands", http);
}

// ---- pairing and credentials ------------------------------------------------------

void FreertosServerSyncWorker::RunPairing(uint32_t now_ms) {
    char body[192];
    Http http{};
    switch (pairing_.Step(now_ms)) {
        case PairingFlow::Action::None:
            break;
        case PairingFlow::Action::Start: {
            const int n = std::snprintf(body, sizeof(body), "{\"device_id\":\"%s\"}", pending_id_);
            Request("POST", "/v1/pairing/start", body, static_cast<size_t>(n), nullptr, &http);
            PairingStartReply reply{};
            const bool parsed = http.status == 200 && !http.overflow &&
                                ParsePairingStart(buf_->reply, http.length, &buf_->doc, &reply);
            pairing_.OnStart(parsed ? 200 : (http.status == 200 ? 0 : http.status),
                             parsed ? &reply : nullptr, NowMs());
            std::memset(&reply, 0, sizeof(reply));
            ESP_LOGI(kTag, "PET_PAIR_START status=%d", http.status);
            break;
        }
        case PairingFlow::Action::Claim: {
            const int n = std::snprintf(body, sizeof(body),
                                        "{\"device_id\":\"%s\",\"claim_secret\":\"%s\"}",
                                        pending_id_, pairing_.ClaimSecret());
            Request("POST", "/v1/pairing/claim", body, static_cast<size_t>(n), nullptr, &http);
            ClaimStatus status = ClaimStatus::Pending;
            const bool parsed = http.status == 200 && !http.overflow &&
                                ParsePairingClaim(buf_->reply, http.length, &buf_->doc, pending_id_,
                                                  &status, pending_token_, sizeof(pending_token_));
            // The reply held the token: do not leave it in the buffer.
            std::memset(buf_->reply, 0, sizeof(buf_->reply));
            pairing_.OnClaim(http.status, parsed, status, NowMs());
            if (http.status != 200 || status == ClaimStatus::Claimed)
                ESP_LOGI(kTag, "PET_PAIR_CLAIM status=%d claimed=%d", http.status,
                         parsed && status == ClaimStatus::Claimed ? 1 : 0);
            break;
        }
        case PairingFlow::Action::Save: {
            const bool saved = SaveCredentials(pending_id_, pending_token_, false);
            if (saved) {
                std::memcpy(device_id_, pending_id_, sizeof(device_id_));
                std::memcpy(token_, pending_token_, sizeof(token_));
                have_credentials_ = true;
                confirmed_ = false;
                needs_repair_ = false;
                auth_failures_ = 0;
            }
            std::memset(pending_token_, 0, sizeof(pending_token_));
            pairing_.OnSaved(saved, NowMs());
            ESP_LOGI(kTag, "PET_PAIR_SAVED ok=%d", saved ? 1 : 0);
            break;
        }
        case PairingFlow::Action::Confirm: {
            const int n = std::snprintf(body, sizeof(body), "{\"device_id\":\"%s\"}", device_id_);
            Request("POST", "/v1/pairing/confirm", body, static_cast<size_t>(n), token_, &http);
            pairing_.OnConfirm(http.status, NowMs());
            ESP_LOGI(kTag, "PET_PAIR_CONFIRM status=%d", http.status);
            if (http.status == 200) {
                confirmed_ = true;
                SaveCredentials(device_id_, token_, true);
                // A new device on the server: its generation cursor is empty.
                accepted_generation_ = 0;
                rotated_at_ = 0;
                next_rotate_check_ms_ = NowMs();
                schedule_.OnConnected(NowMs());
            }
            break;
        }
    }
    if (!pairing_.Active()) {
        if (pairing_.DiscardStored()) EraseCredentials();
        std::memset(pending_token_, 0, sizeof(pending_token_));
        ESP_LOGI(kTag, "PET_PAIR_DONE state=%u", static_cast<unsigned>(pairing_.View().state));
    }
    PublishLink();
}

// Credentials written but the confirm never answered (reboot mid-pairing).
void FreertosServerSyncWorker::ConfirmStored(uint32_t now_ms) {
    if (!Reached(now_ms, next_confirm_ms_)) return;
    next_confirm_ms_ = now_ms + kConfirmRetryMs;
    char body[96];
    const int n = std::snprintf(body, sizeof(body), "{\"device_id\":\"%s\"}", device_id_);
    Http http{};
    Request("POST", "/v1/pairing/confirm", body, static_cast<size_t>(n), token_, &http);
    if (http.status != 200 && http.status != 0) {
        // Maybe it is active already (the confirmed flag was not stored).
        char path[96];
        std::snprintf(path, sizeof(path), "/v1/devices/%s/commands", device_id_);
        Request("GET", path, nullptr, 0, token_, &http);
    }
    ESP_LOGI(kTag, "PET_PAIR_CONFIRM_STORED status=%d", http.status);
    if (http.status == 200) {
        confirmed_ = true;
        SaveCredentials(device_id_, token_, true);
        schedule_.OnConnected(NowMs());
    } else if (http.status == 401 || http.status == 404) {
        EraseCredentials();  // the server dropped the unconfirmed device
    }
    PublishLink();
}

void FreertosServerSyncWorker::MaybeRotate(uint32_t now_ms) {
    if (!Reached(now_ms, next_rotate_check_ms_)) return;
    next_rotate_check_ms_ = now_ms + kRotateCheckMs;
    int64_t utc = 0;
    if (!UtcNow(&utc)) {
        next_rotate_check_ms_ = now_ms + 60000;
        return;
    }
    nvs_handle_t handle = 0;
    if (!rotated_at_) {  // first check after pairing: start counting now
        rotated_at_ = utc;
        if (nvs_open(kNamespace, NVS_READWRITE, &handle) == ESP_OK) {
            nvs_set_i64(handle, "rot_at", utc);
            nvs_commit(handle);
            nvs_close(handle);
        }
        return;
    }
    if (utc - rotated_at_ < kRotateAfterS) return;
    char path[112];
    std::snprintf(path, sizeof(path), "/v1/devices/%s/token/rotate", device_id_);
    Http http{};
    Request("POST", path, nullptr, 0, token_, &http);
    char fresh[kTokenMax + 1]{};
    if (http.status == 200 && !http.overflow &&
        ParseTokenRotate(buf_->reply, http.length, &buf_->doc, fresh, sizeof(fresh))) {
        std::memset(buf_->reply, 0, sizeof(buf_->reply));
        // Stored first; only then does the old token stop working (confirm).
        bool stored = SaveCredentials(device_id_, fresh, true);
        if (stored && nvs_open(kNamespace, NVS_READWRITE, &handle) == ESP_OK) {
            nvs_set_i64(handle, "rot_at", utc);
            nvs_commit(handle);
            nvs_close(handle);
        }
        if (stored) {
            std::memcpy(token_, fresh, sizeof(token_));
            rotated_at_ = utc;
            std::snprintf(path, sizeof(path), "/v1/devices/%s/token/confirm", device_id_);
            Request("POST", path, nullptr, 0, token_, &http);
            ESP_LOGI(kTag, "PET_TOKEN_ROTATED confirm=%d", http.status);
        } else {
            // The old token stays valid for 24 h (S1); after that: pair again.
            ESP_LOGW(kTag, "PET_TOKEN_ROTATE_STORE_FAILED");
            next_rotate_check_ms_ = now_ms + kRotateRetryMs;
        }
        std::memset(fresh, 0, sizeof(fresh));
        return;
    }
    OnAuthResult(http.status);
    if (http.status == 409) {
        rotated_at_ = utc;  // ROTATE_WITH_CURRENT_TOKEN (via_previous): do not retry
        ESP_LOGW(kTag, "PET_TOKEN_ROTATE_REFUSED status=409");
    } else {
        LogDetail("rotate", http);
        next_rotate_check_ms_ = now_ms + kRotateRetryMs;
    }
}

void FreertosServerSyncWorker::PublishLink() {
    const LinkView flow = pairing_.View();
    LinkView view{};
    if (pairing_.Active()) view = flow;
    else if (!origin_ok_) view.state = LinkState::NoServer;
    else if (needs_repair_) view.state = LinkState::NeedsRepair;
    else if (have_credentials_ && confirmed_) view.state = LinkState::Paired;
    else if (flow.state == LinkState::Failed || flow.state == LinkState::NoServer) view = flow;
    else if (have_credentials_) view.state = LinkState::Saving;
    link_bits_.store(PackLinkView(view));
}

}  // namespace kid_app
