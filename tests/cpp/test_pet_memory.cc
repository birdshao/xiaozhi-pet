#include "kid_app/memory_store.h"
#include "kid_app/pet_atomic_ledger.h"

#include <cassert>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <string>

using namespace kid_app;
using Ledger = PetAtomicLedger;
using K = MemoryEventKind;
using Src = MemorySource;

namespace {

MemoryEvent Ev(uint64_t id, K kind, Src source, uint32_t day = 0,
               uint16_t slot = 0, uint16_t option = 0) {
    MemoryEvent e{};
    e.id = id;
    e.kind = kind;
    e.source = source;
    e.day = day;
    e.slot = slot;
    e.option = option;
    return e;
}
MemoryEvent Like(uint64_t id, uint16_t slot, uint16_t option, uint32_t day = 0) {
    return Ev(id, K::PreferenceChosen, Src::DialChoice, day, slot, option);
}
MemoryEvent Book(uint64_t id, uint16_t book, uint32_t day = 0) {
    return Ev(id, K::BookFinished, Src::Device, day, 0, book);
}
MemoryEvent Game(uint64_t id, uint16_t game, uint32_t day = 0) {
    return Ev(id, K::GameCompleted, Src::Device, day, 0, game);
}
MemoryEvent Wipe(uint64_t id) { return Ev(id, K::MemoryCleared, Src::Parent); }

class FakeNvs : public Ledger::Store {
public:
    BlobReadStatus Read(unsigned slot, Blob* out, size_t* length) override {
        if (read_error || slot > 1 || !out || !length) return BlobReadStatus::Error;
        if (!present[slot]) return BlobReadStatus::NotFound;
        *out = slots[slot];
        *length = lengths[slot];
        return BlobReadStatus::Ok;
    }
    bool Write(unsigned slot, const Blob& data) override {
        if (write_error || slot > 1) return false;
        slots[slot] = data;
        lengths[slot] = data.size();
        present[slot] = true;
        return true;
    }
    bool present[2]{};
    bool read_error = false;
    bool write_error = false;
    size_t lengths[2]{};
    Blob slots[2]{};
};

// Independent reference CRC (not the code under test).
uint32_t Crc(const uint8_t* b, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        crc ^= b[i];
        for (int j = 0; j < 8; ++j) crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
    }
    return ~crc;
}
void PutCrc(uint8_t* b, size_t length) {
    const uint32_t c = Crc(b, length - 4);
    for (size_t i = 0; i < 4; ++i) b[length - 4 + i] = static_cast<uint8_t>(c >> (8 * i));
}
int ActiveSlot(const FakeNvs& d) {
    // The slot with the higher generation (bytes 8..15).
    uint64_t g[2]{};
    for (unsigned s = 0; s < 2; ++s)
        for (size_t i = 0; i < 8; ++i) g[s] |= uint64_t(d.slots[s][8 + i]) << (8 * i);
    return g[1] > g[0] ? 1 : 0;
}

