#pragma once

#include "kid_app/care_store.h"
#include "kid_app/json_lite.h"
#include "kid_app/link_view.h"
#include "kid_app/memory_store.h"
#include "kid_app/parent_settings.h"
#include "kid_app/pet_controller.h"

#include <cstddef>
#include <cstdint>

namespace kid_app {

// Device side of the PetLearning server protocol (V5_V6_SERVER_PLAN §5.1-5.4,
// docs/SERVER_S1.md, docs/SERVER_S2.md). Pure logic, host-tested: wire
// encoding and decoding, which delivered command goes where, the pairing
// steps and the upload schedule. The ESP-only worker
// (FreertosServerSyncWorker) owns the HTTP client and NVS and feeds these.

constexpr size_t kSnapshotMaxBytes = 5120;  // server MAX_STATE_BYTES
constexpr size_t kMaxAwaitingConfirm = 8;   // server MAX_AWAITING
constexpr size_t kMaxReceipts = 16;         // server MAX_RECEIPTS
constexpr size_t kMaxCommandsPerReply = 16;  // the rest arrive with the next reply
constexpr uint64_t kMaxCommandId = (uint64_t{1} << 53) - 1;
constexpr size_t kTokenMax = 190;           // device bearer token (S1: token_urlsafe(32))
constexpr size_t kDeviceIdMax = 63;

// Standard base64 with padding. Returns the length (without the NUL it
// also writes), or 0 when the output does not fit.
size_t Base64Encode(const uint8_t* in, size_t size, char* out, size_t capacity);

// S1 device ids: ^[a-zA-Z0-9][a-zA-Z0-9_-]{2,63}$ (and at most kDeviceIdMax).
bool ValidDeviceId(const char* id);
// S1 tokens are urlsafe base64: 32..kTokenMax of [A-Za-z0-9_-].
bool ValidDeviceToken(const char* token);
// Server origin "https://host[:port]" without a path. HTTPS only (ADR:
// certificate-checked TLS, no plain http, not even on the LAN).
bool AllowedServerOrigin(const char* origin);
// fw_version for the snapshot: 1..40 of [A-Za-z0-9._+-]; anything else
// becomes '_', empty becomes "unknown".
void SanitizeFwVersion(const char* in, char* out, size_t capacity);
// A token is only ever sent to the origin it was issued for: stored with
// the credentials at pairing, compared on every boot (empty = never bound).
bool OriginBound(const char* bound, const char* current);

// Verdicts on the answers of the pairing / rotation endpoints (S1).
enum class RotateVerdict : uint8_t {
    Done,        // 200 with a usable token
    RepairSoon,  // 409 ROTATE_WITH_CURRENT_TOKEN: the current token was lost; the
                 // previous one dies within 24 h, so the parent must pair again
    AuthFailed,  // 401
    Retry,       // anything else (network, 5xx, 429, garbled reply)
};
RotateVerdict ClassifyRotate(int http_status, bool parsed);
// Only a 401 on confirm proves the server does not know the token (a 5xx or
// a timeout says nothing): the only case that may erase a pending token.
bool ConfirmRefusesToken(int http_status);

// RFC 7231 IMF-fixdate ("Sun, 06 Nov 1994 08:49:37 GMT") -> Unix seconds.
bool ParseHttpDate(const char* text, int64_t* unix_seconds);

// ---- Upstream state snapshot (S2: PUT /v1/devices/{id}/state) -------------

enum class ReceiptResult : uint8_t { Applied, Rejected, Expired };
const char* ReceiptName(ReceiptResult result);

struct CommandReceipt {
    uint64_t command_id = 0;
    ReceiptResult result = ReceiptResult::Applied;
};

// Everything a snapshot carries. generation, pet, memory and parent must be
// ONE consistent copy of the ledger (FreertosAtomicPetService::SyncViewNow):
// the server hashes them per generation and calls a mismatch a conflict.
struct StateSnapshotInput {
    uint64_t generation = 0;
    const char* fw_version = "unknown";
    uint32_t rtc_day = 0;  // local civil day (MemoryEvent::day), 0 = unknown
    PetSnapshot pet{};
    MemoryStore memory{};
    ParentSettings parent{};
    uint32_t parent_settings_rev = 0;  // ParentSettingsCodec v1 has no rev
    // V8 (ledger schema v4): earned sticker bits (CareStore::Stickers), sent
    // as the ascending list of sticker ids for the parent panel.
    uint32_t stickers = 0;
    uint8_t battery_pct = 0;
    bool charging = false;
    uint64_t awaiting[kMaxAwaitingConfirm]{};
    uint8_t awaiting_count = 0;
    CommandReceipt receipts[kMaxReceipts]{};
    uint8_t receipt_count = 0;
};

// Compact JSON exactly as the server's StateSnapshot model wants it (no
// extra keys, integers only, empty inventory, no sd_free_kib until the TF
// downloader exists). Returns the length, or 0 for invalid input (bad
// generation, settings that do not encode, too many ids, unknown sticker
// bits) or a short buffer. V8: "ledger_schema" is 4 and "stickers" (the
// earned sticker ids) is appended last; every earlier field is unchanged.
size_t EncodeStateSnapshot(const StateSnapshotInput& in, char* out, size_t capacity);

// ---- Downstream commands (S2 §5.3) -----------------------------------------

enum class CommandKind : uint8_t {
    Unknown, MemoryClear, SetTemperament, SetParentSettings, SelectVoicePack, SyncContent
};
CommandKind CommandKindFromName(const char* name);
// D5, decided by kind on the device (the server's requires_confirm is
// display only): a memory clear or a temperament change needs the parent
// to confirm it in the device parent entry.
bool NeedsParentConfirm(CommandKind kind);
// Kinds this firmware can carry out (others are answered "rejected").
bool SupportedCommand(CommandKind kind);

// Plain copy of one delivered command, small enough for FreeRTOS queues.
struct DeviceCommand {
    uint64_t id = 0;            // 1..kMaxCommandId, the ledger event id
    CommandKind kind = CommandKind::Unknown;
    bool payload_ok = false;    // payload valid for a supported kind
    Temperament temperament = Temperament::Gentle;  // SetTemperament
    uint32_t clear_epoch = 0;   // MemoryClear (informational)
    int64_t expires_at = 0;     // Unix seconds
    // Monotonic order of commands of the same kind (payload "seq", S3; 0 =
    // the server sent none). Only SetTemperament is ordered today.
    uint64_t seq = 0;
};

// Name of the ordering field in a command payload.
constexpr const char* kCommandSeqKey = "seq";
// Commands of one ordered class must be applied in seq order: a command
// whose seq is below the last applied one is dropped (answered rejected).
// -1 for kinds without an order (memory clear is idempotent).
int OrderClass(CommandKind kind);
constexpr int kOrderClasses = 1;

enum class StateResult : uint8_t { Accepted, Duplicate, Stale, GenerationConflict };

struct StateReply {
    StateResult result = StateResult::Accepted;
    uint64_t generation = 0;
    uint64_t acked[kMaxReceipts]{};
    uint8_t acked_count = 0;
    DeviceCommand commands[kMaxCommandsPerReply]{};
    uint8_t command_count = 0;
};

// 200 body of PUT state. Entries without a usable command_id are skipped
// (they cannot be answered); a bad kind/payload/expiry still yields an entry
// (payload_ok false -> answered "rejected"). Only accepted/duplicate replies
// acknowledge receipts (the server ignores them otherwise).
bool ParseStateReply(const char* body, size_t size, JsonDoc* scratch, StateReply* out);
// 200 body of GET /v1/devices/{id}/commands: {"server_rev", "commands"}.
bool ParseCommandsReply(const char* body, size_t size, JsonDoc* scratch,
                        DeviceCommand* commands, uint8_t capacity, uint8_t* count);

// ---- Pairing and token rotation (S1) ---------------------------------------

struct PairingStartReply {
    char code[7]{};             // six digits shown on the screen
    char claim_secret[65]{};    // RAM only
    uint32_t expires_in = 0;    // seconds
};
bool ParsePairingStart(const char* body, size_t size, JsonDoc* scratch, PairingStartReply* out);

enum class ClaimStatus : uint8_t { Pending, Claimed };
// "claimed" must name expected_device_id and carry a valid token.
bool ParsePairingClaim(const char* body, size_t size, JsonDoc* scratch,
                       const char* expected_device_id, ClaimStatus* status,
                       char* token, size_t token_capacity);
bool ParseTokenRotate(const char* body, size_t size, JsonDoc* scratch,
                      char* token, size_t token_capacity);

// Network task bookkeeping for delivered commands. Single owner.
//   receipts: answers to report until a reply acknowledges them;
//   in flight: posted to the Pet task, outcome not back yet;
//   rejected: refused by the parent; persisted by the owner (NVS) so a
//   redelivery after a reboot is answered without asking again.
class CommandTracker {
public:
    static constexpr size_t kRejectedCapacity = 16;
    static constexpr size_t kInFlightCapacity = 8;
    static constexpr size_t kRejectedBlobBytes = 4 + kRejectedCapacity * 8;

