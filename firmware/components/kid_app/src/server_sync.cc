#include "kid_app/server_sync.h"

#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace kid_app {
namespace {

bool UrlSafeChar(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-';
}

bool UrlSafe(const char* s, size_t min, size_t max) {
    if (!s) return false;
    size_t n = 0;
    for (; s[n]; ++n) {
        if (n >= max || !UrlSafeChar(s[n])) return false;
    }
    return n >= min;
}

bool FwChar(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '.' || c == '_' || c == '+' || c == '-';
}

// Bounded writer for the snapshot: every Put/Format fails closed.
struct Writer {
    char* out;
    size_t capacity;
    size_t used = 0;
    bool ok = true;

    void Put(const char* s) {
        const size_t n = std::strlen(s);
        if (!ok || used + n >= capacity) {
            ok = false;
            return;
        }
        std::memcpy(out + used, s, n);
        used += n;
        out[used] = '\0';
    }
    __attribute__((format(printf, 2, 3))) void Format(const char* format, ...) {
        if (!ok) return;
        va_list args;
        va_start(args, format);
        const int n = std::vsnprintf(out + used, capacity - used, format, args);
        va_end(args);
        if (n < 0 || used + static_cast<size_t>(n) >= capacity) {
            ok = false;
            return;
        }
        used += static_cast<size_t>(n);
    }
};

const char* ModeName(PetMode mode) {
    switch (mode) {
        case PetMode::Idle: return "IDLE";
        case PetMode::Interact: return "INTERACT";
        case PetMode::Story: return "STORY";
        case PetMode::Listening: return "LISTENING";
        case PetMode::Speaking: return "SPEAKING";
        case PetMode::Quiet: return "QUIET";
    }
    return nullptr;
}

const char* MoodName(PetMood mood) {
    switch (mood) {
        case PetMood::Neutral: return "NEUTRAL";
        case PetMood::Happy: return "HAPPY";
        case PetMood::Curious: return "CURIOUS";
        case PetMood::Thinking: return "THINKING";
        case PetMood::Sleepy: return "SLEEPY";
    }
    return nullptr;
}

bool ValidCommandId(uint64_t id) { return id >= 1 && id <= kMaxCommandId; }

bool ParseCommand(const JsonDoc& doc, int node, DeviceCommand* out) {
    if (!doc.Is(node, JsonDoc::Type::Object)) return false;
    DeviceCommand c{};
    if (!doc.Uint(doc.Get(node, "command_id"), &c.id) || !ValidCommandId(c.id)) return false;
    char kind[32]{};
    if (doc.String(doc.Get(node, "kind"), kind, sizeof(kind))) c.kind = CommandKindFromName(kind);
    const bool expiry = doc.Int(doc.Get(node, "expires_at"), &c.expires_at) && c.expires_at > 0;
    const int payload = doc.Get(node, "payload");
    bool payload_ok = false;
    if (doc.Is(payload, JsonDoc::Type::Object)) {
        switch (c.kind) {
            case CommandKind::MemoryClear: {
                uint64_t epoch = 0;
                payload_ok = doc.Uint(doc.Get(payload, "clear_epoch"), &epoch) && epoch <= 0xFFFFFFFFu;
                c.clear_epoch = static_cast<uint32_t>(epoch);
                break;
            }
            case CommandKind::SetTemperament: {
                const int s = doc.Get(payload, kCommandSeqKey);
                // Absent = 0; present must be an integer inside the exact range.
                const bool seq_ok = s < 0 || (doc.Uint(s, &c.seq) && c.seq <= kMaxCommandId);
                if (!seq_ok) break;
                const int t = doc.Get(payload, "temperament");
                for (uint8_t i = 0; i <= static_cast<uint8_t>(Temperament::Lively); ++i) {
                    const auto value = static_cast<Temperament>(i);
                    if (doc.StringEquals(t, TemperamentName(value))) {
                        c.temperament = value;
                        payload_ok = true;
                    }
                }
                break;
            }
            default:
                break;  // not carried out by this firmware
        }
    }
    c.payload_ok = expiry && payload_ok;
    *out = c;
    return true;
}

size_t ParseCommandList(const JsonDoc& doc, int list, DeviceCommand* out, size_t capacity) {
    size_t count = 0;
    for (int item = doc.First(list); item >= 0 && count < capacity; item = doc.Next(item)) {
        if (ParseCommand(doc, item, &out[count])) ++count;
    }
    return count;
}

int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

bool ReadDigits(const char* s, int count, int* out) {
    int value = 0;
    for (int i = 0; i < count; ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        value = value * 10 + (s[i] - '0');
    }
    *out = value;
    return true;
}

}  // namespace

