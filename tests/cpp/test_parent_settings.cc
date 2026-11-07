#include "kid_app/parent_menu.h"
#include "kid_app/parent_settings.h"
#include "kid_app/pet_atomic_ledger.h"

#include <cassert>
#include <cstring>
#include <initializer_list>
#include <iostream>

using namespace kid_app;
using Ledger = PetAtomicLedger;
using Action = ParentMenuAction;

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
        for (size_t i = 0; i < 8; ++i) g[s] |= uint64_t(d.slots[s][8 + i]) << (8 * i);
    return g[1] > g[0] ? 1 : 0;
}

ParentSettings Custom() {
    ParentSettings s;
    s.schedule.wake_min = 6 * 60 + 45;
    s.schedule.evening_min = 19 * 60;
    s.schedule.quiet_start_min = 20 * 60;
    s.schedule.quiet_end_min = 6 * 60 + 30;
    s.schedule.nap_enabled = true;
    s.volume_cap_pct = 60;
    s.quiet_volume_pct = 20;
    s.motion_enabled = false;
    return s;
}

void TestDefaultsAndValidation() {
    const ParentSettings d;
    assert(ValidParentSettings(d));
    assert(d.volume_cap_pct == 100 && d.quiet_volume_pct == 30 && d.motion_enabled);
    assert(d.schedule.wake_min == 7 * 60 && d.schedule.quiet_start_min == 20 * 60 + 30);
    assert(ValidParentSettings(Custom()));

    ParentSettings s;
    s.volume_cap_pct = 0;  // the cap never mutes everything
    assert(!ValidParentSettings(s));
    s.volume_cap_pct = 105;
    assert(!ValidParentSettings(s));
    s.volume_cap_pct = 55;  // steps of 10
    assert(!ValidParentSettings(s));
    s = ParentSettings{};
    s.quiet_volume_pct = 0;  // silent nights are allowed
    assert(ValidParentSettings(s));
    s.quiet_volume_pct = 110;
    assert(!ValidParentSettings(s));
    // The day plan goes through RhythmScheduler::Valid.
    s = ParentSettings{};
    s.schedule.evening_min = s.schedule.wake_min;
    assert(!ValidParentSettings(s));
    s = ParentSettings{};
    s.schedule.quiet_end_min = 8 * 60;  // wake-up inside quiet hours
    assert(!ValidParentSettings(s));
}

void TestVolumeLimit() {
    ParentSettings s;
    // Defaults reproduce the V3-01 prompt volumes: full by day, 30 % at night.
    assert(LimitVolumePct(s, 100, false) == 100);
    assert(LimitVolumePct(s, 100, true) == 30);
    assert(LimitVolumePct(s, 15, true) == 4);  // self-test "volume 15" at night
    s.volume_cap_pct = 50;
    assert(LimitVolumePct(s, 100, false) == 50);
    assert(LimitVolumePct(s, 40, false) == 20);
    assert(LimitVolumePct(s, 100, true) == 30);  // the lower ceiling wins
    s.quiet_volume_pct = 80;
    assert(LimitVolumePct(s, 100, true) == 50);
    s.quiet_volume_pct = 0;
    assert(LimitVolumePct(s, 100, true) == 0 && LimitVolumePct(s, 100, false) == 50);
    assert(LimitVolumePct(s, 250, false) == 50);  // requests above 100 % clamp
    s.volume_cap_pct = 7;  // invalid settings: defaults, never louder
    assert(LimitVolumePct(s, 100, false) == 100 && LimitVolumePct(s, 100, true) == 30);
}