    // false when full (the command is answered on a later delivery).
    bool AddReceipt(uint64_t id, ReceiptResult result);
    bool HasReceipt(uint64_t id) const;
    void OnAcked(const uint64_t* ids, size_t count);
    size_t Receipts(CommandReceipt* out, size_t capacity) const;
    size_t ReceiptCount() const { return receipt_count_; }

    bool MarkInFlight(uint64_t id);
    bool InFlight(uint64_t id) const;
    void ClearInFlight(uint64_t id);
    size_t InFlightCount() const { return in_flight_count_; }
    // Outcomes that never came back (e.g. a full queue): forget them; the
    // next delivery routes the command again (the ledger catches repeats).
    void ClearAllInFlight();

    // Applied receipts survive a reboot until a reply acknowledges them (a
    // forgotten "applied" could make the parent confirm a wipe twice once the
    // 128-entry replay window has moved on). 'A' 'R' version count, ids.
    static constexpr size_t kAppliedBlobBytes = 4 + kMaxReceipts * 8;
    void EncodeApplied(uint8_t out[kAppliedBlobBytes]) const;
    bool DecodeApplied(const uint8_t* in, size_t size);  // adds Applied receipts

    // Oldest refusal is forgotten when full (its command long expired).
    void AddRejected(uint64_t id);
    bool Rejected(uint64_t id) const;
    // 'R' 'J' version count, then count little-endian ids.
    void EncodeRejected(uint8_t out[kRejectedBlobBytes]) const;
    bool DecodeRejected(const uint8_t* in, size_t size);

