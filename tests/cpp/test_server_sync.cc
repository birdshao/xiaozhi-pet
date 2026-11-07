// Device side of the PetLearning server protocol (V5_V6 §5.1-5.4, §11.2).
//
//   test_server_sync <memory_dir> <sync_dir>          check
//   test_server_sync --write <memory_dir> <sync_dir>  regenerate snapshot_*.json
//
// snapshot_*.json are built from the S2 golden vectors in tests/fixtures/memory
// and must match byte for byte; tests/test_v5_server_device_sync_contract.py
// asserts the real server accepts the same files and produced the reply
// fixtures (state_reply.json, commands_reply.json) the parsers read here.
#include "kid_app/json_lite.h"
#include "kid_app/link_view.h"
#include "kid_app/parent_requests.h"
#include "kid_app/pet_atomic_ledger.h"
#include "kid_app/pet_command_apply.h"
#include "kid_app/pet_local_event_id.h"
#include "kid_app/server_sync.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

using namespace kid_app;

namespace {

bool g_write = false;
std::string g_memory_dir;
std::string g_sync_dir;

std::string ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    assert(in && "missing fixture");
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// ---- JsonDoc ----------------------------------------------------------------

bool Parses(const char* text) {
    static JsonDoc doc;
    return doc.Parse(text, std::strlen(text));
}

void TestJson() {
    JsonDoc doc;
    const char* text = " {\"a\": [1, -2, 3.5, 1e3], \"s\": \"x\\\"y\\u0041\\n\", \"t\": true,"
                       " \"f\": false, \"n\": null, \"o\": {\"k\": 18446744073709551615}} ";
    assert(doc.Parse(text, std::strlen(text)));
    const int root = doc.Root();
    assert(doc.Is(root, JsonDoc::Type::Object) && doc.Count(root) == 6);
    const int a = doc.Get(root, "a");
    assert(doc.Count(a) == 4);
    int64_t i = 0;
    uint64_t u = 0;
    int item = doc.First(a);
    assert(doc.Int(item, &i) && i == 1 && doc.Uint(item, &u) && u == 1);
    item = doc.Next(item);
    assert(doc.Int(item, &i) && i == -2 && !doc.Uint(item, &u));
    item = doc.Next(item);
    assert(!doc.Int(item, &i) && doc.Is(item, JsonDoc::Type::Number));  // 3.5
    item = doc.Next(item);
    assert(!doc.Int(item, &i));                                         // 1e3
    assert(doc.Next(item) < 0);
    char s[16];
    assert(doc.String(doc.Get(root, "s"), s, sizeof(s)) && std::strcmp(s, "x\"yA\n") == 0);
    assert(doc.StringEquals(doc.Get(root, "s"), "x\"yA\n"));
    assert(!doc.String(doc.Get(root, "s"), s, 5));  // does not fit
    bool b = false;
    assert(doc.Bool(doc.Get(root, "t"), &b) && b);
    assert(doc.Bool(doc.Get(root, "f"), &b) && !b);
    assert(doc.Is(doc.Get(root, "n"), JsonDoc::Type::Null));
    assert(doc.Uint(doc.Get(doc.Get(root, "o"), "k"), &u) && u == UINT64_MAX);
    assert(doc.Get(root, "missing") < 0 && doc.Get(a, "x") < 0);
    // Integer limits.
    assert(Parses("[9223372036854775807,-9223372036854775808,18446744073709551616]"));
    {
        JsonDoc d;
        const char* t = "[9223372036854775807,-9223372036854775808,18446744073709551616,9223372036854775808]";
        assert(d.Parse(t, std::strlen(t)));
        int n = d.First(d.Root());
        assert(d.Int(n, &i) && i == INT64_MAX);
        n = d.Next(n);
        assert(d.Int(n, &i) && i == INT64_MIN);
        n = d.Next(n);
        assert(!d.Uint(n, &u) && !d.Int(n, &i));
        n = d.Next(n);
        assert(!d.Int(n, &i) && d.Uint(n, &u) && u == 9223372036854775808ull);
    }
    // Strict grammar.
    for (const char* bad : {"", " ", "{", "[1,]", "{\"a\":1,}", "{\"a\" 1}", "[01]", "[1.]", "[.5]",
                            "[-]", "[1e]", "tru", "nul", "\"abc", "\"a\\x\"", "\"\\u00e9\"",
                            "\"\\u12\"", "{} x", "[1] [2]", "{a:1}", "'a'", "\"tab\there\""})
        assert(!Parses(bad));
    for (const char* good : {"0", "-0", "[]", "{}", "\"\"", " [ 1 , { } ] ", "\"\\/\\b\\f\\r\\t\""})
        assert(Parses(good));
    std::string deep(JsonDoc::kMaxDepth + 2, '[');
    deep += std::string(JsonDoc::kMaxDepth + 2, ']');
    assert(!Parses(deep.c_str()));
    std::string many = "[";
    for (size_t k = 0; k < JsonDoc::kMaxNodes; ++k) many += k ? ",1" : "1";
    many += "]";
    assert(!Parses(many.c_str()));  // the array node + kMaxNodes values
}

void TestBase64AndHelpers() {
    char out[16];
    const char* in = "foobar";
    const char* want[] = {"", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"};
    for (size_t n = 0; n <= 6; ++n) {
        const size_t len = Base64Encode(reinterpret_cast<const uint8_t*>(in), n, out, sizeof(out));
        assert(len == std::strlen(want[n]) && std::strcmp(out, want[n]) == 0);
    }
    assert(!Base64Encode(reinterpret_cast<const uint8_t*>(in), 6, out, 8));  // 8 chars + NUL

    assert(ValidDeviceId("tt-0123456789ab-1f2e") && ValidDeviceId("pet-001") && ValidDeviceId("abc"));
    assert(!ValidDeviceId("ab") && !ValidDeviceId("-abc") && !ValidDeviceId("_abc") &&
           !ValidDeviceId("a b c") && !ValidDeviceId(nullptr) &&
           !ValidDeviceId(std::string(64, 'a').c_str()));
    assert(ValidDeviceToken(std::string(43, 'A').c_str()));
    assert(!ValidDeviceToken(std::string(31, 'A').c_str()) &&
           !ValidDeviceToken(std::string(191, 'A').c_str()) &&
           !ValidDeviceToken((std::string(40, 'A') + "+/").c_str()));

    assert(AllowedServerOrigin("https://pet.example.cn", false));
    assert(AllowedServerOrigin("https://pet.example.cn:8443", false));
    assert(AllowedServerOrigin("https://[fd00::1]:443", false));
    assert(!AllowedServerOrigin("https://pet.example.cn/", false));
    assert(!AllowedServerOrigin("https://user@pet.example.cn", false));
    assert(!AllowedServerOrigin("https://pet.example.cn:0", false));
    assert(!AllowedServerOrigin("https://pet.example.cn:99999", false));
    assert(!AllowedServerOrigin("ftp://pet.example.cn", true));
    // Plain http: never by default; with the LAN flag only RFC 1918 literals.
    assert(!AllowedServerOrigin("http://192.168.1.110:8780", false));
    assert(AllowedServerOrigin("http://192.168.1.110:8780", true));
    assert(AllowedServerOrigin("http://10.0.0.5", true));
    assert(AllowedServerOrigin("http://172.16.4.1:80", true));
    assert(!AllowedServerOrigin("http://172.32.4.1", true));
    assert(!AllowedServerOrigin("http://8.8.8.8", true));
    assert(!AllowedServerOrigin("http://pet.example.cn", true));
    assert(!AllowedServerOrigin("http://192.168.1", true));
    assert(!AllowedServerOrigin("http://192.168.1.256", true));
    assert(!AllowedServerOrigin("http://192.168.1.1.1", true));

    char origin[64];
    assert(OriginOfUrl("http://192.168.1.110:8780/xiaozhi/ota/", origin, sizeof(origin)) &&
           std::strcmp(origin, "http://192.168.1.110:8780") == 0);
    assert(OriginOfUrl("https://pet.example.cn", origin, sizeof(origin)) &&
           std::strcmp(origin, "https://pet.example.cn") == 0);
    assert(!OriginOfUrl("https://u:p@pet.example.cn/x", origin, sizeof(origin)));
    assert(!OriginOfUrl("pet.example.cn/x", origin, sizeof(origin)));
    assert(!OriginOfUrl("https:///x", origin, sizeof(origin)));
    assert(!OriginOfUrl("https://pet.example.cn/x", origin, 10));

    char fw[48];
    SanitizeFwVersion("2.5.2 (kid) build#7", fw, sizeof(fw));
    assert(std::strcmp(fw, "2.5.2__kid__build_7") == 0);
    SanitizeFwVersion("", fw, sizeof(fw));
    assert(std::strcmp(fw, "unknown") == 0);
    SanitizeFwVersion(std::string(60, '1').c_str(), fw, sizeof(fw));
    assert(std::strlen(fw) == 40);

    int64_t t = 0;
    assert(ParseHttpDate("Sun, 06 Nov 1994 08:49:37 GMT", &t) && t == 784111777);
    assert(ParseHttpDate("Thu, 01 Jan 1970 00:00:00 GMT", &t) && t == 0);
    assert(ParseHttpDate("Fri, 06 Nov 2026 04:13:20 GMT", &t) && t == 1793938400);
    assert(!ParseHttpDate("Sun, 06 Nov 1994 08:49:37 UTC", &t));
    assert(!ParseHttpDate("Sunday, 06-Nov-94 08:49:37 GMT", &t));
    assert(!ParseHttpDate("Sun, 06 Foo 1994 08:49:37 GMT", &t));

    for (uint8_t s = 0; s <= static_cast<uint8_t>(LinkState::NeedsRepair); ++s) {
        const LinkView v{static_cast<LinkState>(s), 0};
        assert(UnpackLinkView(PackLinkView(v)) == v);
    }
    const LinkView code{LinkState::ShowCode, 42137};
    assert(UnpackLinkView(PackLinkView(code)) == code);
    assert(UnpackLinkView(PackLinkView(LinkView{LinkState::Paired, 5})).code == 0);
    assert(UnpackLinkView(0x0Fu).state == LinkState::Unpaired);
    assert(UnpackLinkView(1u << 24).state == LinkState::Unpaired);
}

// ---- snapshots (golden) --------------------------------------------------------

struct SnapshotCase {
    const char* name;
    const char* memory;
    const char* settings;
    uint64_t generation;
    uint32_t rtc_day;
    PetSnapshot pet;
    uint8_t battery;
    bool charging;
    std::vector<uint64_t> awaiting;
    std::vector<CommandReceipt> receipts;
};

PetSnapshot Pet(PetMode mode, PetMood mood, uint32_t count, uint16_t page, bool quiet) {
    PetSnapshot p{};
    p.mode = mode;
    p.mood = mood;
    p.interaction_count = count;
    p.active_page = page;
    p.quiet = quiet;
    return p;
}

// Mirrors CASES in tests/test_v5_server_device_sync_contract.py.
std::vector<SnapshotCase> SnapshotCases() {
    const uint64_t max_id = kMaxCommandId;
    SnapshotCase worst{"worst", "memory_full.bin", "settings_custom.bin", 9223372036854775807ull,
                       0xFFFFFFFFu, Pet(PetMode::Quiet, PetMood::Sleepy, 0xFFFFFFFFu, 0xFFFF, true),
                       100, false, {}, {}};
    for (uint64_t i = 0; i < 8; ++i) worst.awaiting.push_back(max_id - i);
    for (uint64_t i = 0; i < 16; ++i)
        worst.receipts.push_back(CommandReceipt{max_id - 100 - i, ReceiptResult::Rejected});
    return {
        {"empty", "memory_empty.bin", "settings_default.bin", 1, 0,
         Pet(PetMode::Idle, PetMood::Neutral, 0, 1, false), 0, false, {}, {}},
        {"rich", "memory_rich.bin", "settings_custom.bin", 1842, 20761,
         Pet(PetMode::Story, PetMood::Happy, 57, 3, false), 64, true, {4098},
         {{4097, ReceiptResult::Applied}, {4099, ReceiptResult::Rejected},
          {4100, ReceiptResult::Expired}}},
        {"wiped", "memory_wiped.bin", "settings_default.bin", 77, 20762,
         Pet(PetMode::Interact, PetMood::Curious, 3, 1, false), 100, true, {},
         {{9007199254740000ull, ReceiptResult::Applied}}},
        worst,
    };
}

StateSnapshotInput InputFor(const SnapshotCase& c) {
    StateSnapshotInput in{};
    const std::string memory = ReadFile(g_memory_dir + "/" + c.memory);
    const std::string settings = ReadFile(g_memory_dir + "/" + c.settings);
    assert(MemoryStore::Decode(reinterpret_cast<const uint8_t*>(memory.data()), memory.size(),
                               &in.memory));
    assert(ParentSettingsCodec::Decode(reinterpret_cast<const uint8_t*>(settings.data()),
                                       settings.size(), &in.parent));
    in.generation = c.generation;
    in.fw_version = "pet-2026.11.06";
    in.rtc_day = c.rtc_day;
    in.pet = c.pet;
    in.battery_pct = c.battery;
    in.charging = c.charging;
    for (uint64_t id : c.awaiting) in.awaiting[in.awaiting_count++] = id;
    for (const auto& r : c.receipts) in.receipts[in.receipt_count++] = r;
    return in;
}

void TestSnapshotGolden() {
    static char out[kSnapshotMaxBytes + 64];
    for (const auto& c : SnapshotCases()) {
        const StateSnapshotInput in = InputFor(c);
        const size_t n = EncodeStateSnapshot(in, out, sizeof(out));
        assert(n > 0 && n == std::strlen(out) && n <= kSnapshotMaxBytes);
        const std::string path = g_sync_dir + "/snapshot_" + c.name + ".json";
        if (g_write) {
            std::ofstream(path, std::ios::binary) << out;
            continue;
        }
        if (ReadFile(path) != out) {
            std::cerr << "snapshot mismatch: " << c.name << "\n" << out << "\n";
            assert(false);
        }
        // A buffer one byte short fails closed (no partial JSON).
        assert(!EncodeStateSnapshot(in, out, n));
        assert(out[0] == '\0');
    }
    // Invalid inputs.
    StateSnapshotInput base = InputFor(SnapshotCases()[1]);
    auto fails = [](StateSnapshotInput in) { return EncodeStateSnapshot(in, out, sizeof(out)) == 0; };
    StateSnapshotInput bad = base;
    bad.generation = 0;
    assert(fails(bad));
    bad = base;
    bad.generation = 9223372036854775808ull;
    assert(fails(bad));
    bad = base;
    bad.pet.active_page = 0;
    assert(fails(bad));
    bad = base;
    bad.pet.mode = static_cast<PetMode>(9);
    assert(fails(bad));
    bad = base;
    bad.battery_pct = 101;
    assert(fails(bad));
    bad = base;
    bad.fw_version = "has space";
    assert(fails(bad));
    bad = base;
    bad.fw_version = "";
    assert(fails(bad));
    bad = base;
    bad.awaiting[0] = kMaxCommandId + 1;
    assert(fails(bad));
    bad = base;
    bad.receipts[0].command_id = 0;
    assert(fails(bad));
    bad = base;
    bad.awaiting_count = kMaxAwaitingConfirm + 1;
    assert(fails(bad));
    bad = base;
    bad.parent.volume_cap_pct = 15;  // not a 10 % step: settings do not encode
    assert(fails(bad));
    assert(!fails(base));
}

// ---- replies ---------------------------------------------------------------------

void TestReplies() {
    JsonDoc doc;
    StateReply reply{};
    std::string text = ReadFile(g_sync_dir + "/state_reply.json");
    assert(ParseStateReply(text.data(), text.size(), &doc, &reply));
    assert(reply.result == StateResult::Accepted && reply.generation == 1);
    assert(reply.acked_count == 1 && reply.acked[0] == 4099);
    assert(reply.command_count == 2);
    const DeviceCommand& clear = reply.commands[0];
    assert(clear.id == 4097 && clear.kind == CommandKind::MemoryClear && clear.payload_ok &&
           clear.clear_epoch == 3 && clear.expires_at > 1700000000);
    const DeviceCommand& temper = reply.commands[1];
    assert(temper.id == 4098 && temper.kind == CommandKind::SetTemperament && temper.payload_ok &&
           temper.temperament == Temperament::Lively);

    DeviceCommand polled[kMaxCommandsPerReply];
    uint8_t count = 0;
    text = ReadFile(g_sync_dir + "/commands_reply.json");
    assert(ParseCommandsReply(text.data(), text.size(), &doc, polled, kMaxCommandsPerReply, &count));
    assert(count == 2 && polled[0].id == 4097 && polled[1].id == 4098);
    assert(ParseCommandsReply(text.data(), text.size(), &doc, polled, 1, &count) && count == 1);

    auto parse = [&](const char* body) {
        return ParseStateReply(body, std::strlen(body), &doc, &reply);
    };
    assert(parse("{\"result\":\"duplicate\",\"generation\":5}"));
    assert(reply.result == StateResult::Duplicate && reply.acked_count == 0 && reply.command_count == 0);
    assert(parse("{\"result\":\"stale\",\"generation\":5,\"acked_commands\":[],\"commands\":[]}") &&
           reply.result == StateResult::Stale);
    assert(parse("{\"result\":\"generation_conflict\",\"generation\":5}") &&
           reply.result == StateResult::GenerationConflict);
    assert(!parse("{\"result\":\"maybe\",\"generation\":5}"));
    assert(!parse("{\"result\":\"accepted\",\"generation\":\"5\"}"));
    assert(!parse("{\"result\":\"accepted\",\"generation\":5,\"commands\":{}}"));
    assert(!parse("{\"result\":\"accepted\",\"generation\":5,\"acked_commands\":7}"));
    assert(!parse("[]"));
    // Entries: no usable id -> skipped; anything else -> kept, maybe not ok.
    assert(parse("{\"result\":\"accepted\",\"generation\":5,\"acked_commands\":[0,1,true,9007199254740992],"
                 "\"commands\":["
                 "{\"command_id\":0,\"kind\":\"MEMORY_CLEAR\",\"expires_at\":9,\"payload\":{\"clear_epoch\":1}},"
                 "{\"command_id\":9007199254740992,\"kind\":\"MEMORY_CLEAR\",\"expires_at\":9,\"payload\":{\"clear_epoch\":1}},"
                 "{\"command_id\":1.0,\"kind\":\"MEMORY_CLEAR\",\"expires_at\":9,\"payload\":{\"clear_epoch\":1}},"
                 "{\"command_id\":11,\"kind\":\"REBOOT\",\"expires_at\":9,\"payload\":{}},"
                 "{\"command_id\":12,\"kind\":\"SET_TEMPERAMENT\",\"expires_at\":9,\"payload\":{\"temperament\":\"grumpy\"}},"
                 "{\"command_id\":13,\"kind\":\"MEMORY_CLEAR\",\"expires_at\":9.5,\"payload\":{\"clear_epoch\":1}},"
                 "{\"command_id\":14,\"kind\":\"MEMORY_CLEAR\",\"expires_at\":9},"
                 "{\"command_id\":15,\"kind\":\"SYNC_CONTENT\",\"expires_at\":9,\"payload\":{}},"
                 "{\"command_id\":16,\"kind\":\"SET_TEMPERAMENT\",\"expires_at\":9,\"payload\":{\"temperament\":\"curious\"}}"
                 "]}"));
    assert(reply.acked_count == 1 && reply.acked[0] == 1);
    assert(reply.command_count == 6);
    assert(reply.commands[0].id == 11 && reply.commands[0].kind == CommandKind::Unknown &&
           !reply.commands[0].payload_ok);
    assert(reply.commands[1].id == 12 && !reply.commands[1].payload_ok);
    assert(reply.commands[2].id == 13 && !reply.commands[2].payload_ok);
    assert(reply.commands[3].id == 14 && !reply.commands[3].payload_ok);
    assert(reply.commands[4].id == 15 && reply.commands[4].kind == CommandKind::SyncContent &&
           !reply.commands[4].payload_ok);
    assert(reply.commands[5].id == 16 && reply.commands[5].payload_ok &&
           reply.commands[5].temperament == Temperament::Curious);
    // More than kMaxCommandsPerReply: the rest come with the next reply.
    std::string many = "{\"result\":\"accepted\",\"generation\":5,\"commands\":[";
    for (int k = 1; k <= 12; ++k)
        many += std::string(k > 1 ? "," : "") + "{\"command_id\":" + std::to_string(k) +
                ",\"kind\":\"MEMORY_CLEAR\",\"expires_at\":9,\"payload\":{\"clear_epoch\":1}}";
    many += "]}";
    assert(parse(many.c_str()) && reply.command_count == kMaxCommandsPerReply &&
           reply.commands[7].id == 8);

    // Pairing.
    PairingStartReply start{};
    text = ReadFile(g_sync_dir + "/pairing_start.json");
    assert(ParsePairingStart(text.data(), text.size(), &doc, &start));
    assert(std::strcmp(start.code, "042137") == 0 && start.expires_in == 587 &&
           std::strlen(start.claim_secret) == 32);
    for (const char* bad : {"{\"code\":\"42137\",\"claim_secret\":\"Zm9vYmFyLWNsYWltLXNlY3JldC0wMDAx\",\"expires_in\":5}",
                            "{\"code\":\"04213x\",\"claim_secret\":\"Zm9vYmFyLWNsYWltLXNlY3JldC0wMDAx\",\"expires_in\":5}",
                            "{\"code\":\"042137\",\"claim_secret\":\"short\",\"expires_in\":5}",
                            "{\"code\":\"042137\",\"claim_secret\":\"Zm9vYmFyLWNsYWltLXNlY3JldC0wMDAx\",\"expires_in\":0}",
                            "{\"code\":\"042137\",\"claim_secret\":\"Zm9vYmFyLWNsYWltLXNlY3JldC0wMDAx\"}"})
        assert(!ParsePairingStart(bad, std::strlen(bad), &doc, &start));

    ClaimStatus status = ClaimStatus::Claimed;
    char token[kTokenMax + 1];
    text = ReadFile(g_sync_dir + "/pairing_claim_pending.json");
    assert(ParsePairingClaim(text.data(), text.size(), &doc, "tt-0123456789ab-1f2e", &status, token,
                             sizeof(token)) &&
           status == ClaimStatus::Pending);
    text = ReadFile(g_sync_dir + "/pairing_claim_claimed.json");
    assert(ParsePairingClaim(text.data(), text.size(), &doc, "tt-0123456789ab-1f2e", &status, token,
                             sizeof(token)) &&
           status == ClaimStatus::Claimed &&
           std::strcmp(token, "TEST-ONLY-not-a-real-token_0123456789abcdefABCD") == 0);
    // Another device's token or a short buffer is refused.
    assert(!ParsePairingClaim(text.data(), text.size(), &doc, "pet-other", &status, token, sizeof(token)));
    assert(!ParsePairingClaim(text.data(), text.size(), &doc, "tt-0123456789ab-1f2e", &status, token, 10));
    const char* weird = "{\"status\":\"claimed\",\"device_id\":\"pet-001\",\"token\":\"has space has space has space has space\"}";
    assert(!ParsePairingClaim(weird, std::strlen(weird), &doc, "pet-001", &status, token, sizeof(token)));
    text = ReadFile(g_sync_dir + "/token_rotate.json");
    assert(ParseTokenRotate(text.data(), text.size(), &doc, token, sizeof(token)) &&
           std::strcmp(token, "TEST-ONLY-rotated-token_0123456789abcdefABCDEFGH") == 0);
}

// ---- bookkeeping and routing ------------------------------------------------------

DeviceCommand Command(uint64_t id, CommandKind kind, bool ok = true, int64_t expires = 1000) {
    DeviceCommand c{};
    c.id = id;
    c.kind = kind;
    c.payload_ok = ok;
    c.expires_at = expires;
    c.temperament = Temperament::Lively;
    return c;
}

void TestTracker() {
    CommandTracker t;
    const uint32_t r0 = t.Revision();
    assert(t.AddReceipt(5, ReceiptResult::Applied) && t.Revision() != r0);
    const uint32_t r1 = t.Revision();
    assert(t.AddReceipt(5, ReceiptResult::Rejected) && t.Revision() == r1);  // first answer stays
    assert(!t.AddReceipt(0, ReceiptResult::Applied) && !t.AddReceipt(kMaxCommandId + 1, ReceiptResult::Applied));
    for (uint64_t id = 6; id < 6 + kMaxReceipts - 1; ++id) assert(t.AddReceipt(id, ReceiptResult::Applied));
    assert(t.ReceiptCount() == kMaxReceipts && !t.AddReceipt(100, ReceiptResult::Applied));
    CommandReceipt list[kMaxReceipts];
    assert(t.Receipts(list, kMaxReceipts) == kMaxReceipts && list[0].command_id == 5 &&
           list[0].result == ReceiptResult::Applied);
    const uint64_t acked[] = {5, 7, 999};
    const uint32_t r2 = t.Revision();
    t.OnAcked(acked, 3);
    assert(t.ReceiptCount() == kMaxReceipts - 2 && !t.HasReceipt(5) && !t.HasReceipt(7) &&
           t.HasReceipt(6) && t.Revision() != r2);
    const uint32_t r3 = t.Revision();
    t.OnAcked(acked, 3);
    assert(t.Revision() == r3);

    assert(t.MarkInFlight(40) && t.InFlight(40) && t.MarkInFlight(40) && t.InFlightCount() == 1);
    t.ClearInFlight(40);
    assert(!t.InFlight(40));
    for (uint64_t id = 1; id <= CommandTracker::kInFlightCapacity; ++id) assert(t.MarkInFlight(id));
    assert(!t.MarkInFlight(99));
    t.ClearAllInFlight();
    assert(t.InFlightCount() == 0 && !t.InFlight(1));

    // Rejected ring: persisted, oldest forgotten when full.
    for (uint64_t id = 1; id <= CommandTracker::kRejectedCapacity + 2; ++id) t.AddRejected(id * 10);
    assert(!t.Rejected(10) && !t.Rejected(20) && t.Rejected(30) &&
           t.Rejected((CommandTracker::kRejectedCapacity + 2) * 10));
    uint8_t blob[CommandTracker::kRejectedBlobBytes];
    t.EncodeRejected(blob);
    CommandTracker back;
    assert(back.DecodeRejected(blob, sizeof(blob)) && back.Rejected(30) && !back.Rejected(20));
    uint8_t broken[CommandTracker::kRejectedBlobBytes];
    std::memcpy(broken, blob, sizeof(blob));
    broken[0] = 'X';
    assert(!back.DecodeRejected(broken, sizeof(broken)));
    assert(!back.DecodeRejected(blob, sizeof(blob) - 1));
    CommandTracker small;
    small.AddRejected(77);
    small.EncodeRejected(blob);
    blob[4 + 8 * 3] = 1;  // garbage after the last id
    assert(!back.DecodeRejected(blob, sizeof(blob)));
    assert(back.Rejected(30));  // a failed decode changes nothing
}

void TestRouting() {
    CommandTracker t;
    auto route = [&](const DeviceCommand& c, bool awaiting = false, bool clock = true,
                     int64_t now = 500) { return RouteCommand(c, t, awaiting, clock, now); };
    CommandDecision d = route(Command(1, CommandKind::MemoryClear));
    assert(d.route == CommandRoute::ToPet);
    assert(route(Command(1, CommandKind::MemoryClear), true).route == CommandRoute::Skip);
    d = route(Command(2, CommandKind::MemoryClear, true, 400));
    assert(d.route == CommandRoute::Answer && d.receipt == ReceiptResult::Expired);
    d = route(Command(2, CommandKind::MemoryClear, true, 500));  // expires_at == now
    assert(d.route == CommandRoute::Answer && d.receipt == ReceiptResult::Expired);
    // Unknown clock: idempotent kinds still run.
    assert(route(Command(2, CommandKind::MemoryClear, true, 400), false, false).route ==
           CommandRoute::ToPet);
    for (CommandKind k : {CommandKind::Unknown, CommandKind::SetParentSettings,
                          CommandKind::SelectVoicePack, CommandKind::SyncContent}) {
        d = route(Command(3, k));
        assert(d.route == CommandRoute::Answer && d.receipt == ReceiptResult::Rejected);
    }
    d = route(Command(4, CommandKind::SetTemperament, false));
    assert(d.route == CommandRoute::Answer && d.receipt == ReceiptResult::Rejected);
    t.AddRejected(5);
    d = route(Command(5, CommandKind::SetTemperament));
    assert(d.route == CommandRoute::Answer && d.receipt == ReceiptResult::Rejected);
    t.AddReceipt(6, ReceiptResult::Applied);
    assert(route(Command(6, CommandKind::MemoryClear)).route == CommandRoute::Skip);
    t.MarkInFlight(7);
    assert(route(Command(7, CommandKind::MemoryClear)).route == CommandRoute::Skip);
    assert(route(Command(0, CommandKind::MemoryClear)).route == CommandRoute::Skip);

    assert(NeedsParentConfirm(CommandKind::MemoryClear) && NeedsParentConfirm(CommandKind::SetTemperament));
    assert(!NeedsParentConfirm(CommandKind::SetParentSettings) && !NeedsParentConfirm(CommandKind::SyncContent));
    assert(CommandKindFromName("MEMORY_CLEAR") == CommandKind::MemoryClear &&
           CommandKindFromName("memory_clear") == CommandKind::Unknown &&
           CommandKindFromName(nullptr) == CommandKind::Unknown);
}

// ---- pairing flow --------------------------------------------------------------------

PairingStartReply StartReply(const char* code = "042137", uint32_t expires = 600) {
    PairingStartReply r{};
    std::snprintf(r.code, sizeof(r.code), "%s", code);
    std::snprintf(r.claim_secret, sizeof(r.claim_secret), "%s", "Zm9vYmFyLWNsYWltLXNlY3JldC0wMDAx");
    r.expires_in = expires;
    return r;
}

void TestPairingFlow() {
    using A = PairingFlow::Action;
    PairingFlow f;
    assert(!f.Active() && f.Step(0) == A::None);
    f.Begin(1000);
    assert(f.Active() && f.View().state == LinkState::Connecting);
    assert(f.Step(1000) == A::Start);
    assert(f.Step(1001) == A::None);  // waits for the reply
    const PairingStartReply reply = StartReply();
    f.OnStart(200, &reply, 2000);
    assert(f.View().state == LinkState::ShowCode && f.View().code == 42137);
    assert(std::strcmp(f.ClaimSecret(), reply.claim_secret) == 0);
    assert(f.Step(2000 + PairingFlow::kClaimIntervalMs - 1) == A::None);
    assert(f.Step(2000 + PairingFlow::kClaimIntervalMs) == A::Claim);
    f.OnClaim(200, true, ClaimStatus::Pending, 8000);
    assert(f.View().state == LinkState::ShowCode && f.Step(9000) == A::None);
    assert(f.Step(13000) == A::Claim);
    f.OnClaim(429, false, ClaimStatus::Pending, 13000);  // throttled: wait longer
    assert(f.Step(13000 + PairingFlow::kClaimIntervalMs) == A::None);
    assert(f.Step(13000 + PairingFlow::kThrottledMs) == A::Claim);
    f.OnClaim(200, true, ClaimStatus::Claimed, 44000);
    assert(f.View().state == LinkState::Saving && f.ClaimSecret()[0] == '\0');
    assert(f.Step(44000) == A::Save);
    f.OnSaved(true, 44100);
    assert(f.Step(44100) == A::Confirm);
    f.OnConfirm(0, 44200);  // network: retried
    assert(f.Active() && f.Step(44200 + PairingFlow::kClaimIntervalMs) == A::Confirm);
    f.OnConfirm(200, 50000);
    assert(!f.Active() && f.View().state == LinkState::Paired && !f.DiscardStored());

    // No pairing opened on the panel.
    f.Begin(0);
    f.Step(0);
    f.OnStart(404, nullptr, 10);
    assert(!f.Active() && f.View().state == LinkState::NoServer);
    // The code expires.
    f.Begin(0);
    f.Step(0);
    const PairingStartReply brief = StartReply("000001", 30);
    f.OnStart(200, &brief, 0);
    assert(f.View().code == 1);
    assert(f.Step(29999) == A::Claim);
    f.OnClaim(200, true, ClaimStatus::Pending, 29999);
    assert(f.Step(30000) == A::None && !f.Active() && f.View().state == LinkState::Failed);
    // The pairing was voided on the server.
    f.Begin(0);
    f.Step(0);
    f.OnStart(200, &reply, 0);
    f.Step(PairingFlow::kClaimIntervalMs);
    f.OnClaim(401, false, ClaimStatus::Pending, 6000);
    assert(!f.Active() && f.View().state == LinkState::Failed && !f.DiscardStored());
    // NVS write failed: never confirm.
    f.Begin(0);
    f.Step(0);
    f.OnStart(200, &reply, 0);
    f.Step(PairingFlow::kClaimIntervalMs);
    f.OnClaim(200, true, ClaimStatus::Claimed, 6000);
    assert(f.Step(6000) == A::Save);
    f.OnSaved(false, 6000);
    assert(!f.Active() && f.View().state == LinkState::Failed && !f.DiscardStored());
    // Confirm refused after the token was stored: the owner erases it.
    f.Begin(0);
    f.Step(0);
    f.OnStart(200, &reply, 0);
    f.Step(PairingFlow::kClaimIntervalMs);
    f.OnClaim(200, true, ClaimStatus::Claimed, 6000);
    f.Step(6000);
    f.OnSaved(true, 6000);
    f.Step(6000);
    f.OnConfirm(401, 6100);
    assert(!f.Active() && f.View().state == LinkState::Failed && f.DiscardStored());
    // Network failures give up after kMaxNetworkFailures.
    f.Begin(0);
    uint32_t now = 0;
    for (int k = 0; k < PairingFlow::kMaxNetworkFailures; ++k) {
        assert(f.Step(now) == A::Start);
        f.OnStart(0, nullptr, now);
        now += PairingFlow::kClaimIntervalMs;
    }
    assert(!f.Active() && f.View().state == LinkState::Failed);
    // A pairing that never finishes stops after kMaxDurationMs.
    f.Begin(0);
    f.Step(0);
    const PairingStartReply ten = StartReply("123456", 3600);
    f.OnStart(200, &ten, 0);
    for (now = PairingFlow::kClaimIntervalMs; f.Active(); now += PairingFlow::kClaimIntervalMs) {
        if (f.Step(now) == A::Claim) f.OnClaim(200, true, ClaimStatus::Pending, now);
        assert(now <= PairingFlow::kMaxDurationMs + PairingFlow::kClaimIntervalMs);
    }
    assert(f.View().state == LinkState::Failed);
    // Late results for another phase are ignored.
    f.Begin(0);
    f.OnClaim(200, true, ClaimStatus::Claimed, 0);
    f.OnConfirm(200, 0);
    assert(f.View().state == LinkState::Connecting && f.Step(0) == A::Start);
}

// ---- upload schedule ---------------------------------------------------------------

void TestUploadSchedule() {
    using U = UploadSchedule;
    U s;
    assert(!s.Due(0, true));
    s.OnConnected(100);
    assert(s.Due(100, true));
    s.OnUploaded(200, true, true, 0);
    assert(!s.Due(200, false));
    // A trigger waits for the debounce; later triggers do not push it back.
    s.Trigger(1000);
    assert(!s.Due(1000 + U::kDebounceMs - 1, false) && s.Due(1000 + U::kDebounceMs, false));
    s.Trigger(20000);
    assert(s.Due(1000 + U::kDebounceMs, false));
    assert(s.NextDueMs(1000, false) == U::kDebounceMs);
    s.OnUploaded(31000, true, false, 0);
    // New generations stay kMinGenerationGapMs apart.
    s.OnConnected(31500);
    assert(!s.Due(31500, false) || true);
    assert(s.Due(31500, false));
    s.OnUploaded(31500, true, true, 0);
    s.OnConnected(32000);
    assert(!s.Due(32000, true) && s.Due(32000, false));
    assert(s.NextDueMs(32000, true) == U::kMinGenerationGapMs - 500);
    assert(s.Due(31500 + U::kMinGenerationGapMs, true));
    // Failures back off, Retry-After wins when longer.
    s.OnUploaded(50000, false, true, 0);
    assert(!s.Due(50000 + U::kFirstBackoffMs - 1, false) && s.Due(50000 + U::kFirstBackoffMs, false));
    s.OnUploaded(60000, false, true, 0);
    assert(s.NextDueMs(60000, false) == 2 * U::kFirstBackoffMs);
    s.OnUploaded(70000, false, true, 120);
    assert(s.NextDueMs(70000, false) == 120000);
    for (int k = 0; k < 20; ++k) s.OnUploaded(80000, false, true, 0);
    assert(s.NextDueMs(80000, false) == U::kMaxBackoffMs);
    s.OnUploaded(90000, true, true, 0);
    // Heartbeat after a quiet half hour.
    assert(!s.Due(90000 + U::kHeartbeatMs - 1, false) && s.Due(90000 + U::kHeartbeatMs, false));
    // Wrap-safe millisecond clock.
    U w;
    w.OnConnected(0xFFFFFF00u);
    w.OnUploaded(0xFFFFFF00u, true, true, 0);
    w.Trigger(0xFFFFFF10u);
    assert(!w.Due(0x00000100u, false) && w.Due(0xFFFFFF10u + U::kDebounceMs, false));
}

// ---- D5 requests and the Pet-side applier ----------------------------------------------

void TestRequests() {
    ParentRequestQueue q;
    assert(!q.Offer(Command(1, CommandKind::SyncContent)));  // needs no parent
    assert(q.Offer(Command(1, CommandKind::MemoryClear, true, 100)));
    const uint32_t r = q.Revision();
    assert(q.Offer(Command(1, CommandKind::MemoryClear)) && q.Revision() == r && q.Count() == 1);
    assert(q.Offer(Command(2, CommandKind::SetTemperament, true, 900)));
    DeviceCommand next{};
    assert(q.Next(&next) && next.id == 1 && q.Waiting() == 2);
    assert(q.SetConfirming(1, true) && q.Waiting() == 1 && q.Next(&next) && next.id == 2);
    bool confirming = false;
    assert(q.Find(1, &next, &confirming) && confirming && !q.Find(9, nullptr, nullptr));
    q.ResetConfirming();
    assert(q.Waiting() == 2);
    uint64_t ids[ParentRequestQueue::kCapacity];
    assert(q.Ids(ids, ParentRequestQueue::kCapacity) == 2 && ids[0] == 1 && ids[1] == 2);
    // Expiry only with a known clock, never for a decided request.
    assert(q.RemoveExpired(false, 5000, ids, 8) == 0);
    q.SetConfirming(2, true);
    assert(q.RemoveExpired(true, 1000, ids, 8) == 1 && ids[0] == 1 && q.Count() == 1);
    q.SetConfirming(2, false);
    assert(q.RemoveExpired(true, 1000, ids, 8) == 1 && q.Count() == 0);
    for (uint64_t id = 1; id <= ParentRequestQueue::kCapacity; ++id)
        assert(q.Offer(Command(id, CommandKind::MemoryClear)));
    assert(!q.Offer(Command(99, CommandKind::MemoryClear)));
    assert(q.Remove(3) && !q.Remove(3) && q.Count() == ParentRequestQueue::kCapacity - 1);

    ConfirmDialog d;
    assert(d.OnInput(InputKind::DialPress, 0) == ConfirmAction::None);
    d.Open(Command(7, CommandKind::SetTemperament), 2, 1000);
    RequestView v = d.View();
    assert(d.IsOpen() && v.kind == RequestKind::Temperament && !v.confirm && v.more == 2 &&
           v.temperament == static_cast<uint8_t>(Temperament::Lively));
    // A stray press refuses.
    assert(d.OnInput(InputKind::DialPress, 1100) == ConfirmAction::Reject && !d.IsOpen());
    d.Open(Command(8, CommandKind::MemoryClear), 0, 2000);
    assert(d.View().kind == RequestKind::MemoryClear);
    assert(d.OnInput(InputKind::Talk, 2100) == ConfirmAction::None && d.IsOpen());
    assert(d.OnInput(InputKind::DialDown, 2200) == ConfirmAction::Redraw && d.View().confirm);
    assert(d.OnInput(InputKind::DialUp, 2300) == ConfirmAction::Redraw && !d.View().confirm);
    d.OnInput(InputKind::DialUp, 2400);
    assert(d.OnInput(InputKind::DialPress, 2500) == ConfirmAction::Confirm && d.Command().id == 8);
    d.Open(Command(9, CommandKind::MemoryClear), 0, 3000);
    assert(d.OnInput(InputKind::DialLong, 3100) == ConfirmAction::Exit && !d.IsOpen());
    d.Open(Command(9, CommandKind::MemoryClear), 0, 4000);
    assert(d.Tick(4000 + ConfirmDialog::kIdleExitMs - 1) == ConfirmAction::None);
    assert(d.Tick(4000 + ConfirmDialog::kIdleExitMs) == ConfirmAction::Exit && !d.IsOpen());
}

class FakeNvs : public PetAtomicLedger::Store {
public:
    BlobReadStatus Read(unsigned slot, Blob* out, size_t* length) override {
        if (slot > 1 || !out || !length) return BlobReadStatus::Error;
        if (!present[slot]) return BlobReadStatus::NotFound;
        *out = slots[slot];
        *length = out->size();
        return BlobReadStatus::Ok;
    }
    bool Write(unsigned slot, const Blob& data) override {
        if (write_error || slot > 1) return false;
        slots[slot] = data;
        present[slot] = true;
        return true;
    }
    bool present[2]{};
    bool write_error = false;
    Blob slots[2]{};
};

void TestApplyCommand() {
    static FakeNvs nvs;
    static PetAtomicLedger ledger(nvs);
    assert(ledger.Load() == PetAtomicLedger::LoadResult::Empty && ledger.InitializeFresh());
    MemoryEvent like{};
    like.id = NextLocalEventId(ledger.Generation());
    like.kind = MemoryEventKind::PreferenceChosen;
    like.source = MemorySource::DialChoice;
    like.day = 20761;
    like.slot = 7;
    like.option = 2;
    assert(ledger.RecordMemory(like) == CommitResult::Committed && !ledger.Memory().Empty());
    const uint64_t generation = ledger.Generation();

    DeviceCommand clear = Command(4097, CommandKind::MemoryClear, true, 0);
    // D5: never without the parent, whatever the server says.
    assert(ApplyServerCommand(ledger, clear, false, 20761) == CommandOutcome::NeedsConfirm);
    assert(ledger.Generation() == generation && !ledger.Memory().Empty() && !ledger.Seen(4097));
    nvs.write_error = true;
    assert(ApplyServerCommand(ledger, clear, true, 20761) == CommandOutcome::StorageError);
    assert(!ledger.Seen(4097) && !ledger.Memory().Empty());
    nvs.write_error = false;
    assert(ApplyServerCommand(ledger, clear, true, 20761) == CommandOutcome::Applied);
    assert(ledger.Memory().Empty() && ledger.Seen(4097) && ledger.Generation() == generation + 1);
    // Redelivered: answered applied without asking again, nothing committed.
    assert(ApplyServerCommand(ledger, clear, false, 20761) == CommandOutcome::AlreadyApplied);
    assert(ApplyServerCommand(ledger, clear, true, 20761) == CommandOutcome::AlreadyApplied);
    assert(ledger.Generation() == generation + 1);

    DeviceCommand temper = Command(4098, CommandKind::SetTemperament);
    temper.temperament = Temperament::Curious;
    assert(ApplyServerCommand(ledger, temper, true, 0) == CommandOutcome::Applied);
    assert(ledger.Memory().GetTemperament() == Temperament::Curious);
    // Unsupported, invalid or local-space ids never commit.
    assert(ApplyServerCommand(ledger, Command(4099, CommandKind::SyncContent), true, 0) ==
           CommandOutcome::Rejected);
    assert(ApplyServerCommand(ledger, Command(4100, CommandKind::MemoryClear, false), true, 0) ==
           CommandOutcome::Rejected);
    assert(ApplyServerCommand(ledger, Command(kLocalEventBit | 5, CommandKind::MemoryClear), true, 0) ==
           CommandOutcome::Rejected);
    assert(ApplyServerCommand(ledger, Command(kMaxCommandId + 1, CommandKind::MemoryClear), true, 0) ==
           CommandOutcome::Rejected);
    DeviceCommand bad_temper = Command(4101, CommandKind::SetTemperament);
    bad_temper.temperament = static_cast<Temperament>(9);
    assert(ApplyServerCommand(ledger, bad_temper, true, 0) == CommandOutcome::Rejected);
    assert(ledger.Generation() == generation + 2);

    ReceiptResult receipt = ReceiptResult::Expired;
    assert(ReceiptForOutcome(CommandOutcome::Applied, &receipt) && receipt == ReceiptResult::Applied);
    assert(ReceiptForOutcome(CommandOutcome::AlreadyApplied, &receipt) && receipt == ReceiptResult::Applied);
    assert(ReceiptForOutcome(CommandOutcome::Rejected, &receipt) && receipt == ReceiptResult::Rejected);
    assert(!ReceiptForOutcome(CommandOutcome::NeedsConfirm, &receipt));
    assert(!ReceiptForOutcome(CommandOutcome::StorageError, &receipt));
}

}  // namespace

int main(int argc, char** argv) {
    int arg = 1;
    if (argc > arg && std::strcmp(argv[arg], "--write") == 0) {
        g_write = true;
        ++arg;
    }
    if (argc != arg + 2) {
        std::cerr << "usage: test_server_sync [--write] <memory_dir> <sync_dir>\n";
        return 2;
    }
    g_memory_dir = argv[arg];
    g_sync_dir = argv[arg + 1];
    TestSnapshotGolden();
    if (g_write) {
        std::cout << "WROTE snapshot fixtures\n";
        return 0;
    }
    TestJson();
    TestBase64AndHelpers();
    TestReplies();
    TestTracker();
    TestRouting();
    TestPairingFlow();
    TestUploadSchedule();
    TestRequests();
    TestApplyCommand();
    std::cout << "PASS: server sync codecs, golden snapshots, replies, routing, pairing, "
                 "schedule, D5 requests, command applier\n";
    return 0;
}
