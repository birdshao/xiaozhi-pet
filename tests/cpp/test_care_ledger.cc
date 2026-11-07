// V8 ledger schema v4: the CareStore rides in the same atomic blob as the
// Pet checkpoint, memory and parent settings. A real v3 blob written by the
// pre-V8 code (tests/fixtures/ledger/v3_pre_v8.bin) must load unchanged and
// upgrade on the next commit; guards, replay and stickers go through the
// same single commit.
#include "kid_app/care_store.h"
#include "kid_app/pet_atomic_ledger.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace kid_app;
using Ledger = PetAtomicLedger;

namespace {

class FakeNvs : public Ledger::Store {
public:
    BlobReadStatus Read(unsigned slot, Blob* out, size_t* length) override {
        if (slot > 1 || !out || !length) return BlobReadStatus::Error;
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
        ++writes;
        return true;
    }
    bool present[2]{};
    bool write_error = false;
    int writes = 0;
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
    uint64_t g[2]{};
    for (unsigned s = 0; s < 2; ++s)
        if (d.present[s])
            for (size_t i = 0; i < 8; ++i) g[s] |= uint64_t(d.slots[s][8 + i]) << (8 * i);
    return g[1] > g[0] ? 1 : 0;
}

std::vector<uint8_t> ReadFile(const std::string& path) {
    std::vector<uint8_t> data;
    FILE* f = std::fopen(path.c_str(), "rb");
    assert(f && "fixture missing");
    uint8_t buf[512];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) data.insert(data.end(), buf, buf + n);
    std::fclose(f);
    return data;
}

FakeNvs FromV3Fixture(const std::string& dir) {
    const auto v3 = ReadFile(dir + "/v3_pre_v8.bin");
    assert(v3.size() == Ledger::kV3Bytes);
    FakeNvs disk;
    disk.present[0] = true;
    disk.lengths[0] = v3.size();
    std::memcpy(disk.slots[0].data(), v3.data(), v3.size());
    return disk;
}

CareEvent Care(uint64_t id, CareEventKind kind, MemorySource src, uint32_t minute = 0,
               uint16_t slot = 0, uint16_t option = 0) {
    CareEvent e{};
    e.id = id;
    e.kind = kind;
    e.source = src;
    e.minute = minute;
    e.slot = slot;
    e.option = option;
    return e;
}

constexpr uint32_t kDay = 20399;  // what the fixture used (2025-11-07)
constexpr uint32_t kMinute = kDay * 1440 + 9 * 60;