void TestGuardRejectsEveryOtherLikePath() {
    MemoryStore m;
    const MemoryStore before = m;
    // V5-03: only a dial choice question may write likes.
    for (Src s : {Src::Button, Src::Device, Src::Parent, Src::Voice, Src::Network}) {
        assert(m.Apply(Ev(10, K::PreferenceChosen, s, 0, 1, 2)) == MemoryResult::Rejected);
    }
    // Voice / network data never writes any memory (no free-speech extraction).
    for (Src s : {Src::Voice, Src::Network}) {
        assert(m.Apply(Ev(11, K::BookFinished, s, 0, 0, 7)) == MemoryResult::Rejected);
        assert(m.Apply(Ev(12, K::GameCompleted, s, 0, 0, 7)) == MemoryResult::Rejected);
        assert(m.Apply(Ev(13, K::MemoryCleared, s)) == MemoryResult::Rejected);
        assert(m.Apply(Ev(14, K::TemperamentSet, s, 0, 0, 0)) == MemoryResult::Rejected);
    }
    // Only the parent wipes or sets temperament; the parent cannot invent
    // finished books or likes.
    for (Src s : {Src::DialChoice, Src::Button, Src::Device}) {
        assert(m.Apply(Ev(15, K::MemoryCleared, s)) == MemoryResult::Rejected);
        assert(m.Apply(Ev(16, K::TemperamentSet, s, 0, 0, 2)) == MemoryResult::Rejected);
    }
    assert(m.Apply(Ev(17, K::BookFinished, Src::Parent, 0, 0, 3)) == MemoryResult::Rejected);
    // Malformed payloads.
    assert(m.Apply(Like(0, 1, 2)) == MemoryResult::Rejected);   // no event id
    assert(m.Apply(Like(18, 0, 2)) == MemoryResult::Rejected);  // no slot
    assert(m.Apply(Like(19, 1, 0)) == MemoryResult::Rejected);  // no option
    assert(m.Apply(Book(20, 0)) == MemoryResult::Rejected);
    assert(m.Apply(Game(21, 0)) == MemoryResult::Rejected);
    assert(m.Apply(Ev(22, K::TemperamentSet, Src::Parent, 0, 0, 3)) == MemoryResult::Rejected);
    assert(m.Apply(Ev(23, static_cast<K>(9), Src::Parent)) == MemoryResult::Rejected);
    assert(m.Apply(Ev(24, K::BookFinished, static_cast<Src>(9), 0, 0, 3)) ==
           MemoryResult::Rejected);
    assert(m == before && m.Empty());

    assert(m.Apply(Like(30, 1, 2)) == MemoryResult::Applied);
    uint16_t option = 0;
    assert(m.FindPreference(1, &option) && option == 2);
    assert(m.PreferenceAt(0).source_event == 30);
}

void TestTodayRolloverAndClockSkew() {
    MemoryStore m;
    m.NotePetAction(PetAction::Feed);     // before the clock is known
    m.NotePetAction(PetAction::Pickup);
    m.NotePetAction(PetAction::StartStory);  // not a memory counter
    assert(m.Today().day == 0 && m.Today().feeds == 1 && m.Today().greetings == 1);
    assert(m.GetGrowth().active_days == 1);

    assert(m.Apply(Book(1, 42, 100)) == MemoryResult::Applied);
    assert(m.Today().day == 100 && m.Today().feeds == 1 && m.Today().books == 1);
    assert(m.Today().last_book == 42);

    assert(m.Apply(Game(2, 5, 101)) == MemoryResult::Applied);  // next day
    assert(m.Yesterday().day == 100 && m.Yesterday().books == 1 &&
           m.Yesterday().feeds == 1 && m.Yesterday().last_book == 42);
    assert(m.Today().day == 101 && m.Today().games == 1 && m.Today().feeds == 0);
    assert(m.GetGrowth().active_days == 2);

    // Clock went backwards: growth and recent books still count, today not.
    assert(m.Apply(Book(3, 43, 99)) == MemoryResult::Applied);
    assert(m.Today().day == 101 && m.Today().books == 0);
    assert(m.GetGrowth().books_finished == 2 && m.RecentBookAt(0) == 43);

    // Days without any activity in between: yesterday = last active day.
    assert(m.Apply(Like(4, 1, 1, 103)) == MemoryResult::Applied);
    assert(m.Yesterday().day == 101 && m.Today().day == 103 && m.Today().choices == 1);
}

