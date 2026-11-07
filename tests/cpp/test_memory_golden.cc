// Golden vectors shared with the PetLearning Python decoders (server S2).
//
//   test_memory_golden <dir>          check: every vector in <dir> is exactly
//                                     what the C++ codecs produce / reject
//   test_memory_golden --write <dir>  regenerate the vectors from C++
//
// Valid vectors are built through the real MemoryStore::Apply / NotePetAction
// rules and ParentSettingsCodec::Encode; corrupt vectors patch a valid one and
// must be rejected by the C++ decoders. services/pet_learning/memory_codec.py
// is asserted against the same files (tests/test_v5_server_memory_codec.py).
#include "kid_app/memory_store.h"
#include "kid_app/parent_settings.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

using namespace kid_app;
using K = MemoryEventKind;
using Src = MemorySource;
using Bytes = std::vector<uint8_t>;
using Patch = std::pair<size_t, uint8_t>;

namespace {

bool g_write = false;
std::string g_dir;
int g_files = 0;

MemoryEvent Ev(uint64_t id, K kind, Src source, uint32_t day, uint16_t slot,
               uint16_t option) {
    MemoryEvent e{};
    e.id = id;
    e.kind = kind;
    e.source = source;
    e.day = day;
    e.slot = slot;
    e.option = option;
    return e;
}
void Apply(MemoryStore* m, const MemoryEvent& e) {
    const MemoryResult r = m->Apply(e);
    assert(r == MemoryResult::Applied);
    (void)r;
}
void Like(MemoryStore* m, uint64_t id, uint16_t slot, uint16_t option, uint32_t day) {
    Apply(m, Ev(id, K::PreferenceChosen, Src::DialChoice, day, slot, option));
}
void Book(MemoryStore* m, uint64_t id, uint16_t book, uint32_t day) {
    Apply(m, Ev(id, K::BookFinished, Src::Device, day, 0, book));
}
void Game(MemoryStore* m, uint64_t id, uint16_t game, uint32_t day) {
    Apply(m, Ev(id, K::GameCompleted, Src::Device, day, 0, game));
}
void SetTemperament(MemoryStore* m, uint64_t id, Temperament t) {
    Apply(m, Ev(id, K::TemperamentSet, Src::Parent, 0, 0, static_cast<uint16_t>(t)));
}

// Writes (--write) or compares (check) one file byte for byte.
void Golden(const std::string& name, const Bytes& bytes) {
    const std::string path = g_dir + "/" + name;
    if (g_write) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        assert(out.good());
    } else {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            std::cerr << "missing golden vector " << path << "\n";
            assert(false);
        }
        const Bytes stored((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
        if (stored != bytes) {
            std::cerr << "golden vector differs from C++ codec: " << path << "\n";
            assert(false);
        }
    }
    ++g_files;
}

Bytes Encode(const MemoryStore& m) {
    Bytes b(MemoryStore::kBytes);
    const bool ok = m.Encode(b.data(), b.size());
    assert(ok);
    (void)ok;
    return b;
}

void ValidMemory(const std::string& name, const MemoryStore& m) {
    const Bytes b = Encode(m);
    MemoryStore back;
    assert(MemoryStore::Decode(b.data(), b.size(), &back) && back == m);
    Golden("memory_" + name + ".bin", b);
    char json[1024];
    const bool ok = FormatMinimalSummary(MakeMinimalSummary(m), json, sizeof(json));
    assert(ok);
    (void)ok;
    Golden("memory_" + name + ".summary.json",
           Bytes(json, json + std::strlen(json)));
}

void CorruptMemory(const std::string& name, Bytes b, std::initializer_list<Patch> patches,
                   size_t size = MemoryStore::kBytes) {
    for (const Patch& p : patches) b[p.first] = p.second;
    b.resize(size);
    MemoryStore out;
    assert(!MemoryStore::Decode(b.data(), b.size(), &out));
    Golden("memory_bad_" + name + ".bin", b);
}

MemoryStore Rich() {
    // Two dated days: likes, books, a game, feeds and a greeting.
    MemoryStore m;
    SetTemperament(&m, 1, Temperament::Curious);
    Like(&m, 2, 3, 4, 20760);
    Book(&m, 3, 17, 20760);
    m.NotePetAction(PetAction::Feed, 20760);
    m.NotePetAction(PetAction::Pickup, 20761);
    m.NotePetAction(PetAction::Feed, 20761);
    m.NotePetAction(PetAction::Feed, 20761);
    Game(&m, 4, 5, 20761);
    Like(&m, 5, 7, 2, 20761);
    Book(&m, 6, 9, 20761);
    assert(m.Today().day == 20761 && m.Yesterday().day == 20760);
    assert(m.Today().feeds == 2 && m.Today().greetings == 1);
    assert(m.GetGrowth().milestones == 41);
    return m;
}

MemoryStore Full() {
    // Clock unknown (day 0); 17 likes evict the oldest topic, 9 books evict
    // the oldest book and a re-read moves book 5 to the front.
    MemoryStore m;
    for (uint16_t i = 0; i < 17; ++i) Like(&m, 10 + i, i + 1, i + 1, 0);
    for (uint16_t i = 0; i < 9; ++i) Book(&m, 100 + i, i + 1, 0);
    Book(&m, 109, 5, 0);
    assert(m.PreferenceCount() == 16 && m.PreferenceAt(0).slot == 17);
    assert(m.RecentBookCount() == 8 && m.RecentBookAt(0) == 5);
    assert(m.GetGrowth().milestones == 39);
    return m;
}

MemoryStore Wiped() {
    MemoryStore m;
    SetTemperament(&m, 1, Temperament::Lively);
    Like(&m, 2, 1, 1, 30);
    Book(&m, 3, 4, 31);
    Apply(&m, Ev(4, K::MemoryCleared, Src::Parent, 0, 0, 0));
    assert(m.Empty() && m.GetTemperament() == Temperament::Lively);
    return m;
}

MemoryStore Capped() {
    MemoryStore m;
    for (int i = 0; i < 260; ++i) m.NotePetAction(PetAction::Feed, 40);
    assert(m.Today().feeds == MemoryStore::kTodayCap);
    return m;
}

void MemoryVectors() {
    ValidMemory("empty", MemoryStore{});
    ValidMemory("rich", Rich());
    ValidMemory("full", Full());
    ValidMemory("wiped", Wiped());
    ValidMemory("capped", Capped());

    // Layout (memory_store.cc): today @4, yesterday @18, growth @32,
    // books @44, preferences @60 (12 bytes each), reserved @252.
    const Bytes rich = Encode(Rich());
    CorruptMemory("format_v2", rich, {{0, 2}});
    CorruptMemory("format_v0", rich, {{0, 0}});
    CorruptMemory("temperament", rich, {{1, 3}});
    CorruptMemory("pref_count_over", rich, {{2, 17}});
    CorruptMemory("pref_count_empty_entry", rich, {{2, 3}});
    CorruptMemory("book_count_over", rich, {{3, 9}});
    CorruptMemory("day_padding", rich, {{4 + 9, 1}});
    CorruptMemory("feeds_over_cap", rich, {{4 + 4, 251}});
    CorruptMemory("milestone_bit", rich, {{32 + 11, 0x80}});
    CorruptMemory("unused_pref_nonzero", rich, {{60 + 12 * 2, 1}});
    CorruptMemory("duplicate_slot", rich, {{60 + 12, 7}});
    CorruptMemory("reserved_tail", rich, {{MemoryStore::kBytes - 1, 1}});
    CorruptMemory("yesterday_not_before_today", rich, {{18, 0x19}});
    CorruptMemory("zero_book", rich, {{44 + 2, 0}});
    CorruptMemory("duplicate_book", rich, {{44 + 2, 9}});
    CorruptMemory("zero_source_event", rich, {{60 + 4, 0}});
    CorruptMemory("last_game_without_games", rich, {{18 + 12, 1}});
    CorruptMemory("truncated", rich, {}, MemoryStore::kBytes - 1);
}

ParentSettings Custom() {
    ParentSettings s;
    s.schedule.wake_min = 6 * 60 + 30;
    s.schedule.evening_min = 19 * 60;
    s.schedule.quiet_start_min = 20 * 60;
    s.schedule.quiet_end_min = 6 * 60 + 30;
    s.schedule.nap_enabled = true;
    s.schedule.nap_start_min = 13 * 60;
    s.schedule.nap_end_min = 14 * 60 + 30;
    s.volume_cap_pct = 70;
    s.quiet_volume_pct = 0;
    s.motion_enabled = false;
    return s;
}

Bytes EncodeSettings(const ParentSettings& s) {
    Bytes b(ParentSettingsCodec::kBytes);
    const bool ok = ParentSettingsCodec::Encode(s, b.data(), b.size());
    assert(ok);
    (void)ok;
    return b;
}

void FixCrc(Bytes* b) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < 24; ++i) {
        crc ^= (*b)[i];
        for (unsigned j = 0; j < 8; ++j) crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0);
    }
    crc = ~crc;
    for (size_t i = 0; i < 4; ++i) (*b)[24 + i] = static_cast<uint8_t>(crc >> (8 * i));
}