    // Changes whenever the receipt list changes (an upload trigger).
    uint32_t Revision() const { return revision_; }

    // Last applied seq per ordered class (persisted by the owner: load it
    // with SetAppliedSeq at boot, store it when NoteApplied returns true).
    uint64_t AppliedSeq(CommandKind kind) const;
    void SetAppliedSeq(CommandKind kind, uint64_t seq);
    bool NoteApplied(CommandKind kind, uint64_t seq);  // true = the value rose
    bool Stale(const DeviceCommand& command) const;    // seq below the applied one

private:
    uint64_t applied_seq_[kOrderClasses]{};
    CommandReceipt receipts_[kMaxReceipts]{};
    uint8_t receipt_count_ = 0;
    uint64_t in_flight_[kInFlightCapacity]{};
    uint8_t in_flight_count_ = 0;
    uint64_t rejected_[kRejectedCapacity]{};
    uint8_t rejected_count_ = 0;
    uint32_t revision_ = 0;
};

enum class CommandRoute : uint8_t {
    Skip,    // already answered, in flight or waiting for the parent
    Answer,  // report `receipt` now, nothing to run
    ToPet,   // copy into the Pet task's inbox
};
struct CommandDecision {
    CommandRoute route = CommandRoute::Skip;
    ReceiptResult receipt = ReceiptResult::Rejected;
};
// clock_known: now_utc is real Unix time. Unknown clock: only idempotent
// commands run (every supported kind is: clear, temperament).
CommandDecision RouteCommand(const DeviceCommand& command, const CommandTracker& tracker,
                             bool awaiting_parent, bool clock_known, int64_t now_utc);
bool CommandExpired(const DeviceCommand& command, bool clock_known, int64_t now_utc);

// ---- On-device pairing flow ("连接服务器" in the parent entry) --------------

// Pairing steps for the network task. Drive with the HTTP results; Step()
// says what to do next at now_ms (uint32 ms, wrap-safe).
class PairingFlow {
public:
    static constexpr uint32_t kClaimIntervalMs = 5000;  // S1: poll ~5 s
    static constexpr uint32_t kThrottledMs = 30000;     // after a 429
    static constexpr uint32_t kMaxDurationMs = 11 * 60 * 1000u;
    static constexpr uint8_t kMaxNetworkFailures = 6;
    static constexpr uint32_t kStartTimeoutMs = 60 * 1000u;  // no answer to "start"