void TestBoundsAndEviction() {
    MemoryStore m;
    for (uint16_t s = 1; s <= 20; ++s) assert(m.Apply(Like(s, s, 100 + s)) == MemoryResult::Applied);
    assert(m.PreferenceCount() == MemoryStore::kPreferenceCapacity);
    assert(m.PreferenceAt(0).slot == 20);                      // newest first
    assert(m.PreferenceAt(15).slot == 5);                      // 1..4 evicted (LRU)
    assert(!m.FindPreference(4, nullptr) && m.FindPreference(5, nullptr));
    // Re-answering a topic replaces it in place of a new entry.
    assert(m.Apply(Like(21, 5, 999)) == MemoryResult::Applied);
    uint16_t opt = 0;
    assert(m.PreferenceCount() == 16 && m.PreferenceAt(0).slot == 5);
    assert(m.FindPreference(5, &opt) && opt == 999);
    assert(m.PreferenceAt(15).slot == 6);
    assert(m.GetGrowth().preferences_chosen == 21);

    for (uint16_t b = 1; b <= 12; ++b) assert(m.Apply(Book(100 + b, b)) == MemoryResult::Applied);
    assert(m.RecentBookCount() == MemoryStore::kRecentBookCapacity);
    assert(m.RecentBookAt(0) == 12 && m.RecentBookAt(7) == 5);
    assert(m.Apply(Book(200, 7)) == MemoryResult::Applied);  // re-read moves to front
    assert(m.RecentBookCount() == 8 && m.RecentBookAt(0) == 7 && m.RecentBookAt(1) == 12);
    assert(m.RecentBookAt(7) == 5);

    for (int i = 0; i < 400; ++i) m.NotePetAction(PetAction::Feed);
    assert(m.Today().feeds == MemoryStore::kTodayCap);
    for (int i = 0; i < 300; ++i) assert(m.Apply(Game(1000 + i, 3)) == MemoryResult::Applied);
    assert(m.Today().games == MemoryStore::kTodayCap);
    assert(m.GetGrowth().games_completed == 300);
}

void TestMilestones() {
    MemoryStore m;
    assert(m.GetGrowth().milestones == 0);
    assert(m.Apply(Book(1, 1)) == MemoryResult::Applied);
    assert(m.GetGrowth().milestones == MemoryStore::kFirstBook);
    for (uint16_t b = 2; b <= 5; ++b) assert(m.Apply(Book(b, b)) == MemoryResult::Applied);
    assert(m.GetGrowth().milestones & MemoryStore::kFiveBooks);
    assert(!(m.GetGrowth().milestones & MemoryStore::kTenBooks));
    for (uint32_t d = 1; d <= 7; ++d) assert(m.Apply(Game(100 + d, 1, d)) == MemoryResult::Applied);
    const uint32_t ms = m.GetGrowth().milestones;
    assert((ms & MemoryStore::kFirstGame) && (ms & MemoryStore::kSevenDays));
    assert(!(ms & MemoryStore::kTenGames) && !(ms & MemoryStore::kFirstPreference));
}

void TestCodecIsStrictAndCanonical() {
    MemoryStore m;
    assert(m.Apply(Ev(1, K::TemperamentSet, Src::Parent, 0, 0, 0)) == MemoryResult::Applied);
    assert(m.Apply(Like(2, 3, 4, 10)) == MemoryResult::Applied);
    assert(m.Apply(Like(3, 5, 6, 10)) == MemoryResult::Applied);
    assert(m.Apply(Book(4, 77, 11)) == MemoryResult::Applied);
    m.NotePetAction(PetAction::Feed);

    uint8_t blob[MemoryStore::kBytes];
    assert(!m.Encode(blob, sizeof(blob) - 1));
    assert(m.Encode(blob, sizeof(blob)));
    MemoryStore back;
    assert(MemoryStore::Decode(blob, sizeof(blob), &back) && back == m);
    assert(back.GetTemperament() == Temperament::Curious);
    assert(back.Today().day == 11 && back.Yesterday().day == 10);

    auto rejects = [&](size_t at, uint8_t value) {
        uint8_t bad[MemoryStore::kBytes];
        std::memcpy(bad, blob, sizeof(bad));
        bad[at] = value;
        MemoryStore out;
        const MemoryStore untouched = out;
        return !MemoryStore::Decode(bad, sizeof(bad), &out) && out == untouched;
    };
    assert(rejects(0, 2));      // unknown memory format
    assert(rejects(1, 3));      // temperament out of range
    assert(rejects(2, 17));     // preference count over capacity
    assert(rejects(2, 3));      // count says 3, third entry is empty
    assert(rejects(3, 9));      // recent books over capacity
    assert(rejects(4 + 9, 1));  // Day padding
    assert(rejects(4 + 4, 251));  // feeds over the daily cap
    assert(rejects(32 + 11, 0x80));  // unknown milestone bit
    assert(rejects(60 + 12 * 2, 1)); // unused preference entry not zero
    assert(rejects(60 + 12, 5));     // duplicate slot (entries are slot 5, slot 3)
    assert(rejects(MemoryStore::kBytes - 1, 1));  // reserved tail
    assert(rejects(18, 12));  // yesterday must be before today
    assert(!MemoryStore::Decode(blob, sizeof(blob), nullptr));
}