void TestCodec() {
    uint8_t b[ParentSettingsCodec::kBytes];
    for (const ParentSettings& s : {ParentSettings{}, Custom()}) {
        assert(ParentSettingsCodec::Encode(s, b, sizeof(b)));
        ParentSettings out;
        out.volume_cap_pct = 10;
        assert(ParentSettingsCodec::Decode(b, sizeof(b), &out) && out == s);
    }
    // Byte layout is stable little-endian (shared by NVS and ledger v3).
    assert(ParentSettingsCodec::Encode(Custom(), b, sizeof(b)));
    assert(b[0] == 'P' && b[1] == 'S' && b[2] == 1 && b[3] == 0);
    assert(b[4] == 0x01 && b[5] == 0);  // nap on, motion off
    assert(b[6] == ((6 * 60 + 45) & 0xFF) && b[7] == ((6 * 60 + 45) >> 8));
    assert(b[18] == 60 && b[19] == 20);
    for (size_t i = 20; i < 24; ++i) assert(b[i] == 0);
    uint32_t crc = 0;
    for (size_t i = 0; i < 4; ++i) crc |= uint32_t(b[24 + i]) << (8 * i);
    assert(crc == Crc(b, 24));

    // Never encodes invalid settings, never into a short buffer.
    ParentSettings bad;
    bad.volume_cap_pct = 0;
    assert(!ParentSettingsCodec::Encode(bad, b, sizeof(b)));
    assert(!ParentSettingsCodec::Encode(Custom(), b, sizeof(b) - 1));
    assert(!ParentSettingsCodec::Encode(Custom(), nullptr, sizeof(b)));

    // Rejects: wrong tag/version, reserved bits, bad CRC, invalid values
    // (even with a correct CRC), short input. *out stays untouched.
    assert(ParentSettingsCodec::Encode(Custom(), b, sizeof(b)));
    const ParentSettings keep;
    auto rejects = [&](size_t at, uint8_t value, bool fix_crc) {
        uint8_t c[ParentSettingsCodec::kBytes];
        std::memcpy(c, b, sizeof(c));
        c[at] = value;
        if (fix_crc) PutCrc(c, sizeof(c));
        ParentSettings out = keep;
        const bool ok = ParentSettingsCodec::Decode(c, sizeof(c), &out);
        return !ok && out == keep;
    };
    assert(rejects(0, 'X', true));
    assert(rejects(2, 2, true));          // future version
    assert(rejects(4, 0x05, true));       // unknown flag
    assert(rejects(5, 1, true));
    assert(rejects(21, 1, true));         // reserved tail
    assert(rejects(18, 0, true));         // cap 0 %
    assert(rejects(19, 15, true));        // not a 10 % step
    assert(rejects(9, 0x05, true));       // evening 21:xx after quiet start
    assert(rejects(18, 70, false));       // CRC mismatch
    ParentSettings out;
    assert(!ParentSettingsCodec::Decode(b, sizeof(b) - 1, &out));
    assert(!ParentSettingsCodec::Decode(nullptr, sizeof(b), &out));
}