size_t Base64Encode(const uint8_t* in, size_t size, char* out, size_t capacity) {
    static const char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const size_t need = (size + 2) / 3 * 4;
    if (!out || (!in && size) || need + 1 > capacity) return 0;
    size_t o = 0;
    for (size_t i = 0; i < size; i += 3) {
        const uint32_t b0 = in[i];
        const uint32_t b1 = i + 1 < size ? in[i + 1] : 0;
        const uint32_t b2 = i + 2 < size ? in[i + 2] : 0;
        const uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
        out[o++] = kAlphabet[(triple >> 18) & 0x3F];
        out[o++] = kAlphabet[(triple >> 12) & 0x3F];
        out[o++] = i + 1 < size ? kAlphabet[(triple >> 6) & 0x3F] : '=';
        out[o++] = i + 2 < size ? kAlphabet[triple & 0x3F] : '=';
    }
    out[o] = '\0';
    return o;
}

bool ValidDeviceId(const char* id) {
    if (!id || !id[0] || id[0] == '_' || id[0] == '-') return false;
    return UrlSafe(id, 3, kDeviceIdMax);
}

bool ValidDeviceToken(const char* token) { return UrlSafe(token, 32, kTokenMax); }

bool AllowedServerOrigin(const char* origin) {
    if (!origin) return false;
    const size_t total = std::strlen(origin);
    if (total > 155) return false;
    if (std::strncmp(origin, "https://", 8) != 0) return false;
    const char* host = origin + 8;
    // host[:port], nothing else (no path, credentials, query or spaces).
    const char* colon = std::strrchr(host, ':');
    const bool bracketed = host[0] == '[';
    if (bracketed) {
        const char* close = std::strchr(host, ']');
        if (!close) return false;
        colon = close[1] == ':' ? close + 1 : nullptr;
        if (close[1] && !colon) return false;
    }
    const size_t host_len = colon ? static_cast<size_t>(colon - host) : std::strlen(host);
    if (!host_len) return false;
    if (colon) {
        const char* port = colon + 1;
        const size_t digits = std::strlen(port);
        if (!digits || digits > 5) return false;
        long value = 0;
        for (size_t i = 0; i < digits; ++i) {
            if (port[i] < '0' || port[i] > '9') return false;
            value = value * 10 + (port[i] - '0');
        }
        if (value < 1 || value > 65535) return false;
    }
    if (!bracketed) {
        for (size_t i = 0; i < host_len; ++i) {
            const char c = host[i];
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '-'))
                return false;
        }
    } else {
        for (size_t i = 1; i + 1 < host_len; ++i) {
            const char c = host[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') ||
                  c == ':'))
                return false;
        }
    }
    return true;
}

bool OriginBound(const char* bound, const char* current) {
    return bound && current && bound[0] && std::strcmp(bound, current) == 0;
}

RotateVerdict ClassifyRotate(int http_status, bool parsed) {
    if (http_status == 200 && parsed) return RotateVerdict::Done;
    if (http_status == 409) return RotateVerdict::RepairSoon;
    if (http_status == 401) return RotateVerdict::AuthFailed;
    return RotateVerdict::Retry;
}

bool ConfirmRefusesToken(int http_status) { return http_status == 401; }

void SanitizeFwVersion(const char* in, char* out, size_t capacity) {
    if (!out || !capacity) return;
    size_t n = 0;
    const size_t limit = capacity - 1 < 40 ? capacity - 1 : 40;
    for (; in && in[n] && n < limit; ++n) out[n] = FwChar(in[n]) ? in[n] : '_';
    out[n] = '\0';
    if (!n && capacity > 7) std::memcpy(out, "unknown", 8);
}