void TestWipeKeepsTemperamentOnly() {
    MemoryStore m;
    assert(m.Apply(Ev(1, K::TemperamentSet, Src::Parent, 0, 0, 2)) == MemoryResult::Applied);
    assert(m.Empty());  // profile is not memory
    assert(m.Apply(Like(2, 1, 1, 5)) == MemoryResult::Applied);
    assert(m.Apply(Book(3, 9, 6)) == MemoryResult::Applied);
    m.NotePetAction(PetAction::Pickup);
    assert(!m.Empty());
    assert(m.Apply(Wipe(4)) == MemoryResult::Applied);
    assert(m.Empty() && m.GetTemperament() == Temperament::Lively);
    assert(m.PreferenceCount() == 0 && m.RecentBookCount() == 0);
    assert(m.Today() == MemoryStore::Day{} && m.Yesterday() == MemoryStore::Day{});
    assert(m.GetGrowth().milestones == 0 && m.GetGrowth().books_finished == 0);
}

void TestMinimalSummaryHasNoIdentifiers() {
    MemoryStore m;
    const uint64_t secret_id = 0x8000000000ABCDEFull;
    assert(m.Apply(Like(secret_id, 2, 31, 20456)) == MemoryResult::Applied);
    assert(m.Apply(Book(secret_id + 1, 17, 20456)) == MemoryResult::Applied);
    m.NotePetAction(PetAction::Feed);
    m.NotePetAction(PetAction::Feed);

    const MinimalSummary s = MakeMinimalSummary(m);
    assert(s.temperament == Temperament::Gentle && s.preference_count == 1);
    assert(s.preferences[0][0] == 2 && s.preferences[0][1] == 31);
    assert(s.feeds == 2 && s.books == 1 && s.last_book == 17);

    char out[512];
    assert(FormatMinimalSummary(s, out, sizeof(out)));
    const std::string json(out);
    assert(json ==
           "{\"trait\":\"gentle\",\"preferences\":[{\"slot\":2,\"option\":31}],"
           "\"today\":{\"feed\":2,\"greet\":0,\"books\":1,\"games\":0},"
           "\"last_book\":17,\"milestones\":33}");
    // No event ids (audit trail) and no dates leave the device.
    assert(json.find("20456") == std::string::npos);
    assert(json.find("abcdef") == std::string::npos);
    assert(json.find(std::to_string(secret_id)) == std::string::npos);
    char tiny[20];
    assert(!FormatMinimalSummary(s, tiny, sizeof(tiny)) && tiny[0] == '\0');
    assert(!FormatMinimalSummary(s, nullptr, 10));

    MemoryStore empty;
    assert(FormatMinimalSummary(MakeMinimalSummary(empty), out, sizeof(out)));
    assert(std::string(out) ==
           "{\"trait\":\"gentle\",\"preferences\":[],\"today\":{\"feed\":0,\"greet\":0,"
           "\"books\":0,\"games\":0},\"last_book\":0,\"milestones\":0}");
}

// Builds a v1 (pre-V5) blob from first principles: drop the memory block,
// write version 1 / size 1480 and a fresh CRC.
void MakeV1(FakeNvs* disk, unsigned slot) {
    auto& b = disk->slots[slot];
    const size_t memory_at = Ledger::kV1Bytes - 4;
    for (size_t i = memory_at; i < b.size(); ++i) b[i] = 0;
    b[4] = 1; b[5] = 0;
    b[6] = static_cast<uint8_t>(Ledger::kV1Bytes);
    b[7] = static_cast<uint8_t>(Ledger::kV1Bytes >> 8);
    PutCrc(b.data(), Ledger::kV1Bytes);
    disk->lengths[slot] = Ledger::kV1Bytes;
}