// V8 page 2: the good habit preset and the birthday (month, day). They are
// care settings: saving answers SavedCare and never touches ParentSettings.
void TestCareRows() {
    using kid_app::ParentCare;
    ParentMenu menu;
    menu.Open(ParentSettings{}, 0);
    for (uint8_t i = 0; i < static_cast<uint8_t>(ParentItem::Habit); ++i)
        menu.OnInput(InputKind::DialDown, 0);
    assert(menu.Selected() == ParentItem::Habit && menu.Care() == ParentCare{});
    // Habit: none -> presets 1..8, clamped; press saves.
    assert(menu.OnInput(InputKind::DialPress, 0) == Action::Redraw && menu.Editing());
    // While editing, dial up raises the value (like every other row).
    assert(menu.OnInput(InputKind::DialDown, 0) == Action::None);  // already "none"
    for (int i = 0; i < 3; ++i) assert(menu.OnInput(InputKind::DialUp, 0) == Action::Redraw);
    assert(menu.View().care.habit == 3 && menu.Care().habit == 0);  // draft only
    assert(menu.OnInput(InputKind::DialPress, 0) == Action::SavedCare);
    assert(menu.Care().habit == 3 && menu.Settings() == ParentSettings{});
    // Unchanged value: a press just leaves editing.
    menu.OnInput(InputKind::DialPress, 0);
    assert(menu.OnInput(InputKind::DialPress, 0) == Action::Redraw);
    menu.OnInput(InputKind::DialPress, 0);
    for (int i = 0; i < 10; ++i) menu.OnInput(InputKind::DialUp, 0);
    assert(menu.View().care.habit == 8);
    menu.OnInput(InputKind::DialPress, 0);
    // Birthday: the day needs a month; the month keeps the day valid.
    menu.OnInput(InputKind::DialDown, 0);
    menu.OnInput(InputKind::DialDown, 0);
    assert(menu.Selected() == ParentItem::BirthDay);
    menu.OnInput(InputKind::DialPress, 0);
    assert(menu.OnInput(InputKind::DialUp, 0) == Action::None);  // no month yet
    menu.OnInput(InputKind::DialPress, 0);
    menu.OnInput(InputKind::DialUp, 0);
    assert(menu.Selected() == ParentItem::BirthMonth);
    menu.OnInput(InputKind::DialPress, 0);
    menu.OnInput(InputKind::DialUp, 0);  // January, day 1
    assert(menu.View().care.birth_month == 1 && menu.View().care.birth_day == 1);
    assert(menu.OnInput(InputKind::DialPress, 0) == Action::SavedCare);
    menu.OnInput(InputKind::DialDown, 0);
    menu.OnInput(InputKind::DialPress, 0);
    for (int i = 0; i < 40; ++i) menu.OnInput(InputKind::DialUp, 0);
    assert(menu.View().care.birth_day == 31);  // clamped at the month's end
    assert(menu.OnInput(InputKind::DialPress, 0) == Action::SavedCare);
    menu.OnInput(InputKind::DialUp, 0);
    menu.OnInput(InputKind::DialPress, 0);
    menu.OnInput(InputKind::DialUp, 0);  // February: 31 -> 29
    assert(menu.View().care.birth_month == 2 && menu.View().care.birth_day == 29);
    menu.OnInput(InputKind::DialDown, 0);
    menu.OnInput(InputKind::DialDown, 0);  // not set: the day goes too
    assert(menu.View().care.birth_month == 0 && menu.View().care.birth_day == 0);
    // Long press drops the draft; the saved birthday stays.
    assert(menu.OnInput(InputKind::DialLong, 0) == Action::Exit);
    assert(menu.Care().birth_month == 1 && menu.Care().birth_day == 31);
    // Invalid care values from storage open as "not set".
    ParentCare bad{};
    bad.habit = 9;
    bad.birth_month = 2;
    bad.birth_day = 30;
    menu.Open(ParentSettings{}, 0, bad);
    assert(menu.Care() == ParentCare{});
    // Recording failed: the previous values come back.
    ParentCare good{};
    good.habit = 2;
    menu.Open(ParentSettings{}, 0, good);
    menu.RevertCare(ParentCare{});
    assert(menu.Care() == ParentCare{} && !menu.Editing());
    assert(std::strcmp(ParentItemName(ParentItem::BirthMonth), "birth_month") == 0);
}

void TestMenuNavigation() {
    ParentMenu menu;
    assert(!menu.IsOpen() && menu.OnInput(InputKind::DialDown, 0) == Action::None);
    menu.Open(ParentSettings{}, 1000);
    assert(menu.IsOpen() && !menu.Editing() && menu.Selected() == ParentItem::Wake);
    // Up at the top stays put; down walks every row and stops at the end.
    assert(menu.OnInput(InputKind::DialUp, 1100) == Action::None);
    for (uint8_t i = 1; i < kParentItemCount; ++i) {
        assert(menu.OnInput(InputKind::DialDown, 1100) == Action::Redraw);
        assert(menu.View().cursor == i && !menu.View().editing);
    }
    assert(menu.Selected() == ParentItem::BirthDay);  // V8 page 2 is the end
    assert(menu.OnInput(InputKind::DialDown, 1200) == Action::None);
    for (int i = 0; i < 3; ++i) assert(menu.OnInput(InputKind::DialUp, 1210) == Action::Redraw);
    assert(menu.Selected() == ParentItem::Server);
    // The Server row opens the pairing page: no editing, settings unchanged.
    assert(menu.OnInput(InputKind::DialPress, 1250) == Action::OpenLink);
    assert(menu.IsOpen() && !menu.Editing() && menu.Settings() == ParentSettings{});
    assert(menu.View().page == kid_app::ParentPage::Menu);
    assert(menu.OnInput(InputKind::DialUp, 1260) == Action::Redraw);
    assert(menu.Selected() == ParentItem::Motion);
    assert(menu.OnInput(InputKind::DialDown, 1270) == Action::Redraw);
    // Other keys do nothing (but count as input).
    for (InputKind k : {InputKind::Talk, InputKind::BootLong, InputKind::SleepRequest,
                        InputKind::ParentCombo})
        assert(menu.OnInput(k, 1300) == Action::None && menu.IsOpen());
    // Reopening starts at the top again.
    menu.Open(menu.Settings(), 2000);
    assert(menu.Selected() == ParentItem::Wake);
    // Long press leaves.
    assert(menu.OnInput(InputKind::DialLong, 2100) == Action::Exit && !menu.IsOpen());
    assert(menu.Tick(999999) == Action::None);
    // Invalid settings open as the defaults.
    ParentSettings bad;
    bad.volume_cap_pct = 3;
    menu.Open(bad, 0);
    assert(menu.Settings() == ParentSettings{});
}