bool ParseHttpDate(const char* text, int64_t* unix_seconds) {
    // "Sun, 06 Nov 1994 08:49:37 GMT" (29 characters).
    static const char kMonths[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    if (!text || !unix_seconds || std::strlen(text) != 29) return false;
    if (text[3] != ',' || text[4] != ' ' || text[7] != ' ' || text[11] != ' ' ||
        text[16] != ' ' || text[19] != ':' || text[22] != ':' || std::strcmp(text + 25, " GMT") != 0)
        return false;
    int day = 0, year = 0, hour = 0, minute = 0, second = 0;
    if (!ReadDigits(text + 5, 2, &day) || !ReadDigits(text + 12, 4, &year) ||
        !ReadDigits(text + 17, 2, &hour) || !ReadDigits(text + 20, 2, &minute) ||
        !ReadDigits(text + 23, 2, &second))
        return false;
    int month = 0;
    for (int m = 0; m < 12; ++m) {
        if (std::strncmp(text + 8, kMonths + m * 3, 3) == 0) month = m + 1;
    }
    if (!month || day < 1 || day > 31 || year < 1970 || hour > 23 || minute > 59 || second > 60)
        return false;
    const int64_t days = DaysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    *unix_seconds = days * 86400 + hour * 3600 + minute * 60 + second;
    return true;
}

const char* ReceiptName(ReceiptResult result) {
    switch (result) {
        case ReceiptResult::Applied: return "applied";
        case ReceiptResult::Rejected: return "rejected";
        case ReceiptResult::Expired: return "expired";
    }
    return "rejected";
}

size_t EncodeStateSnapshot(const StateSnapshotInput& in, char* out, size_t capacity) {
    if (!out || !capacity) return 0;
    out[0] = '\0';
    const char* mode = ModeName(in.pet.mode);
    const char* mood = MoodName(in.pet.mood);
    if (!in.generation || in.generation > static_cast<uint64_t>(INT64_MAX) || !mode || !mood ||
        !in.pet.active_page || in.battery_pct > 100 || in.awaiting_count > kMaxAwaitingConfirm ||
        in.receipt_count > kMaxReceipts || !in.fw_version || (in.stickers & ~kAllStickers))
        return 0;
    const size_t fw_len = std::strlen(in.fw_version);
    if (!fw_len || fw_len > 40) return 0;
    for (size_t i = 0; i < fw_len; ++i) {
        if (!FwChar(in.fw_version[i])) return 0;
    }
    uint8_t memory[MemoryStore::kBytes];
    uint8_t settings[ParentSettingsCodec::kBytes];
    if (!in.memory.Encode(memory, sizeof(memory)) ||
        !ParentSettingsCodec::Encode(in.parent, settings, sizeof(settings)))
        return 0;
    char memory_b64[(MemoryStore::kBytes + 2) / 3 * 4 + 1];
    char settings_b64[(ParentSettingsCodec::kBytes + 2) / 3 * 4 + 1];
    if (!Base64Encode(memory, sizeof(memory), memory_b64, sizeof(memory_b64)) ||
        !Base64Encode(settings, sizeof(settings), settings_b64, sizeof(settings_b64)))
        return 0;

    Writer w{out, capacity};
    w.Format("{\"schema_version\":\"2.0\",\"generation\":%" PRIu64
             ",\"ledger_schema\":4,\"fw_version\":\"%s\",\"rtc_day\":%" PRIu32,
             in.generation, in.fw_version, in.rtc_day);
    w.Format(",\"pet\":{\"mode\":\"%s\",\"mood\":\"%s\",\"interaction_count\":%" PRIu32
             ",\"active_page\":%u,\"quiet\":%s}",
             mode, mood, in.pet.interaction_count, static_cast<unsigned>(in.pet.active_page),
             in.pet.quiet ? "true" : "false");
    w.Format(",\"memory_b64\":\"%s\",\"parent_settings_b64\":\"%s\",\"parent_settings_rev\":%" PRIu32,
             memory_b64, settings_b64, in.parent_settings_rev);
    w.Format(",\"battery\":{\"pct\":%u,\"charging\":%s},\"inventory\":[]",
             static_cast<unsigned>(in.battery_pct), in.charging ? "true" : "false");
    w.Put(",\"awaiting_confirm\":[");
    for (uint8_t i = 0; i < in.awaiting_count; ++i) {
        if (!ValidCommandId(in.awaiting[i])) return 0;
        w.Format("%s%" PRIu64, i ? "," : "", in.awaiting[i]);
    }
    w.Put("],\"applied_commands\":[");
    for (uint8_t i = 0; i < in.receipt_count; ++i) {
        const CommandReceipt& r = in.receipts[i];
        if (!ValidCommandId(r.command_id)) return 0;
        w.Format("%s{\"command_id\":%" PRIu64 ",\"result\":\"%s\"}", i ? "," : "", r.command_id,
                 ReceiptName(r.result));
    }
    w.Put("],\"stickers\":[");
    bool first = true;
    for (uint8_t bit = 0; bit < kStickerCount; ++bit) {
        if (!(in.stickers & (1u << bit))) continue;
        w.Format("%s%u", first ? "" : ",", static_cast<unsigned>(bit));
        first = false;
    }
    w.Put("]}");
    if (!w.ok || w.used > kSnapshotMaxBytes) {
        out[0] = '\0';
        return 0;
    }
    return w.used;
}

CommandKind CommandKindFromName(const char* name) {
    if (!name) return CommandKind::Unknown;
    if (std::strcmp(name, "MEMORY_CLEAR") == 0) return CommandKind::MemoryClear;
    if (std::strcmp(name, "SET_TEMPERAMENT") == 0) return CommandKind::SetTemperament;
    if (std::strcmp(name, "SET_PARENT_SETTINGS") == 0) return CommandKind::SetParentSettings;
    if (std::strcmp(name, "SELECT_VOICE_PACK") == 0) return CommandKind::SelectVoicePack;
    if (std::strcmp(name, "SYNC_CONTENT") == 0) return CommandKind::SyncContent;
    return CommandKind::Unknown;
}

int OrderClass(CommandKind kind) { return kind == CommandKind::SetTemperament ? 0 : -1; }

uint64_t CommandTracker::AppliedSeq(CommandKind kind) const {
    const int c = OrderClass(kind);
    return c < 0 ? 0 : applied_seq_[c];
}

void CommandTracker::SetAppliedSeq(CommandKind kind, uint64_t seq) {
    const int c = OrderClass(kind);
    if (c >= 0) applied_seq_[c] = seq > kMaxCommandId ? kMaxCommandId : seq;
}

bool CommandTracker::NoteApplied(CommandKind kind, uint64_t seq) {
    const int c = OrderClass(kind);
    if (c < 0 || seq <= applied_seq_[c]) return false;
    applied_seq_[c] = seq > kMaxCommandId ? kMaxCommandId : seq;
    return true;
}

bool CommandTracker::Stale(const DeviceCommand& command) const {
    return OrderClass(command.kind) >= 0 && command.seq < AppliedSeq(command.kind);
}

bool NeedsParentConfirm(CommandKind kind) {
    return kind == CommandKind::MemoryClear || kind == CommandKind::SetTemperament;
}

bool SupportedCommand(CommandKind kind) {
    // SET_PARENT_SETTINGS needs ParentSettingsCodec v2 (rev), SELECT_VOICE_PACK
    // the multi voice pack and SYNC_CONTENT the TF downloader (§11.2).
    return kind == CommandKind::MemoryClear || kind == CommandKind::SetTemperament;
}

bool ParseStateReply(const char* body, size_t size, JsonDoc* doc, StateReply* out) {
    if (!doc || !out || !doc->Parse(body, size)) return false;
    const int root = doc->Root();
    if (!doc->Is(root, JsonDoc::Type::Object)) return false;
    StateReply r{};
    const int result = doc->Get(root, "result");
    if (doc->StringEquals(result, "accepted")) r.result = StateResult::Accepted;
    else if (doc->StringEquals(result, "duplicate")) r.result = StateResult::Duplicate;
    else if (doc->StringEquals(result, "stale")) r.result = StateResult::Stale;
    else if (doc->StringEquals(result, "generation_conflict")) r.result = StateResult::GenerationConflict;
    else return false;
    if (!doc->Uint(doc->Get(root, "generation"), &r.generation)) return false;
    const int acked = doc->Get(root, "acked_commands");
    if (acked >= 0 && !doc->Is(acked, JsonDoc::Type::Array)) return false;
    for (int item = doc->First(acked); item >= 0 && r.acked_count < kMaxReceipts;
         item = doc->Next(item)) {
        uint64_t id = 0;
        if (doc->Uint(item, &id) && ValidCommandId(id)) r.acked[r.acked_count++] = id;
    }
    const int commands = doc->Get(root, "commands");
    if (commands >= 0 && !doc->Is(commands, JsonDoc::Type::Array)) return false;
    r.command_count = static_cast<uint8_t>(
        ParseCommandList(*doc, commands, r.commands, kMaxCommandsPerReply));
    *out = r;
    return true;
}

bool ParseCommandsReply(const char* body, size_t size, JsonDoc* doc,
                        DeviceCommand* commands, uint8_t capacity, uint8_t* count) {
    if (!doc || !commands || !count || !doc->Parse(body, size)) return false;
    const int root = doc->Root();
    const int list = doc->Get(root, "commands");
    if (!doc->Is(list, JsonDoc::Type::Array)) return false;
    *count = static_cast<uint8_t>(ParseCommandList(*doc, list, commands, capacity));
    return true;
}

bool ParsePairingStart(const char* body, size_t size, JsonDoc* doc, PairingStartReply* out) {
    if (!doc || !out || !doc->Parse(body, size)) return false;
    const int root = doc->Root();
    PairingStartReply r{};
    uint64_t expires = 0;
    if (!doc->String(doc->Get(root, "code"), r.code, sizeof(r.code)) || std::strlen(r.code) != 6 ||
        !doc->String(doc->Get(root, "claim_secret"), r.claim_secret, sizeof(r.claim_secret)) ||
        !UrlSafe(r.claim_secret, 16, 64) || !doc->Uint(doc->Get(root, "expires_in"), &expires) ||
        expires < 1 || expires > 3600)
        return false;
    for (int i = 0; i < 6; ++i) {
        if (r.code[i] < '0' || r.code[i] > '9') return false;
    }
    r.expires_in = static_cast<uint32_t>(expires);
    *out = r;
    return true;
}

bool ParsePairingClaim(const char* body, size_t size, JsonDoc* doc,
                       const char* expected_device_id, ClaimStatus* status,
                       char* token, size_t token_capacity) {
    if (!doc || !status || !doc->Parse(body, size)) return false;
    const int root = doc->Root();
    const int state = doc->Get(root, "status");
    if (doc->StringEquals(state, "pending")) {
        *status = ClaimStatus::Pending;
        return true;
    }
    if (!doc->StringEquals(state, "claimed") || !expected_device_id || !token ||
        !doc->StringEquals(doc->Get(root, "device_id"), expected_device_id))
        return false;
    char value[kTokenMax + 1]{};
    if (!doc->String(doc->Get(root, "token"), value, sizeof(value)) || !ValidDeviceToken(value) ||
        std::strlen(value) + 1 > token_capacity)
        return false;
    std::memcpy(token, value, std::strlen(value) + 1);
    std::memset(value, 0, sizeof(value));
    *status = ClaimStatus::Claimed;
    return true;
}

bool ParseTokenRotate(const char* body, size_t size, JsonDoc* doc,
                      char* token, size_t token_capacity) {
    if (!doc || !token || !doc->Parse(body, size)) return false;
    char value[kTokenMax + 1]{};
    if (!doc->String(doc->Get(doc->Root(), "token"), value, sizeof(value)) ||
        !ValidDeviceToken(value) || std::strlen(value) + 1 > token_capacity)
        return false;
    std::memcpy(token, value, std::strlen(value) + 1);
    std::memset(value, 0, sizeof(value));
    return true;
}

// ---- CommandTracker ---------------------------------------------------------

bool CommandTracker::AddReceipt(uint64_t id, ReceiptResult result) {
    if (!ValidCommandId(id)) return false;
    if (HasReceipt(id)) return true;
    if (receipt_count_ >= kMaxReceipts) {
        // Full: an "applied" answer must never be lost (the server would ask
        // the parent again). Make room by dropping the oldest refusal or
        // expiry: those are re-derived when the command is delivered again
        // (refusals are persisted, expiry follows from the clock).
        uint8_t victim = receipt_count_;
        for (uint8_t i = 0; i < receipt_count_ && victim == receipt_count_; ++i) {
            if (receipts_[i].result != ReceiptResult::Applied) victim = i;
        }
        if (victim == receipt_count_) return false;
        for (uint8_t j = victim; j + 1 < receipt_count_; ++j) receipts_[j] = receipts_[j + 1];
        receipts_[--receipt_count_] = CommandReceipt{};
    }
    receipts_[receipt_count_++] = CommandReceipt{id, result};
    ++revision_;
    return true;
}

bool CommandTracker::HasReceipt(uint64_t id) const {
    for (uint8_t i = 0; i < receipt_count_; ++i) {
        if (receipts_[i].command_id == id) return true;
    }
    return false;
}

void CommandTracker::OnAcked(const uint64_t* ids, size_t count) {
    if (!ids) return;
    uint8_t kept = 0;
    for (uint8_t i = 0; i < receipt_count_; ++i) {
        bool acked = false;
        for (size_t j = 0; j < count; ++j) acked = acked || ids[j] == receipts_[i].command_id;
        if (!acked) receipts_[kept++] = receipts_[i];
    }
    if (kept != receipt_count_) ++revision_;
    for (uint8_t i = kept; i < receipt_count_; ++i) receipts_[i] = CommandReceipt{};
    receipt_count_ = kept;
}

size_t CommandTracker::Receipts(CommandReceipt* out, size_t capacity) const {
    size_t n = 0;
    for (; out && n < receipt_count_ && n < capacity; ++n) out[n] = receipts_[n];
    return n;
}

bool CommandTracker::MarkInFlight(uint64_t id) {
    if (!ValidCommandId(id)) return false;
    if (InFlight(id)) return true;
    if (in_flight_count_ >= kInFlightCapacity) return false;
    in_flight_[in_flight_count_++] = id;
    return true;
}

bool CommandTracker::InFlight(uint64_t id) const {
    for (uint8_t i = 0; i < in_flight_count_; ++i) {
        if (in_flight_[i] == id) return true;
    }
    return false;
}

void CommandTracker::ClearInFlight(uint64_t id) {
    for (uint8_t i = 0; i < in_flight_count_; ++i) {
        if (in_flight_[i] != id) continue;
        for (uint8_t j = i; j + 1 < in_flight_count_; ++j) in_flight_[j] = in_flight_[j + 1];
        in_flight_[--in_flight_count_] = 0;
        return;
    }
}

void CommandTracker::EncodeApplied(uint8_t out[kAppliedBlobBytes]) const {
    std::memset(out, 0, kAppliedBlobBytes);
    out[0] = 'A';
    out[1] = 'R';
    out[2] = 1;
    uint8_t n = 0;
    for (uint8_t i = 0; i < receipt_count_; ++i) {
        if (receipts_[i].result != ReceiptResult::Applied) continue;
        for (int b = 0; b < 8; ++b)
            out[4 + n * 8 + b] = static_cast<uint8_t>(receipts_[i].command_id >> (8 * b));
        ++n;
    }
    out[3] = n;
}

bool CommandTracker::DecodeApplied(const uint8_t* in, size_t size) {
    if (!in || size != kAppliedBlobBytes || in[0] != 'A' || in[1] != 'R' || in[2] != 1 ||
        in[3] > kMaxReceipts)
        return false;
    uint64_t ids[kMaxReceipts]{};
    for (size_t i = 0; i < kMaxReceipts; ++i) {
        uint64_t id = 0;
        for (int b = 0; b < 8; ++b) id |= static_cast<uint64_t>(in[4 + i * 8 + b]) << (8 * b);
        if (i < in[3] ? !ValidCommandId(id) : id != 0) return false;
        ids[i] = id;
    }
    for (size_t i = 0; i < in[3]; ++i) AddReceipt(ids[i], ReceiptResult::Applied);
    return true;
}

void CommandTracker::ClearAllInFlight() {
    for (auto& id : in_flight_) id = 0;
    in_flight_count_ = 0;
}

void CommandTracker::AddRejected(uint64_t id) {
    if (!ValidCommandId(id) || Rejected(id)) return;
    if (rejected_count_ >= kRejectedCapacity) {
        for (size_t i = 0; i + 1 < kRejectedCapacity; ++i) rejected_[i] = rejected_[i + 1];
        --rejected_count_;
    }
    rejected_[rejected_count_++] = id;
}

bool CommandTracker::Rejected(uint64_t id) const {
    for (uint8_t i = 0; i < rejected_count_; ++i) {
        if (rejected_[i] == id) return true;
    }
    return false;
}

void CommandTracker::EncodeRejected(uint8_t out[kRejectedBlobBytes]) const {
    std::memset(out, 0, kRejectedBlobBytes);
    out[0] = 'R';
    out[1] = 'J';
    out[2] = 1;
    out[3] = rejected_count_;
    for (uint8_t i = 0; i < rejected_count_; ++i) {
        for (int b = 0; b < 8; ++b)
            out[4 + i * 8 + b] = static_cast<uint8_t>(rejected_[i] >> (8 * b));
    }
}

bool CommandTracker::DecodeRejected(const uint8_t* in, size_t size) {
    if (!in || size != kRejectedBlobBytes || in[0] != 'R' || in[1] != 'J' || in[2] != 1 ||
        in[3] > kRejectedCapacity)
        return false;
    uint64_t ids[kRejectedCapacity]{};
    for (size_t i = 0; i < kRejectedCapacity; ++i) {
        uint64_t id = 0;
        for (int b = 0; b < 8; ++b) id |= static_cast<uint64_t>(in[4 + i * 8 + b]) << (8 * b);
        if (i < in[3] ? !ValidCommandId(id) : id != 0) return false;
        ids[i] = id;
    }
    std::memcpy(rejected_, ids, sizeof(ids));
    rejected_count_ = in[3];
    return true;
}

bool CommandExpired(const DeviceCommand& command, bool clock_known, int64_t now_utc) {
    return clock_known && now_utc >= command.expires_at;
}

CommandDecision RouteCommand(const DeviceCommand& command, const CommandTracker& tracker,
                             bool awaiting_parent, bool clock_known, int64_t now_utc) {
    CommandDecision d{};
    if (!ValidCommandId(command.id) || tracker.HasReceipt(command.id) ||
        tracker.InFlight(command.id) || awaiting_parent)
        return d;  // Skip
    d.route = CommandRoute::Answer;
    d.receipt = ReceiptResult::Rejected;
    if (tracker.Rejected(command.id) || !SupportedCommand(command.kind) || !command.payload_ok ||
        tracker.Stale(command))
        return d;  // a stale command (older than one already applied) is refused
    if (CommandExpired(command, clock_known, now_utc)) {
        d.receipt = ReceiptResult::Expired;
        return d;
    }
    d.route = CommandRoute::ToPet;
    return d;
}

// ---- PairingFlow ---------------------------------------------------

void PairingFlow::Begin(uint32_t now_ms) {
    phase_ = Phase::Start;
    view_ = LinkView{LinkState::Connecting, 0};
    begun_ms_ = now_ms;
    deadline_ms_ = now_ms;
    next_ms_ = now_ms;
    failures_ = 0;
    waiting_ = false;
    discard_stored_ = false;
    std::memset(claim_secret_, 0, sizeof(claim_secret_));
}

void PairingFlow::Fail(LinkState state) {
    discard_stored_ = false;
    phase_ = Phase::Idle;
    waiting_ = false;
    view_ = LinkView{state, 0};
    std::memset(claim_secret_, 0, sizeof(claim_secret_));
}

void PairingFlow::Retry(uint32_t now_ms, uint32_t wait_ms) {
    if (++failures_ >= kMaxNetworkFailures) {
        Fail(LinkState::Failed);
        return;
    }
    next_ms_ = now_ms + wait_ms;
}

void PairingFlow::Tick(uint32_t now_ms) {
    if (phase_ == Phase::Idle) return;
    const bool pre_save = phase_ == Phase::Start || phase_ == Phase::ShowCode;
    if (pre_save && now_ms - begun_ms_ >= kMaxDurationMs) {
        Fail(LinkState::Failed);
    } else if (phase_ == Phase::Start && now_ms - begun_ms_ >= kStartTimeoutMs) {
        Fail(LinkState::Failed);  // never reached the server
    } else if (phase_ == Phase::ShowCode && static_cast<int32_t>(now_ms - deadline_ms_) >= 0) {
        Fail(LinkState::Failed);  // the code expired
    }
}

PairingFlow::Action PairingFlow::Step(uint32_t now_ms) {
    if (phase_ == Phase::Idle) return Action::None;
    Tick(now_ms);
    if (phase_ == Phase::Idle || waiting_) return Action::None;
    if (static_cast<int32_t>(now_ms - next_ms_) < 0) return Action::None;
    waiting_ = true;
    switch (phase_) {
        case Phase::Start: return Action::Start;
        case Phase::ShowCode: return Action::Claim;
        case Phase::Save: return Action::Save;
        case Phase::Confirm: return Action::Confirm;
        case Phase::Idle: break;
    }
    waiting_ = false;
    return Action::None;
}

void PairingFlow::OnStart(int http_status, const PairingStartReply* reply, uint32_t now_ms) {
    if (phase_ != Phase::Start) return;
    waiting_ = false;
    if (http_status == 200 && reply) {
        std::memcpy(claim_secret_, reply->claim_secret, sizeof(claim_secret_));
        claim_secret_[sizeof(claim_secret_) - 1] = '\0';
        const uint32_t seconds = reply->expires_in > 600 ? 600 : reply->expires_in;
        phase_ = Phase::ShowCode;
        view_ = LinkView{LinkState::ShowCode,
                         static_cast<uint32_t>(std::strtoul(reply->code, nullptr, 10))};
        deadline_ms_ = now_ms + seconds * 1000u;
        next_ms_ = now_ms + kClaimIntervalMs;
        failures_ = 0;
        return;
    }
    // 404: no pairing opened on the parent panel (or this id is taken).
    if (http_status == 404) Fail(LinkState::NoServer);
    else if (http_status == 0 || http_status >= 500) Retry(now_ms, kClaimIntervalMs);
    else Fail(LinkState::Failed);
}

void PairingFlow::OnClaim(int http_status, bool parsed, ClaimStatus status, uint32_t now_ms) {
    if (phase_ != Phase::ShowCode) return;
    waiting_ = false;
    if (http_status == 200 && parsed) {
        failures_ = 0;
        if (status == ClaimStatus::Claimed) {
            phase_ = Phase::Save;
            view_ = LinkView{LinkState::Saving, 0};
            next_ms_ = now_ms;
            std::memset(claim_secret_, 0, sizeof(claim_secret_));
        } else {
            next_ms_ = now_ms + kClaimIntervalMs;
        }
        return;
    }
    if (http_status == 429) next_ms_ = now_ms + kThrottledMs;
    else if (http_status == 0 || http_status >= 500 || http_status == 200) Retry(now_ms, kClaimIntervalMs);
    else Fail(LinkState::Failed);  // 401: the pairing was voided
}

void PairingFlow::OnSaved(bool ok, uint32_t now_ms) {
    if (phase_ != Phase::Save) return;
    waiting_ = false;
    if (!ok) {
        Fail(LinkState::Failed);  // never confirm a token that is not stored
        return;
    }
    phase_ = Phase::Confirm;
    next_ms_ = now_ms;
    failures_ = 0;
}

void PairingFlow::OnConfirm(int http_status, uint32_t now_ms) {
    if (phase_ != Phase::Confirm) return;
    waiting_ = false;
    if (http_status == 200) {
        phase_ = Phase::Idle;
        view_ = LinkView{LinkState::Paired, 0};
        return;
    }
    if (ConfirmRefusesToken(http_status)) {
        Fail(LinkState::Failed);  // the server does not know this token
        discard_stored_ = true;
        return;
    }
    // Anything else (network, 5xx, 429, other 4xx) proves nothing about the
    // token: keep it and try again; when the tries run out the owner keeps
    // the pending pairing and confirms it later (boot or retry).
    Retry(now_ms, kClaimIntervalMs);
}

// ---- UploadSchedule -------------------------------------------------------------

void UploadSchedule::OnConnected(uint32_t now_ms) {
    if (pending_ && static_cast<int32_t>(due_ms_ - now_ms) <= 0) return;
    pending_ = true;
    due_ms_ = now_ms;
    backoff_ms_ = 0;
}

void UploadSchedule::Trigger(uint32_t now_ms) {
    if (pending_) return;  // coalesce: the earlier due time stands
    pending_ = true;
    due_ms_ = now_ms + kDebounceMs;
}

uint32_t UploadSchedule::NextDueMs(uint32_t now_ms, bool new_generation) const {
    uint32_t wait = kHeartbeatMs;
    if (pending_) {
        const int32_t left = static_cast<int32_t>(due_ms_ - now_ms);
        wait = left > 0 ? static_cast<uint32_t>(left) : 0;
    } else if (sent_ok_) {
        const uint32_t since = now_ms - last_ok_ms_;
        wait = since >= kHeartbeatMs ? 0 : kHeartbeatMs - since;
    }
    if (new_generation && sent_generation_) {
        const uint32_t since = now_ms - last_generation_ms_;
        if (since < kMinGenerationGapMs && wait < kMinGenerationGapMs - since)
            wait = kMinGenerationGapMs - since;
    }
    return wait;
}

bool UploadSchedule::Due(uint32_t now_ms, bool new_generation) const {
    if (!pending_ && !sent_ok_) return false;
    return NextDueMs(now_ms, new_generation) == 0;
}

void UploadSchedule::OnUploaded(uint32_t now_ms, bool ok, bool new_generation,
                                uint32_t retry_after_s) {
    if (ok) {
        pending_ = false;
        sent_ok_ = true;
        last_ok_ms_ = now_ms;
        backoff_ms_ = 0;
        if (new_generation) {
            sent_generation_ = true;
            last_generation_ms_ = now_ms;
        }
        return;
    }
    backoff_ms_ = !backoff_ms_ ? kFirstBackoffMs
                : backoff_ms_ >= kMaxBackoffMs / 2 ? kMaxBackoffMs
                                                   : backoff_ms_ * 2;
    uint32_t wait = backoff_ms_;
    if (retry_after_s) {
        const uint32_t capped = retry_after_s > 3600 ? 3600 : retry_after_s;
        if (capped * 1000u > wait) wait = capped * 1000u;
    }
    pending_ = true;
    due_ms_ = now_ms + wait;
}

}  // namespace kid_app
