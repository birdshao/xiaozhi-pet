#pragma once

#include "kid_app/freertos_atomic_pet_service.h"
#include "kid_app/parent_requests.h"
#include "kid_app/server_sync.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace kid_app {

// What the board lends the network task: Wi-Fi state, the RTC day and the
// last battery reading. All three are called on the network task and must
// only read atomics (never I2C, never the ledger).
struct ServerSyncHooks {
    bool (*online)(void* context) = nullptr;
    uint32_t (*local_day)(void* context) = nullptr;
    void (*battery)(void* context, uint8_t* pct, bool* charging) = nullptr;
    void* context = nullptr;
};

// ESP-only: the device side of the PetLearning server (V5_V6_SERVER_PLAN
// §5.1-5.4, §11.2). One dedicated task owns HTTP, the 'kid_srv' NVS
// namespace (device id, token, rejected requests) and the command
// bookkeeping; it never touches PetAtomicLedger:
//   - snapshots are built from FreertosAtomicPetService::SyncViewNow copies;
//   - delivered commands are copied into the Pet task's inbox (PostCommand)
//     and their outcomes come back through OnCommandOutcome;
//   - requests waiting for the parent (D5) sit in a mutex-guarded queue the
//     input task reads for the parent entry and the home hint.
// No token, claim secret or pairing code is ever logged.
class FreertosServerSyncWorker {
public:
    explicit FreertosServerSyncWorker(FreertosAtomicPetService& pet) : pet_(pet) {}
    FreertosServerSyncWorker(const FreertosServerSyncWorker&) = delete;
    FreertosServerSyncWorker& operator=(const FreertosServerSyncWorker&) = delete;

    // Call before pet.Start(): registers the command callback.
    void Attach();
    // After pet.Start() succeeded: loads 'kid_srv' and starts the task.
    bool Start(const ServerSyncHooks& hooks, const char* fw_version);

    // Pet worker -> network task (copy only).
    static void OnCommandOutcome(const DeviceCommand& command, CommandOutcome outcome,
                                 void* context);

    // ---- input task (parent entry) ----
    // Parent pressed on the server page: (re)pair with a new code. Stored
    // credentials are only replaced once the new token is claimed and saved.
    void BeginPairing();
    LinkView Link() const { return UnpackLinkView(link_bits_.load()); }
    size_t WaitingRequests() const;
    // Oldest undecided request and how many more wait behind it.
    bool NextRequest(DeviceCommand* out, uint8_t* more) const;
    // Confirm: the Pet task commits it with MemorySource::Parent. Reject: the
    // network task stores the refusal in NVS, then reports "rejected".
    // False if the request is gone (expired, answered) or the inbox is full.
    bool ConfirmRequest(uint64_t id);
    // Before power-off: asks the network task for one last snapshot and
    // waits up to timeout_ms for it (returns at once when it cannot sync).
    bool FlushBeforePowerOff(uint32_t timeout_ms);
    bool RejectRequest(uint64_t id);

private:
    enum class EventKind : uint8_t { Outcome, Reject };
    struct Event {
        EventKind kind = EventKind::Outcome;
        DeviceCommand command{};
        CommandOutcome outcome = CommandOutcome::StorageError;
    };
    // Large buffers, allocated once (PSRAM when there is some).
    struct Buffers {
        JsonDoc doc;
        char body[kSnapshotMaxBytes + 64];
        char reply[12288];
        StateSnapshotInput input;
        PetSyncView view;
        StateReply state;
        DeviceCommand polled[kMaxCommandsPerReply];
    };
    struct Http {
        int status = 0;
        size_t length = 0;
        bool overflow = false;
        uint32_t retry_after_s = 0;
        bool have_date = false;
        int64_t date = 0;
    };