// fix_crc: recompute the CRC so the decoder must reject the field itself.
void CorruptSettings(const std::string& name, Bytes b, std::initializer_list<Patch> patches,
                     bool fix_crc, size_t size = ParentSettingsCodec::kBytes) {
    for (const Patch& p : patches) b[p.first] = p.second;
    if (fix_crc) FixCrc(&b);
    b.resize(size);
    ParentSettings out;
    assert(!ParentSettingsCodec::Decode(b.data(), b.size(), &out));
    Golden("settings_bad_" + name + ".bin", b);
}

void SettingsVectors() {
    for (const auto& item : {std::make_pair(std::string("default"), ParentSettings{}),
                             std::make_pair(std::string("custom"), Custom())}) {
        const Bytes b = EncodeSettings(item.second);
        ParentSettings back;
        assert(ParentSettingsCodec::Decode(b.data(), b.size(), &back) && back == item.second);
        Golden("settings_" + item.first + ".bin", b);
    }
    const Bytes good = EncodeSettings(Custom());
    CorruptSettings("tag", good, {{0, 'X'}}, true);
    CorruptSettings("version_2", good, {{2, 2}}, true);
    CorruptSettings("version_0", good, {{2, 0}}, true);
    CorruptSettings("crc", good, {{24, static_cast<uint8_t>(good[24] ^ 1)}}, false);
    CorruptSettings("payload_without_crc", good, {{18, 60}}, false);
    CorruptSettings("unknown_flag", good, {{4, static_cast<uint8_t>(good[4] | 0x04)}}, true);
    CorruptSettings("byte5", good, {{5, 1}}, true);
    CorruptSettings("reserved", good, {{21, 1}}, true);
    CorruptSettings("volume_step", good, {{18, 75}}, true);
    CorruptSettings("volume_cap_zero", good, {{18, 0}}, true);
    CorruptSettings("quiet_volume_over", good, {{19, 110}}, true);
    CorruptSettings("quiet_empty", good, {{12, good[10]}, {13, good[11]}}, true);
    CorruptSettings("minute_out_of_day", good, {{6, 0xA0}, {7, 0x05}}, true);
    CorruptSettings("nap_after_evening", good, {{16, 0xB0}, {17, 0x04}}, true);
    CorruptSettings("truncated", good, {}, false, ParentSettingsCodec::kBytes - 1);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--write") {
        g_write = true;
        g_dir = argv[2];
    } else if (argc == 2) {
        g_dir = argv[1];
    } else {
        std::cerr << "usage: test_memory_golden [--write] <dir>\n";
        return 2;
    }
    MemoryVectors();
    SettingsVectors();
    std::cout << (g_write ? "wrote " : "checked ") << g_files
              << " memory/settings golden vectors\n";
    return 0;
}