void TestMenuIdleExit() {
    ParentMenu menu;
    menu.Open(ParentSettings{}, 0xFFFFF000u);  // wraps during the wait
    assert(menu.Tick(0xFFFFF000u + ParentMenu::kIdleExitMs - 1) == Action::None);
    assert(menu.OnInput(InputKind::DialDown, 0xFFFFF000u + 20000) == Action::Redraw);
    assert(menu.Tick(0xFFFFF000u + 20000 + ParentMenu::kIdleExitMs - 1) == Action::None);
    // An unsaved draft is dropped when the menu times out.
    assert(menu.OnInput(InputKind::DialPress, 0xFFFFF000u + 21000) == Action::Redraw);
    assert(menu.OnInput(InputKind::DialUp, 0xFFFFF000u + 21000) == Action::Redraw);
    assert(menu.View().settings != menu.Settings());
    assert(menu.Tick(0xFFFFF000u + 21000 + ParentMenu::kIdleExitMs) == Action::Exit);
    assert(!menu.IsOpen() && menu.Settings() == ParentSettings{});
}

void TestMenuEditsTimes() {
    ParentMenu menu;
    menu.Open(ParentSettings{}, 0);
    // Press edits; up/down change the draft only; press saves.
    assert(menu.OnInput(InputKind::DialPress, 10) == Action::Redraw && menu.Editing());
    assert(menu.OnInput(InputKind::DialUp, 20) == Action::Redraw);
    assert(menu.View().editing && menu.View().settings.schedule.wake_min == 7 * 60 + 15);
    assert(menu.Settings().schedule.wake_min == 7 * 60);  // not saved yet
    assert(menu.OnInput(InputKind::DialUp, 30) == Action::Redraw);
    assert(menu.OnInput(InputKind::DialDown, 40) == Action::Redraw);
    assert(menu.OnInput(InputKind::DialDown, 40) == Action::Redraw);
    // 06:45 lies in the default quiet hours (20:30-07:00): the dial stops.
    assert(menu.OnInput(InputKind::DialDown, 40) == Action::None);
    assert(menu.View().settings.schedule.wake_min == 7 * 60);
    assert(menu.OnInput(InputKind::DialUp, 45) == Action::Redraw);
    assert(menu.OnInput(InputKind::DialPress, 50) == Action::Saved && !menu.Editing());
    assert(menu.Settings().schedule.wake_min == 7 * 60 + 15);
    assert(menu.Settings() == menu.View().settings);

    // Unchanged draft: press just stops editing (nothing to persist).
    assert(menu.OnInput(InputKind::DialPress, 60) == Action::Redraw);
    assert(menu.OnInput(InputKind::DialPress, 70) == Action::Redraw && !menu.Editing());

    // Long press while editing drops the draft and leaves.
    const ParentSettings saved = menu.Settings();
    assert(menu.OnInput(InputKind::DialPress, 80) == Action::Redraw);
    assert(menu.OnInput(InputKind::DialUp, 90) == Action::Redraw);
    assert(menu.OnInput(InputKind::DialLong, 100) == Action::Exit);
    assert(menu.Settings() == saved);
}