void TestRealV3BlobUpgrades(const std::string& dir) {
    FakeNvs disk = FromV3Fixture(dir);
    const auto v3_bytes = disk.slots[0];
    Ledger l(disk);
    assert(l.Load() == Ledger::LoadResult::Ready);
    // Everything from v3 is kept; care starts empty.
    assert(l.Generation() == 5);
    assert(l.Snapshot().interaction_count == 1);
    assert(l.Memory().RecentBookAt(0) == 7 && l.Memory().GetGrowth().books_finished == 1);
    uint16_t option = 0;
    assert(l.Memory().FindPreference(3, &option) && option == 2);
    assert(l.Parent().volume_cap_pct == 70 && !l.Parent().motion_enabled);
    assert(l.Care() == CareStore{});
    // The replay window survives: old ids are duplicates, also for care.
    PetEffects fx{};
    assert(l.ApplyDurably({101, PetAction::Feed}, &fx) == CommitResult::Duplicate);
    assert(l.RecordCare(Care(103, CareEventKind::Bathed, MemorySource::Device)) ==
           CommitResult::Duplicate);
    assert(disk.writes == 0);  // loading never writes

    // First V8 commit: v4 into the other slot; the v3 slot stays as rollback.
    uint32_t earned = 0;
    assert(l.RecordCare(Care(200, CareEventKind::Fed, MemorySource::DialChoice, kMinute, 0, 1),
                        &earned) == CommitResult::Committed);
    // The v3 milestones (first book, first like) become stickers right away.
    assert(earned & StickerBit(Sticker::FirstFeed));
    assert(earned & StickerBit(Sticker::FirstBook));
    assert(earned & StickerBit(Sticker::FirstLike));
    assert(disk.lengths[1] == Ledger::kBytes && disk.lengths[0] == Ledger::kV3Bytes &&
           disk.slots[0] == v3_bytes);
    assert(ActiveSlot(disk) == 1 && disk.slots[1][4] == 4 && disk.slots[1][5] == 0);  // v4

    Ledger after(disk);
    assert(after.Load() == Ledger::LoadResult::Ready);
    assert(after.Generation() == 6 && after.Care() == l.Care());
    assert(after.Care().FoodCount(Food::Fish) == 1 && after.Memory().RecentBookAt(0) == 7);
    assert(after.Parent().volume_cap_pct == 70);
    assert(after.RecordCare(Care(200, CareEventKind::Fed, MemorySource::DialChoice, kMinute, 0, 1)) ==
           CommitResult::Duplicate);

    // A corrupt care section is a corrupt v4 blob: the v3 slot is used.
    FakeNvs bad = disk;
    bad.slots[1][Ledger::kV3Bytes - 4 + 95] = 1;  // reserved byte of the care store
    PutCrc(bad.slots[1].data(), Ledger::kBytes);
    Ledger fallback(bad);
    assert(fallback.Load() == Ledger::LoadResult::Ready);
    assert(fallback.Generation() == 5 && fallback.Care() == CareStore{});
    // A v3-sized blob that claims version 4 is corrupt.
    FakeNvs liar = FromV3Fixture(dir);
    liar.slots[0][4] = 4;
    PutCrc(liar.slots[0].data(), Ledger::kV3Bytes);
    Ledger rejected(liar);
    assert(rejected.Load() == Ledger::LoadResult::Corrupt);
    // A v4-sized blob that claims version 3 is corrupt too.
    FakeNvs liar4 = disk;
    liar4.present[0] = false;
    liar4.slots[1][4] = 3;
    PutCrc(liar4.slots[1].data(), Ledger::kBytes);
    Ledger rejected4(liar4);
    assert(rejected4.Load() == Ledger::LoadResult::Corrupt);
}

void TestGuardsReplayAndPowerLoss() {
    FakeNvs disk;
    Ledger l(disk);
    assert(l.Load() == Ledger::LoadResult::Empty && l.InitializeFresh());
    const int writes = disk.writes;
    // Voice and network never write care or growth: no commit at all.
    for (CareEventKind k : {CareEventKind::Fed, CareEventKind::Bathed, CareEventKind::HabitDone,
                            CareEventKind::Dressed, CareEventKind::HabitSet,
                            CareEventKind::FestivalSeen}) {
        for (MemorySource src : {MemorySource::Voice, MemorySource::Network}) {
            assert(l.RecordCare(Care(300 + static_cast<uint64_t>(k), k, src, kMinute, 0, 1)) ==
                   CommitResult::Rejected);
        }
    }
    assert(disk.writes == writes && l.Care() == CareStore{});
    // A rejected id is not burnt: the real event with that id still applies.
    assert(l.RecordCare(Care(300, CareEventKind::Fed, MemorySource::DialChoice, kMinute, 0, 2)) ==
           CommitResult::Committed);
    assert(l.RecordCare(Care(300, CareEventKind::Fed, MemorySource::DialChoice, kMinute, 0, 2)) ==
           CommitResult::Duplicate);
    assert(l.Care().FoodCount(Food::Milk) == 1);
    // Care ids share the window with pet and memory ids.
    PetEffects fx{};
    assert(l.ApplyDurably({300, PetAction::Feed}, &fx) == CommitResult::Duplicate);
    // Power loss (write fails): nothing changes, then the retry commits once.
    const CareStore before = l.Care();
    const uint64_t gen = l.Generation();
    disk.write_error = true;
    assert(l.RecordCare(Care(301, CareEventKind::Bathed, MemorySource::Device, kMinute)) ==
           CommitResult::StorageError);
    assert(l.Care() == before && l.Generation() == gen && !l.Seen(301));
    disk.write_error = false;
    assert(l.RecordCare(Care(301, CareEventKind::Bathed, MemorySource::Device, kMinute)) ==
           CommitResult::Committed);
    assert(l.Care().GetTotals().baths == 1);
    // Care is never queued in the server outbox (the snapshot reports it).
    assert(l.PendingCount() == 0);
}