void TestLedgerMigratesV1WithoutLosingPetState() {
    FakeNvs disk;
    {
        Ledger old(disk);
        assert(old.Load() == Ledger::LoadResult::Empty && old.InitializeFresh());
        PetEffects fx{};
        assert(old.ApplyDurably({123, PetAction::Feed}, &fx) == CommitResult::Committed);
    }
    const unsigned v1_slot = static_cast<unsigned>(ActiveSlot(disk));
    MakeV1(&disk, v1_slot);
    const auto v1_bytes = disk.slots[v1_slot];

    Ledger migrated(disk);
    assert(migrated.Load() == Ledger::LoadResult::Ready);
    assert(migrated.Snapshot().interaction_count == 1);
    assert(migrated.PendingCount() == 1);
    assert(migrated.Memory().Empty());  // v1 had no memory
    PetEffects fx{};
    assert(migrated.ApplyDurably({123, PetAction::Feed}, &fx) == CommitResult::Duplicate);

    assert(migrated.RecordMemory(Book(500, 8, 30)) == CommitResult::Committed);
    const unsigned v2_slot = v1_slot ? 0u : 1u;
    assert(disk.lengths[v2_slot] == Ledger::kBytes);
    // The v1 slot is preserved as the rollback copy.
    assert(disk.lengths[v1_slot] == Ledger::kV1Bytes && disk.slots[v1_slot] == v1_bytes);

    Ledger after(disk);
    assert(after.Load() == Ledger::LoadResult::Ready);
    assert(after.Snapshot().interaction_count == 1 && after.PendingCount() == 1);
    assert(after.Memory().RecentBookAt(0) == 8 && after.Memory().GetGrowth().books_finished == 1);

    // A v1-length blob that claims v2, or a v1 blob with a bad CRC, is corrupt.
    FakeNvs bad = disk;
    bad.present[v2_slot] = false;
    bad.slots[v1_slot][4] = 2;
    PutCrc(bad.slots[v1_slot].data(), Ledger::kV1Bytes);
    Ledger refuse(bad);
    assert(refuse.Load() == Ledger::LoadResult::Corrupt);
    FakeNvs crc = disk;
    crc.present[v2_slot] = false;
    crc.slots[v1_slot][40] ^= 1;
    Ledger refuse_crc(crc);
    assert(refuse_crc.Load() == Ledger::LoadResult::Corrupt);
    FakeNvs odd = disk;
    odd.present[v2_slot] = false;
    odd.lengths[v1_slot] = Ledger::kV1Bytes + 4;  // neither v1 nor v2 size
    Ledger refuse_size(odd);
    assert(refuse_size.Load() == Ledger::LoadResult::Corrupt);
}

void TestLedgerReplayNeverDoubleCounts() {
    FakeNvs disk;
    Ledger l(disk);
    assert(l.Load() == Ledger::LoadResult::Empty && l.InitializeFresh());
    assert(l.RecordMemory(Book(500, 3, 40)) == CommitResult::Committed);
    assert(l.RecordMemory(Like(501, 1, 2, 40)) == CommitResult::Committed);
    const auto gen = l.Generation();
    assert(l.RecordMemory(Book(500, 3, 40)) == CommitResult::Duplicate);
    assert(l.RecordMemory(Like(501, 1, 9, 40)) == CommitResult::Duplicate);
    assert(l.Generation() == gen);

    PetEffects fx{};
    assert(l.ApplyDurably({600, PetAction::Feed}, &fx) == CommitResult::Committed);
    assert(l.ApplyDurably({600, PetAction::Feed}, &fx) == CommitResult::Duplicate);
    // Memory and Pet events share one replay window.
    assert(l.ApplyDurably({500, PetAction::Pickup}, &fx) == CommitResult::Duplicate);
    assert(l.RecordMemory(Book(600, 4, 40)) == CommitResult::Duplicate);

    Ledger rebooted(disk);
    assert(rebooted.Load() == Ledger::LoadResult::Ready);
    assert(rebooted.RecordMemory(Book(500, 3, 40)) == CommitResult::Duplicate);
    assert(rebooted.ApplyDurably({600, PetAction::Feed}, &fx) == CommitResult::Duplicate);
    const auto& m = rebooted.Memory();
    assert(m.GetGrowth().books_finished == 1 && m.Today().books == 1);
    assert(m.Today().feeds == 1 && m.Today().choices == 1);
    uint16_t opt = 0;
    assert(m.FindPreference(1, &opt) && opt == 2);
    assert(rebooted.Snapshot().interaction_count == 1);
}