void TestMenuStopsAtInvalidTimes() {
    ParentMenu menu;
    menu.Open(ParentSettings{}, 0);
    // Evening (19:30) up: 19:45, 20:00, 20:15, then 20:30 would be the start
    // of quiet hours: refused until quiet hours move later.
    assert(menu.OnInput(InputKind::DialDown, 0) == Action::Redraw);
    assert(menu.Selected() == ParentItem::Evening);
    assert(menu.OnInput(InputKind::DialPress, 0) == Action::Redraw);
    for (int want : {19 * 60 + 45, 20 * 60, 20 * 60 + 15}) {
        assert(menu.OnInput(InputKind::DialUp, 0) == Action::Redraw);
        assert(menu.View().settings.schedule.evening_min == want);
    }
    assert(menu.OnInput(InputKind::DialUp, 0) == Action::None);
    assert(menu.View().settings.schedule.evening_min == 20 * 60 + 15);
    // Quiet end crosses midnight backwards when the plan allows it.
    ParentSettings late;
    late.schedule.quiet_start_min = 23 * 60;
    late.schedule.quiet_end_min = 0;
    assert(ValidParentSettings(late));
    ParentMenu wrap;
    wrap.Open(late, 0);
    for (int i = 0; i < 3; ++i) wrap.OnInput(InputKind::DialDown, 0);
    assert(wrap.Selected() == ParentItem::QuietEnd);
    wrap.OnInput(InputKind::DialPress, 0);
    assert(wrap.OnInput(InputKind::DialDown, 0) == Action::Redraw);
    assert(wrap.View().settings.schedule.quiet_end_min == 23 * 60 + 45);
    assert(wrap.OnInput(InputKind::DialUp, 0) == Action::Redraw);
    assert(wrap.View().settings.schedule.quiet_end_min == 0);
    // Every value the dial reaches keeps the whole plan valid.
    for (int i = 0; i < 200; ++i) {
        menu.OnInput(i % 3 ? InputKind::DialDown : InputKind::DialUp, 0);
        assert(ValidParentSettings(menu.View().settings));
    }
    assert(menu.OnInput(InputKind::DialLong, 0) == Action::Exit);

    // Quiet start / end cannot swallow wake-up or evening either.
    for (int row : {2, 3}) {
        menu.Open(ParentSettings{}, 0);
        for (int i = 0; i < row; ++i) menu.OnInput(InputKind::DialDown, 0);
        menu.OnInput(InputKind::DialPress, 0);
        for (int i = 0; i < 100; ++i) {
            menu.OnInput(i < 50 ? InputKind::DialUp : InputKind::DialDown, 0);
            assert(ValidParentSettings(menu.View().settings));
        }
        assert(menu.OnInput(InputKind::DialPress, 0) != Action::None);
        assert(ValidParentSettings(menu.Settings()));
    }
}

void TestMenuVolumesAndSwitches() {
    ParentMenu menu;
    menu.Open(ParentSettings{}, 0);
    for (int i = 0; i < 5; ++i) menu.OnInput(InputKind::DialDown, 0);
    assert(menu.Selected() == ParentItem::Volume);
    assert(menu.OnInput(InputKind::DialPress, 0) == Action::Redraw);
    assert(menu.OnInput(InputKind::DialUp, 0) == Action::None);  // already 100 %
    for (int pct = 90; pct >= 10; pct -= 10) {
        assert(menu.OnInput(InputKind::DialDown, 0) == Action::Redraw);
        assert(menu.View().settings.volume_cap_pct == pct);
    }
    assert(menu.OnInput(InputKind::DialDown, 0) == Action::None);  // never below 10 %
    assert(menu.OnInput(InputKind::DialUp, 0) == Action::Redraw);
    assert(menu.OnInput(InputKind::DialPress, 0) == Action::Saved);
    assert(menu.Settings().volume_cap_pct == 20);

    menu.OnInput(InputKind::DialDown, 0);
    assert(menu.Selected() == ParentItem::QuietVolume);
    menu.OnInput(InputKind::DialPress, 0);
    for (int i = 0; i < 5; ++i) menu.OnInput(InputKind::DialDown, 0);
    assert(menu.View().settings.quiet_volume_pct == 0);  // silent is allowed
    assert(menu.OnInput(InputKind::DialPress, 0) == Action::Saved);

    // Switches flip and save on one press, no editing state.
    menu.OnInput(InputKind::DialDown, 0);
    assert(menu.Selected() == ParentItem::Motion);
    assert(menu.OnInput(InputKind::DialPress, 0) == Action::Saved);
    assert(!menu.Editing() && !menu.Settings().motion_enabled);
    assert(menu.OnInput(InputKind::DialUp, 0) == Action::Redraw);
    assert(menu.OnInput(InputKind::DialUp, 0) == Action::Redraw);
    assert(menu.OnInput(InputKind::DialUp, 0) == Action::Redraw);
    assert(menu.Selected() == ParentItem::Nap);
    assert(menu.OnInput(InputKind::DialPress, 0) == Action::Saved);
    assert(menu.Settings().schedule.nap_enabled && !menu.Editing());

    // A nap that does not fit the day plan cannot be switched on.
    ParentSettings early;
    early.schedule.evening_min = 12 * 60;  // evening before the 13:00 nap
    assert(ValidParentSettings(early));
    menu.Open(early, 0);
    for (int i = 0; i < 4; ++i) menu.OnInput(InputKind::DialDown, 0);
    assert(menu.OnInput(InputKind::DialPress, 0) == Action::None);
    assert(!menu.Settings().schedule.nap_enabled && !menu.View().settings.schedule.nap_enabled);

    // Persisting failed: the old values come back.
    menu.Open(ParentSettings{}, 0);
    for (int i = 0; i < 7; ++i) menu.OnInput(InputKind::DialDown, 0);
    assert(menu.OnInput(InputKind::DialPress, 0) == Action::Saved);
    menu.Revert(ParentSettings{});
    assert(menu.IsOpen() && menu.Settings().motion_enabled && menu.View().settings.motion_enabled);
    assert(std::strcmp(ParentItemName(ParentItem::QuietVolume), "quiet_volume") == 0);
}