    static void Entry(void* context);
    void Run();
    void HandleEvent(const Event& event, uint32_t now_ms);
    void LoadNvs();
    bool SaveRejected();
    bool SaveApplied();
    bool SaveAppliedSeq(uint64_t seq);
    bool SavePending();
    void ErasePending();
    bool PromotePending();
    bool SaveToken();
    void SetRepairFlag(uint8_t value);
    void EnterRepair();
    bool CanSync() const;
    void ConfirmPending(uint32_t now_ms);
    bool ResolveOrigin();
    // method "GET"/"POST"/"PUT"; bearer null = no Authorization.
    int Request(const char* method, const char* path, const char* body, size_t size,
                const char* bearer, Http* out);
    void RunPairing(uint32_t now_ms);
    void Upload(uint32_t now_ms);
    void PollCommands(uint32_t now_ms);
    void MaybeRotate(uint32_t now_ms);
    void RouteCommands(const DeviceCommand* commands, size_t count, uint32_t now_ms);
    void OnAuthResult(int status);
    void SweepStale(uint32_t now_ms);
    void PublishLink();
    bool UtcNow(int64_t* out) const;
    void SetUtc(int64_t utc);
    void LogDetail(const char* what, const Http& http);

    FreertosAtomicPetService& pet_;
    ServerSyncHooks hooks_{};
    QueueHandle_t events_ = nullptr;
    TaskHandle_t task_ = nullptr;
    mutable SemaphoreHandle_t requests_mutex_ = nullptr;
    ParentRequestQueue requests_;          // under requests_mutex_
    std::atomic<bool> pair_requested_{false};
    std::atomic<uint32_t> link_bits_{0};
    std::atomic<uint32_t> confirm_posted_ms_{0};  // input task: last Confirm/Reject
    Buffers* buf_ = nullptr;

    // ---- network task only ----
    CommandTracker tracker_;
    UploadSchedule schedule_;
    PairingFlow pairing_;
    char fw_version_[41]{};
    char origin_[160]{};
    char device_id_[kDeviceIdMax + 1]{};
    char token_[kTokenMax + 1]{};          // never logged
    bool have_credentials_ = false;
    bool needs_repair_ = false;
    bool origin_ok_ = false;
    char bound_origin_[160]{};            // origin the stored token was issued for
    bool repair_notice_ = false;          // 24 h left on the previous token: pair again
    bool pending_loaded_ = false;         // boot found an unconfirmed pairing in NVS
    bool token_dirty_ = false;            // a rotated token not yet stored
    bool flush_pending_ = false;
    bool stack_logged_ = false;
    std::atomic<bool> flush_requested_{false};
    std::atomic<bool> flush_done_{true};
    char pending_id_[kDeviceIdMax + 1]{};  // id of the pairing in progress
    char pending_token_[kTokenMax + 1]{};  // claimed, not yet stored
    uint8_t auth_failures_ = 0;
    int64_t rotated_at_ = 0;               // UTC; 0 = not yet recorded
    uint32_t next_rotate_check_ms_ = 0;
    uint32_t next_confirm_ms_ = 0;
    uint32_t next_poll_ms_ = 0;
    uint32_t last_post_ms_ = 0;
    bool was_online_ = false;
    uint64_t accepted_generation_ = 0;     // last generation the server took
    uint64_t seen_generation_ = 0;
    uint32_t seen_receipts_rev_ = 0;
    uint32_t applied_saved_rev_ = 0;
    uint32_t seen_requests_rev_ = 0;
    // UTC from the server's Date header against the monotonic clock: XiaoZhi
    // sets the system clock to local time, so time() is not UTC. Read by the
    // input task too (request expiry), hence the spinlock.
    mutable portMUX_TYPE clock_lock_ = portMUX_INITIALIZER_UNLOCKED;
    bool utc_known_ = false;      // under clock_lock_
    int64_t utc_base_ = 0;        // under clock_lock_
    int64_t mono_base_us_ = 0;    // under clock_lock_
};

}  // namespace kid_app