void TestLedgerGuardAndPowerLoss() {
    FakeNvs disk;
    Ledger l(disk);
    assert(l.RecordMemory(Book(1, 1)) == CommitResult::StorageError);  // not loaded
    assert(l.Load() == Ledger::LoadResult::Empty && l.InitializeFresh());
    const auto gen = l.Generation();
    assert(l.RecordMemory(Ev(2, K::PreferenceChosen, Src::Voice, 0, 1, 1)) ==
           CommitResult::Rejected);
    assert(l.RecordMemory(Ev(3, K::PreferenceChosen, Src::Device, 0, 1, 1)) ==
           CommitResult::Rejected);
    assert(l.RecordMemory(Ev(4, K::MemoryCleared, Src::Network)) == CommitResult::Rejected);
    assert(l.RecordMemory(Like(0, 1, 1)) == CommitResult::Rejected);
    assert(l.Generation() == gen && l.Memory().Empty());
    // A rejected id is not burned: the genuine dial answer may reuse it.
    assert(l.RecordMemory(Like(2, 1, 1)) == CommitResult::Committed);

    disk.write_error = true;
    assert(l.RecordMemory(Book(10, 5)) == CommitResult::StorageError);
    PetEffects fx{};
    assert(l.ApplyDurably({11, PetAction::Feed}, &fx) == CommitResult::StorageError);
    assert(l.Memory().RecentBookCount() == 0 && l.Memory().Today().feeds == 0);
    assert(l.Snapshot().interaction_count == 0);
    disk.write_error = false;
    // The same ids retry cleanly after the storage failure.
    assert(l.RecordMemory(Book(10, 5)) == CommitResult::Committed);
    assert(l.ApplyDurably({11, PetAction::Feed}, &fx) == CommitResult::Committed);
    assert(l.Memory().RecentBookAt(0) == 5 && l.Memory().Today().feeds == 1);
}

void TestLedgerParentWipe() {
    FakeNvs disk;
    Ledger l(disk);
    assert(l.Load() == Ledger::LoadResult::Empty && l.InitializeFresh());
    assert(l.RecordMemory(Ev(1, K::TemperamentSet, Src::Parent, 0, 0, 2)) ==
           CommitResult::Committed);
    PetEffects fx{};
    assert(l.ApplyDurably({2, PetAction::Feed}, &fx) == CommitResult::Committed);
    assert(l.RecordMemory(Like(3, 1, 2, 50)) == CommitResult::Committed);
    assert(l.RecordMemory(Book(4, 6, 50)) == CommitResult::Committed);
    assert(!l.Memory().Empty());

    assert(l.RecordMemory(Wipe(5)) == CommitResult::Committed);
    assert(l.Memory().Empty());
    assert(l.Memory().GetTemperament() == Temperament::Lively);
    assert(l.Snapshot().interaction_count == 1);  // Pet state is not memory
    assert(l.PendingCount() == 1);                // nor the outbox

    assert(l.RecordMemory(Like(6, 7, 8, 51)) == CommitResult::Committed);
    // A replayed old wipe must not erase what was remembered afterwards.
    assert(l.RecordMemory(Wipe(5)) == CommitResult::Duplicate);
    Ledger rebooted(disk);
    assert(rebooted.Load() == Ledger::LoadResult::Ready);
    assert(rebooted.Memory().PreferenceCount() == 1);
    assert(rebooted.Memory().FindPreference(7, nullptr));
    assert(!rebooted.Memory().FindPreference(1, nullptr));
    assert(rebooted.Memory().GetTemperament() == Temperament::Lively);
}