// Builds a v2 (V5) blob from a v3 one: drop the parent block, write
// version 2 / size 1768 and a fresh CRC.
void MakeV2(FakeNvs* disk, unsigned slot) {
    auto& b = disk->slots[slot];
    for (size_t i = Ledger::kV2Bytes - 4; i < b.size(); ++i) b[i] = 0;
    b[4] = 2;
    b[5] = 0;
    b[6] = static_cast<uint8_t>(Ledger::kV2Bytes);
    b[7] = static_cast<uint8_t>(Ledger::kV2Bytes >> 8);
    PutCrc(b.data(), Ledger::kV2Bytes);
    disk->lengths[slot] = Ledger::kV2Bytes;
}

MemoryEvent Book(uint64_t id, uint16_t book) {
    MemoryEvent e{};
    e.id = id;
    e.kind = MemoryEventKind::BookFinished;
    e.source = MemorySource::Device;
    e.option = book;
    return e;
}

void TestLedgerV3MigratesV2() {
    FakeNvs disk;
    {
        Ledger old(disk);
        assert(old.Load() == Ledger::LoadResult::Empty && old.InitializeFresh());
        PetEffects fx{};
        assert(old.ApplyDurably({123, PetAction::Feed}, &fx) == CommitResult::Committed);
        assert(old.RecordMemory(Book(124, 8)) == CommitResult::Committed);
        assert(old.Parent() == ParentSettings{});
    }
    const unsigned v2_slot = static_cast<unsigned>(ActiveSlot(disk));
    assert(disk.lengths[v2_slot] == Ledger::kBytes);  // written as v3
    MakeV2(&disk, v2_slot);
    const auto v2_bytes = disk.slots[v2_slot];

    Ledger migrated(disk);
    assert(migrated.Load() == Ledger::LoadResult::Ready);
    // Nothing from v2 is lost; the parent settings start at the defaults.
    assert(migrated.Snapshot().interaction_count == 1 && migrated.PendingCount() == 1);
    assert(migrated.Memory().RecentBookAt(0) == 8);
    assert(migrated.Parent() == ParentSettings{});
    PetEffects fx{};
    assert(migrated.ApplyDurably({123, PetAction::Feed}, &fx) == CommitResult::Duplicate);
    assert(migrated.RecordMemory(Book(124, 8)) == CommitResult::Duplicate);

    // The parent menu commits v3 into the other slot; v2 stays as rollback.
    const uint32_t interactions = migrated.Snapshot().interaction_count;
    assert(migrated.SetParentSettings(900, Custom()) == CommitResult::Committed);
    assert(migrated.Snapshot().interaction_count == interactions);  // not a pet interaction
    assert(migrated.PendingCount() == 1);                           // nothing for the server
    const unsigned v3_slot = v2_slot ? 0u : 1u;
    assert(disk.lengths[v3_slot] == Ledger::kBytes);
    assert(disk.lengths[v2_slot] == Ledger::kV2Bytes && disk.slots[v2_slot] == v2_bytes);

    Ledger after(disk);
    assert(after.Load() == Ledger::LoadResult::Ready);
    assert(after.Parent() == Custom() && after.Memory().RecentBookAt(0) == 8);
    assert(after.Snapshot().interaction_count == 1 && after.PendingCount() == 1);

    // A v2-length blob that claims v3 is corrupt.
    FakeNvs bad = disk;
    bad.present[v3_slot] = false;
    bad.slots[v2_slot][4] = 3;
    PutCrc(bad.slots[v2_slot].data(), Ledger::kV2Bytes);
    Ledger refuse(bad);
    assert(refuse.Load() == Ledger::LoadResult::Corrupt);
}