void TestStickersMemoryAndWipe() {
    FakeNvs disk;
    Ledger l(disk);
    assert(l.Load() == Ledger::LoadResult::Empty && l.InitializeFresh());
    l.SetProjection(false);
    uint64_t id = 1000;
    // Finished books: sticker in the same commit, and they count as play.
    assert(l.RecordCare(Care(id++, CareEventKind::Tickled, MemorySource::Device, kMinute)) ==
           CommitResult::Committed);
    assert(l.Care().Needs(kMinute + 6 * 60, DayPhase::Day).bored == 2);
    MemoryEvent book{};
    book.id = id++;
    book.kind = MemoryEventKind::BookFinished;
    book.source = MemorySource::Device;
    book.day = kDay;
    book.option = 5;
    assert(l.RecordMemory(book, kMinute + 6 * 60) == CommitResult::Committed);
    assert(l.Care().Stickers() & StickerBit(Sticker::FirstBook));
    assert(l.Care().Needs(kMinute + 6 * 60, DayPhase::Day).bored == 0);
    // Seven active days of feeding -> the seven-days sticker, via pet actions.
    PetEffects fx{};
    for (uint32_t d = 0; d < 7; ++d)
        assert(l.ApplyDurably({id++, PetAction::Feed}, &fx, kDay + d) == CommitResult::Committed);
    assert(l.Memory().GetGrowth().active_days >= 7);
    assert(l.Care().Stickers() & StickerBit(Sticker::SevenDays));
    // Parent settings for V8 live in the care store (no ParentSettingsCodec v2).
    assert(l.RecordCare(Care(id++, CareEventKind::BirthdaySet, MemorySource::Parent, 0, 5, 20)) ==
           CommitResult::Committed);
    assert(l.RecordCare(Care(id++, CareEventKind::HabitSet, MemorySource::Parent, 0, 0, 2)) ==
           CommitResult::Committed);
    assert(l.RecordCare(Care(id++, CareEventKind::HabitDone, MemorySource::Button, kMinute)) ==
           CommitResult::Committed);
    // Parent wipe forgets the child's care, keeps birthday and habit preset.
    MemoryEvent wipe{};
    wipe.id = id++;
    wipe.kind = MemoryEventKind::MemoryCleared;
    wipe.source = MemorySource::Parent;
    assert(l.RecordMemory(wipe) == CommitResult::Committed);
    assert(l.Care().Stickers() == 0 && l.Care().GetTotals().All() == 0 &&
           l.Care().GetHabit().total == 0 && l.Care().GetHabit().habit == Habit::WashHands &&
           l.Care().BirthMonth() == 5 && l.Care().BirthDay() == 20);
    Ledger reboot(disk);
    assert(reboot.Load() == Ledger::LoadResult::Ready && reboot.Care() == l.Care());
}

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "tests/fixtures/ledger";
    static_assert(Ledger::kV3Bytes == 1796 && Ledger::kBytes == 1892, "v4 = v3 + 96");
    TestRealV3BlobUpgrades(dir);
    TestGuardsReplayAndPowerLoss();
    TestStickersMemoryAndWipe();
    std::puts("PASS: V8 ledger v4: real v3 blob upgrade, care guards, replay, power loss, stickers");
    return 0;
}