    enum class Action : uint8_t { None, Start, Claim, Save, Confirm };

    // Starts over (a running pairing is dropped; stored credentials are
    // only replaced once a new token is claimed).
    void Begin(uint32_t now_ms);
    bool Active() const { return phase_ != Phase::Idle; }
    // The next request to make; the flow waits for its On*() result.
    Action Step(uint32_t now_ms);
    // Deadlines only (code expired, took too long, no server within
    // kStartTimeoutMs). Safe to call while offline: Step() needs the network,
    // this does not, so a pairing page can never hang forever.
    void Tick(uint32_t now_ms);

    // http_status 0 = no reply (network). Bodies are parsed by the caller:
    // reply / status are only read for a 200 that parsed.
    void OnStart(int http_status, const PairingStartReply* reply, uint32_t now_ms);
    void OnClaim(int http_status, bool parsed, ClaimStatus status, uint32_t now_ms);
    void OnSaved(bool ok, uint32_t now_ms);
    void OnConfirm(int http_status, uint32_t now_ms);

    LinkView View() const { return view_; }
    const char* ClaimSecret() const { return claim_secret_; }
    // The flow failed after the token was written: the owner erases it
    // (the server drops an unconfirmed device after 300 s anyway).
    bool DiscardStored() const { return discard_stored_; }

private:
    enum class Phase : uint8_t { Idle, Start, ShowCode, Save, Confirm };
    void Fail(LinkState state);
    // A request failed for a transient reason: try again after `wait_ms`,
    // or give up after kMaxNetworkFailures.
    void Retry(uint32_t now_ms, uint32_t wait_ms);

    Phase phase_ = Phase::Idle;
    LinkView view_{};
    uint32_t begun_ms_ = 0;
    uint32_t deadline_ms_ = 0;   // the code expires (ShowCode)
    uint32_t next_ms_ = 0;       // next claim / confirm attempt
    uint8_t failures_ = 0;
    bool waiting_ = false;       // an HTTP request is outstanding
    bool discard_stored_ = false;
    char claim_secret_[65]{};
};

// ---- Upload schedule (S2 "何时上传") ----------------------------------------
// Coalesces triggers: the first trigger after an upload starts a 30 s
// debounce (an immediate one right after connecting), a new generation is
// never sent within 10 s of the previous one, failures back off 5 s..5 min,
// a 429 waits Retry-After, and a heartbeat re-sends every 30 min.
class UploadSchedule {
public:
    static constexpr uint32_t kDebounceMs = 30000;
    static constexpr uint32_t kMinGenerationGapMs = 10000;
    static constexpr uint32_t kHeartbeatMs = 30 * 60 * 1000u;
    static constexpr uint32_t kFirstBackoffMs = 5000;
    static constexpr uint32_t kMaxBackoffMs = 5 * 60 * 1000u;

    void OnConnected(uint32_t now_ms);      // upload as soon as allowed
    void Trigger(uint32_t now_ms);          // ledger commit / receipts / awaiting changed
    // new_generation: the snapshot to send carries a generation the server
    // has not accepted yet (those are kept kMinGenerationGapMs apart).
    bool Due(uint32_t now_ms, bool new_generation) const;
    // Before power-off: upload as soon as the generation gap allows.
    void Flush(uint32_t now_ms) {
        pending_ = true;
        due_ms_ = now_ms;
    }
    // An upload ran: ok = 200 with a parsed reply. new_generation as for
    // Due. retry_after_s from a 429 (0 = none).
    void OnUploaded(uint32_t now_ms, bool ok, bool new_generation, uint32_t retry_after_s);
    // ms until Due would be true (0 = now); bounded by the heartbeat.
    uint32_t NextDueMs(uint32_t now_ms, bool new_generation) const;

private:
    bool pending_ = false;
    uint32_t due_ms_ = 0;
    uint32_t last_ok_ms_ = 0;
    uint32_t last_generation_ms_ = 0;
    bool sent_generation_ = false;
    bool sent_ok_ = false;
    uint32_t backoff_ms_ = 0;
};

}  // namespace kid_app