// Review finding: a week of feeding only (no dated memory events) must not
// pile up in one stale "today".
void TestDatedPetActionsRollTheDay() {
    FakeNvs disk;
    Ledger l(disk);
    assert(l.Load() == Ledger::LoadResult::Empty && l.InitializeFresh());
    assert(l.RecordMemory(Book(1, 42, 100)) == CommitResult::Committed);
    PetEffects fx{};
    for (uint32_t day = 101; day <= 107; ++day) {
        assert(l.ApplyDurably({1000 + day, PetAction::Feed}, &fx, day) ==
               CommitResult::Committed);
    }
    const auto& m = l.Memory();
    assert(m.Today().day == 107 && m.Today().feeds == 1);
    assert(m.Yesterday().day == 106 && m.Yesterday().feeds == 1);
    assert(m.GetGrowth().active_days == 8);
    assert(m.GetGrowth().milestones & MemoryStore::kSevenDays);
    assert(m.BookReadOn(100) == 0);  // day 100 is no longer kept
    // Clock went backwards: the feed still commits to the Pet, not to today.
    assert(l.ApplyDurably({2000, PetAction::Feed}, &fx, 90) == CommitResult::Committed);
    assert(m.Today().day == 107 && m.Today().feeds == 1);
    assert(l.Snapshot().interaction_count == 8);
    // Undated pet actions still count into the current day.
    assert(l.ApplyDurably({2001, PetAction::Pickup}, &fx) == CommitResult::Committed);
    assert(m.Today().greetings == 1);

    Ledger rebooted(disk);
    assert(rebooted.Load() == Ledger::LoadResult::Ready);
    assert(rebooted.Memory() == m);

    // A summary asked for on a later day reports nothing done today yet.
    const MinimalSummary later = MakeMinimalSummary(m, 110);
    assert(!later.feeds && !later.greetings && !later.books && !later.games);
    assert(later.last_book == 42);  // recent books are not per-day
    const MinimalSummary now = MakeMinimalSummary(m, 107);
    assert(now.feeds == 1 && now.greetings == 1);
}

void TestBookReadOn() {
    MemoryStore m;
    assert(m.BookReadOn(0) == 0 && m.BookReadOn(5) == 0);
    assert(m.Apply(Book(1, 7, 10)) == MemoryResult::Applied);
    assert(m.Apply(Book(2, 8, 10)) == MemoryResult::Applied);
    assert(m.BookReadOn(10) == 8);
    m.NotePetAction(PetAction::Feed, 11);
    assert(m.BookReadOn(10) == 8 && m.BookReadOn(11) == 0);
    assert(m.Apply(Book(3, 9, 11)) == MemoryResult::Applied);
    assert(m.BookReadOn(11) == 9 && m.BookReadOn(10) == 8 && m.BookReadOn(9) == 0);
}

void TestLedgerRejectsCorruptMemoryBlock() {
    FakeNvs disk;
    Ledger l(disk);
    assert(l.Load() == Ledger::LoadResult::Empty && l.InitializeFresh());
    assert(l.RecordMemory(Book(1, 2)) == CommitResult::Committed);
    assert(l.RecordMemory(Book(3, 4)) == CommitResult::Committed);
    // Newest slot: valid CRC but a non-canonical memory block (reserved byte).
    const unsigned newest = static_cast<unsigned>(ActiveSlot(disk));
    disk.slots[newest][Ledger::kV2Bytes - 5] = 1;  // last memory byte
    PutCrc(disk.slots[newest].data(), Ledger::kBytes);
    Ledger fallback(disk);
    assert(fallback.Load() == Ledger::LoadResult::Ready);
    assert(fallback.Memory().RecentBookCount() == 1 && fallback.Memory().RecentBookAt(0) == 2);
}

}  // namespace

int main() {
    static_assert(MemoryStore::kBytes == 288, "memory block is fixed-size");
    TestGuardRejectsEveryOtherLikePath();
    TestTodayRolloverAndClockSkew();
    TestBoundsAndEviction();
    TestMilestones();
    TestCodecIsStrictAndCanonical();
    TestWipeKeepsTemperamentOnly();
    TestMinimalSummaryHasNoIdentifiers();
    TestLedgerMigratesV1WithoutLosingPetState();
    TestLedgerReplayNeverDoubleCounts();
    TestLedgerGuardAndPowerLoss();
    TestLedgerParentWipe();
    TestLedgerRejectsCorruptMemoryBlock();
    TestDatedPetActionsRollTheDay();
    TestBookReadOn();
    std::cout << "PASS: V5 memory guard, bounds, codec, v1 migration, replay, wipe, summary\n";
}