void TestLedgerParentReplayAndGuards() {
    FakeNvs disk;
    Ledger l(disk);
    assert(l.SetParentSettings(1, Custom()) == CommitResult::StorageError);  // not loaded
    assert(l.Load() == Ledger::LoadResult::Empty && l.InitializeFresh());
    assert(l.SetParentSettings(0, Custom()) == CommitResult::Rejected);
    ParentSettings bad = Custom();
    bad.volume_cap_pct = 0;
    const int writes = disk.writes;
    assert(l.SetParentSettings(7, bad) == CommitResult::Rejected && disk.writes == writes);
    assert(l.Parent() == ParentSettings{});

    assert(l.SetParentSettings(7, Custom()) == CommitResult::Committed);
    // Replayed id (e.g. a retried parent sync): Duplicate, never re-applied.
    ParentSettings other = Custom();
    other.volume_cap_pct = 90;
    assert(l.SetParentSettings(7, other) == CommitResult::Duplicate);
    assert(l.Parent() == Custom());
    // Pet and parent events share one replay window.
    PetEffects fx{};
    assert(l.ApplyDurably({7, PetAction::Feed}, &fx) == CommitResult::Duplicate);

    // A failed write keeps the old settings in memory and on disk.
    disk.write_error = true;
    assert(l.SetParentSettings(8, other) == CommitResult::StorageError);
    assert(l.Parent() == Custom());
    disk.write_error = false;
    Ledger reboot(disk);
    assert(reboot.Load() == Ledger::LoadResult::Ready && reboot.Parent() == Custom());
    // Pet commits keep the parent settings.
    assert(reboot.ApplyDurably({9, PetAction::Feed}, &fx) == CommitResult::Committed);
    Ledger again(disk);
    assert(again.Load() == Ledger::LoadResult::Ready && again.Parent() == Custom());

    // A v3 blob whose parent block is invalid (CRC fixed up) is corrupt;
    // the older slot is used instead.
    const unsigned newest = static_cast<unsigned>(ActiveSlot(disk));
    disk.slots[newest][Ledger::kV2Bytes - 4 + 18] = 0;  // cap 0 %
    PutCrc(disk.slots[newest].data(), Ledger::kBytes);
    Ledger fallback(disk);
    assert(fallback.Load() == Ledger::LoadResult::Ready && fallback.Parent() == Custom());
    assert(fallback.Snapshot().interaction_count == 0);  // the slot before feed 9
}

}  // namespace

int main() {
    static_assert(ParentSettingsCodec::kBytes == 28, "parent settings block is fixed-size");
    TestDefaultsAndValidation();
    TestVolumeLimit();
    TestCodec();
    TestMenuNavigation();
    TestCareRows();
    TestMenuIdleExit();
    TestMenuEditsTimes();
    TestMenuStopsAtInvalidTimes();
    TestMenuVolumesAndSwitches();
    TestLedgerV3MigratesV2();
    TestLedgerParentReplayAndGuards();
    std::cout << "PASS: parent settings codec, volume cap, parent menu, ledger v3 + v2 migration\n";
    return 0;
}
