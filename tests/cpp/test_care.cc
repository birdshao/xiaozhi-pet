// V8 care and growth: needs, taste, stickers, wardrobe, habits, festivals,
// diary / morning lines and the care sessions. Pure rules, no clock source:
// every time is a care minute passed in by the test.
#include "kid_app/care_calendar.h"
#include "kid_app/care_phrases.h"
#include "kid_app/care_session.h"
#include "kid_app/care_store.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

using namespace kid_app;

namespace {

uint32_t Minute(uint16_t y, uint8_t mo, uint8_t d, uint8_t h = 9, uint8_t mi = 0) {
    CivilTime t{};
    t.year = y;
    t.month = mo;
    t.day = d;
    t.hour = h;
    t.minute = mi;
    return CareMinute(t, true);
}

CareEvent Ev(uint64_t id, CareEventKind kind, MemorySource src, uint32_t minute = 0,
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

const MemorySource kAllSources[] = {MemorySource::DialChoice, MemorySource::Button,
                                    MemorySource::Device,     MemorySource::Parent,
                                    MemorySource::Voice,      MemorySource::Network};

void TestMinutesAndUnknownClock() {
    CivilTime t{};
    t.year = 2026;
    t.month = 11;
    t.day = 7;
    t.hour = 8;
    const uint32_t m = CareMinute(t, true);
    assert(m && CareDay(m) == static_cast<uint32_t>(EpochFromCivil(t) / 86400));
    assert(CareMinute(t, false) == 0);
    CivilTime unset{};  // 2000-01-01: implausible
    assert(CareMinute(unset, true) == 0);

    // Unknown clock: a content pet, nothing drifts.
    CareStore c{};
    for (DayPhase p : {DayPhase::Night, DayPhase::Evening, DayPhase::Day})
        assert(c.Needs(0, p) == NeedLevels{});
    assert(CareStore::TopNeed(c.Needs(0, DayPhase::Night)) == Need::None);
    // Never cared for yet, clock known: still content until the first care.
    assert(c.Needs(m, DayPhase::Day) == NeedLevels{});
}

void TestNeedsDriftAndCare() {
    const uint32_t m0 = Minute(2026, 11, 7, 8);
    CareStore c{};
    assert(c.Apply(Ev(1, CareEventKind::Fed, MemorySource::DialChoice, m0, 0, 1)) ==
           CareResult::Applied);
    // The first dated care starts every need clock.
    NeedLevels n = c.Needs(m0, DayPhase::Day);
    assert(n == NeedLevels{});
    // Slow drift: 4 h per hunger level, 12 h dirt, 3 h boredom, capped at 3.
    n = c.Needs(m0 + 4 * 60, DayPhase::Day);
    assert(n.hungry == 1 && n.dirty == 0 && n.bored == 1 && n.sleepy == 0);
    n = c.Needs(m0 + 8 * 60, DayPhase::Day);
    assert(n.hungry == 2 && n.bored == 2);
    uint8_t level = 0;
    assert(CareStore::TopNeed(n, &level) == Need::Hungry && level == 2);  // ties: hungry first
    n = c.Needs(m0 + 1000 * 60, DayPhase::Day);
    assert(n.hungry == 3 && n.dirty == 3 && n.bored == 3);  // capped: no worse than "asking"
    n = c.Needs(m0 + 100000 * 60, DayPhase::Day);
    assert(n.hungry == kNeedMax);
    // Clock went backwards: no drift at all.
    assert(c.Needs(m0 - 600, DayPhase::Day) == NeedLevels{});
    // Evening / night make sleepy; a lullaby keeps it away for a while.
    assert(c.Needs(m0, DayPhase::Evening).sleepy == 2);
    assert(c.Needs(m0, DayPhase::Night).sleepy == 3);
    const uint32_t m1 = m0 + 12 * 60;
    assert(c.Apply(Ev(2, CareEventKind::Lulled, MemorySource::Device, m1)) == CareResult::Applied);
    assert(c.Needs(m1 + 30, DayPhase::Evening).sleepy == 0);
    assert(c.Needs(m1 + CareStore::kRestedMin, DayPhase::Evening).sleepy == 2);
    // Each care lowers its own need.
    assert(c.Needs(m1, DayPhase::Day).dirty == 1);
    assert(c.Apply(Ev(3, CareEventKind::Bathed, MemorySource::Device, m1)) == CareResult::Applied);
    assert(c.Needs(m1, DayPhase::Day).dirty == 0);
    assert(c.Needs(m1, DayPhase::Day).hungry == 3);
    assert(c.Apply(Ev(4, CareEventKind::Fed, MemorySource::DialChoice, m1, 0, 2)) ==
           CareResult::Applied);
    assert(c.Needs(m1, DayPhase::Day).hungry == 0);
    assert(c.Needs(m1, DayPhase::Day).bored == 3);
    assert(c.Apply(Ev(5, CareEventKind::Tickled, MemorySource::Device, m1)) == CareResult::Applied);
    assert(c.Needs(m1, DayPhase::Day).bored == 0);
    // Books and games count as play.
    c.NotePlay(m1 + 9 * 60);
    assert(c.Needs(m1 + 9 * 60, DayPhase::Day).bored == 0);
    // Brushing tidies one dirt level, never cleaner than "now".
    const uint32_t m2 = m1 + 30 * 60;
    assert(c.Needs(m2, DayPhase::Day).dirty == 2);
    assert(c.Apply(Ev(6, CareEventKind::Brushed, MemorySource::Device, m2)) == CareResult::Applied);
    assert(c.Needs(m2, DayPhase::Day).dirty == 1);
    // Undated care still counts, without moving the need clocks.
    CareStore u{};
    assert(u.Apply(Ev(7, CareEventKind::Bathed, MemorySource::Device, 0)) == CareResult::Applied);
    assert(u.GetTotals().baths == 1 && u.GetToday().baths == 1 && u.GetToday().day == 0);
    assert(u.Needs(m2, DayPhase::Day) == NeedLevels{});
}

void TestGuards() {
    const uint32_t m = Minute(2026, 11, 7);
    struct Case { CareEventKind kind; uint16_t slot, option; MemorySource ok1, ok2; };
    const Case cases[] = {
        {CareEventKind::Fed, 0, 1, MemorySource::DialChoice, MemorySource::DialChoice},
        {CareEventKind::Bathed, 0, 0, MemorySource::Device, MemorySource::Device},
        {CareEventKind::Brushed, 0, 0, MemorySource::Device, MemorySource::Device},
        {CareEventKind::Tickled, 0, 0, MemorySource::Device, MemorySource::Device},
        {CareEventKind::Lulled, 0, 0, MemorySource::Device, MemorySource::Device},
        {CareEventKind::HabitSet, 0, 1, MemorySource::Parent, MemorySource::Parent},
        {CareEventKind::BirthdaySet, 6, 1, MemorySource::Parent, MemorySource::Parent},
        {CareEventKind::FestivalSeen, 0, 1, MemorySource::Device, MemorySource::Device},
        {CareEventKind::Dressed, 0, 0, MemorySource::DialChoice, MemorySource::DialChoice},
    };
    for (const auto& k : cases) {
        for (MemorySource src : kAllSources) {
            CareStore c{};
            const bool allowed = src == k.ok1 || src == k.ok2;
            const auto r = c.Apply(Ev(9, k.kind, src, m, k.slot, k.option));
            assert((r == CareResult::Applied) == allowed);
            if (!allowed) assert(c == CareStore{});  // rejected leaves it untouched
        }
    }
    // Habit ticks: the child's dial or button, never voice / network.
    for (MemorySource src : kAllSources) {
        CareStore c{};
        assert(c.Apply(Ev(1, CareEventKind::HabitSet, MemorySource::Parent, 0, 0, 2)) ==
               CareResult::Applied);
        const bool allowed = src == MemorySource::DialChoice || src == MemorySource::Button;
        assert((c.Apply(Ev(2, CareEventKind::HabitDone, src, m)) == CareResult::Applied) == allowed);
    }
    // Payload checks.
    CareStore c{};
    assert(c.Apply(Ev(0, CareEventKind::Fed, MemorySource::DialChoice, m, 0, 1)) ==
           CareResult::Rejected);  // id 0
    assert(c.Apply(Ev(1, CareEventKind::Fed, MemorySource::DialChoice, m, 0, 0)) ==
           CareResult::Rejected);
    // Foods 7 and 8 are the V9 seasonal foods (test_care_v9); 9 never is.
    assert(c.Apply(Ev(1, CareEventKind::Fed, MemorySource::DialChoice, m, 0, 9)) ==
           CareResult::Rejected);
    assert(c.Apply(Ev(1, CareEventKind::Bathed, MemorySource::Device, m, 0, 3)) ==
           CareResult::Rejected);
    assert(c.Apply(Ev(1, CareEventKind::HabitSet, MemorySource::Parent, 0, 0, 9)) ==
           CareResult::Rejected);
    assert(c.Apply(Ev(1, CareEventKind::BirthdaySet, MemorySource::Parent, 0, 2, 30)) ==
           CareResult::Rejected);
    assert(c.Apply(Ev(1, CareEventKind::BirthdaySet, MemorySource::Parent, 0, 13, 1)) ==
           CareResult::Rejected);
    assert(c.Apply(Ev(1, CareEventKind::BirthdaySet, MemorySource::Parent, 0, 0, 5)) ==
           CareResult::Rejected);
    assert(c.Apply(Ev(1, CareEventKind::BirthdaySet, MemorySource::Parent, 0, 2, 29)) ==
           CareResult::Applied);
    assert(c.BirthMonth() == 2 && c.BirthDay() == 29);
    assert(c.Apply(Ev(2, CareEventKind::BirthdaySet, MemorySource::Parent, 0, 0, 0)) ==
           CareResult::Applied);
    assert(c.BirthMonth() == 0);
    assert(c.Apply(Ev(1, CareEventKind::FestivalSeen, MemorySource::Device, 0, 0, 8)) ==
           CareResult::Rejected);
    // Locked clothes cannot be worn, whatever the source.
    assert(c.Apply(Ev(1, CareEventKind::Dressed, MemorySource::DialChoice, 0, 0, 1)) ==
           CareResult::Rejected);
    assert(c.Apply(Ev(1, CareEventKind::Dressed, MemorySource::DialChoice, 0, 5, 0)) ==
           CareResult::Rejected);
}

void TestTasteAndFavourite() {
    // Every temperament loves one food and likes one; nothing is disliked.
    for (uint8_t t = 0; t <= 2; ++t) {
        int love = 0, like = 0;
        for (uint8_t f = 1; f <= kFoodCount; ++f) {
            const Taste taste = TasteOf(static_cast<Temperament>(t), static_cast<Food>(f));
            love += taste == Taste::Love;
            like += taste == Taste::Like;
        }
        assert(love == 1 && like == 1);
    }
    assert(TasteOf(Temperament::Lively, Food::Fish) == Taste::Love);
    assert(TasteOf(Temperament::Gentle, Food::Milk) == Taste::Love);
    assert(TasteOf(Temperament::Curious, Food::Apple) == Taste::Love);
    assert(TasteOf(Temperament::Curious, Food::None) == Taste::Okay);
    CareStore c{};
    assert(c.MostFed() == Food::None);
    uint64_t id = 1;
    for (int i = 0; i < 3; ++i)
        c.Apply(Ev(id++, CareEventKind::Fed, MemorySource::DialChoice, 0, 0, 4));
    for (int i = 0; i < 3; ++i)
        c.Apply(Ev(id++, CareEventKind::Fed, MemorySource::DialChoice, 0, 0, 2));
    assert(c.MostFed() == Food::Milk);  // tie: lower food id
    c.Apply(Ev(id++, CareEventKind::Fed, MemorySource::DialChoice, 0, 0, 4));
    assert(c.MostFed() == Food::Carrot && c.FoodCount(Food::Carrot) == 4);
    // Voice never teaches taste.
    CareStore before = c;
    assert(c.Apply(Ev(id++, CareEventKind::Fed, MemorySource::Voice, 0, 0, 1)) ==
           CareResult::Rejected);
    assert(c == before);
}

void TestTodayRollAndSaturation() {
    const uint32_t d1 = Minute(2026, 11, 7, 9);
    const uint32_t d2 = Minute(2026, 11, 8, 9);
    CareStore c{};
    uint64_t id = 1;
    c.Apply(Ev(id++, CareEventKind::Fed, MemorySource::DialChoice, d1, 0, 3));
    c.Apply(Ev(id++, CareEventKind::Fed, MemorySource::DialChoice, d1, 0, 5));
    assert(c.GetToday().day == CareDay(d1) && c.GetToday().foods == 0x14 &&
           c.GetToday().last_food == 5 && c.GetToday().feeds == 2);
    c.Apply(Ev(id++, CareEventKind::Bathed, MemorySource::Device, d2));
    assert(c.GetToday().day == CareDay(d2) && c.GetToday().feeds == 0 && c.GetToday().baths == 1);
    assert(c.TodayOn(CareDay(d1)).Cares() == 0);  // a stale day is empty
    // The clock corrected backwards (it once ran ahead): today starts
    // again on the earlier date instead of freezing; totals keep counting.
    c.Apply(Ev(id++, CareEventKind::Bathed, MemorySource::Device, d1));
    assert(c.GetToday().day == CareDay(d1) && c.GetToday().baths == 1 &&
           c.GetToday().feeds == 0 && c.GetTotals().baths == 2);
    c.Apply(Ev(id++, CareEventKind::Bathed, MemorySource::Device, d2));
    assert(c.GetToday().day == CareDay(d2) && c.GetToday().baths == 1);
    // Saturation: today counters stop at 250, totals at 65535.
    for (int i = 0; i < 300; ++i)
        c.Apply(Ev(id++, CareEventKind::Tickled, MemorySource::Device, d2));
    assert(c.GetToday().tickles == CareStore::kTodayCap && c.GetTotals().tickles == 300);
    for (int i = 0; i < 70000; ++i)
        c.Apply(Ev(id++, CareEventKind::Fed, MemorySource::DialChoice, d2, 0, 1));
    assert(c.GetTotals().feeds == UINT16_MAX && c.FoodCount(Food::Fish) == UINT16_MAX);
    uint8_t buf[CareStore::kBytes];
    CareStore back{};
    assert(c.Encode(buf, sizeof(buf)) && CareStore::Decode(buf, sizeof(buf), &back) && back == c);
}

void TestHabits() {
    CareStore c{};
    uint64_t id = 1;
    const uint32_t d1 = Minute(2026, 11, 1);
    // No habit chosen: nothing to tick.
    assert(c.Apply(Ev(id++, CareEventKind::HabitDone, MemorySource::DialChoice, d1)) ==
           CareResult::Rejected);
    assert(c.Apply(Ev(id++, CareEventKind::HabitSet, MemorySource::Parent, 0, 0,
                      static_cast<uint16_t>(Habit::BrushTeeth))) == CareResult::Applied);
    // Unknown clock: no tick (streaks are days).
    assert(c.Apply(Ev(id++, CareEventKind::HabitDone, MemorySource::DialChoice, 0)) ==
           CareResult::Rejected);
    for (int d = 0; d < 3; ++d)
        assert(c.Apply(Ev(id++, CareEventKind::HabitDone, MemorySource::DialChoice,
                          d1 + d * 1440)) == CareResult::Applied);
    assert(c.GetHabit().streak == 3 && c.GetHabit().best == 3 && c.GetHabit().total == 3);
    // Twice on one day: refused ("already ticked"), nothing double counts.
    assert(c.Apply(Ev(id++, CareEventKind::HabitDone, MemorySource::Button, d1 + 2 * 1440 + 60)) ==
           CareResult::Rejected);
    assert(c.GetToday().habit_done);
    // A missed day quietly restarts the streak; best and total are kept.
    assert(c.Apply(Ev(id++, CareEventKind::HabitDone, MemorySource::Button, d1 + 5 * 1440)) ==
           CareResult::Applied);
    assert(c.GetHabit().streak == 1 && c.GetHabit().best == 3 && c.GetHabit().total == 4);
    // The clock was set ahead and then corrected back: ticks go on (a
    // quiet new streak of 1, best and total kept) instead of being refused
    // until the clock catches up again.
    assert(c.Apply(Ev(id++, CareEventKind::HabitDone, MemorySource::Button, d1 + 4 * 1440)) ==
           CareResult::Applied);
    assert(c.GetHabit().streak == 1 && c.GetHabit().best == 3 && c.GetHabit().total == 5 &&
           c.GetHabit().last_day == CareDay(d1 + 4 * 1440) && c.GetToday().habit_done);
    assert(c.Apply(Ev(id++, CareEventKind::HabitDone, MemorySource::Button, d1 + 4 * 1440 + 60)) ==
           CareResult::Rejected);  // still once a day
    // Stickers for 3 / 7 / 14 days in a row.
    MemoryStore::Growth g{};
    assert(c.EarnStickers(g) & StickerBit(Sticker::Habit3));
    for (int d = 5; d < 5 + 14; ++d)
        c.Apply(Ev(id++, CareEventKind::HabitDone, MemorySource::DialChoice, d1 + d * 1440));
    assert(c.GetHabit().streak == 15);
    const uint32_t got = c.EarnStickers(g);
    assert((got & StickerBit(Sticker::Habit7)) && (got & StickerBit(Sticker::Habit14)));
    // Same habit again: no reset; a new one starts a new track, stickers stay.
    c.Apply(Ev(id++, CareEventKind::HabitSet, MemorySource::Parent, 0, 0, 1));
    assert(c.GetHabit().streak == 15);
    c.Apply(Ev(id++, CareEventKind::HabitSet, MemorySource::Parent, 0, 0, 4));
    assert(c.GetHabit().habit == Habit::DrinkWater && c.GetHabit().streak == 0 &&
           c.GetHabit().total == 0 && !c.GetToday().habit_done);
    assert(c.Stickers() & StickerBit(Sticker::Habit14));
}

void TestStickersAndWardrobe() {
    assert(WardrobeCount() >= 8);
    // Every wardrobe item: unique (slot, item), items numbered from 1.
    for (size_t i = 0; i < WardrobeCount(); ++i) {
        const auto& w = WardrobeAt(i);
        assert(w.item >= 1 && FindWardrobe(w.slot, w.item) == &w);
        assert(CareArtValid(WardrobeArt(i)));
    }
    CareStore c{};
    MemoryStore::Growth g{};
    assert(c.EarnStickers(g) == 0);
    uint64_t id = 1;
    c.Apply(Ev(id++, CareEventKind::Fed, MemorySource::DialChoice, 0, 0, 1));
    assert(c.EarnStickers(g) == StickerBit(Sticker::FirstFeed));
    assert(c.EarnStickers(g) == 0);  // sticky, earned once
    // The cap needs the first-feed sticker.
    assert(c.Apply(Ev(id++, CareEventKind::Dressed, MemorySource::DialChoice, 0, 0, 1)) ==
           CareResult::Applied);
    assert(c.Worn(WearSlot::Head) == 1);
    assert(c.Apply(Ev(id++, CareEventKind::Dressed, MemorySource::DialChoice, 0, 0, 2)) ==
           CareResult::Rejected);  // the bow needs a bath first
    c.Apply(Ev(id++, CareEventKind::Bathed, MemorySource::Device));
    c.EarnStickers(g);
    assert(c.Apply(Ev(id++, CareEventKind::Dressed, MemorySource::DialChoice, 0, 0, 2)) ==
           CareResult::Applied);
    assert(c.Worn(WearSlot::Head) == 2);
    assert(c.Apply(Ev(id++, CareEventKind::Dressed, MemorySource::DialChoice, 0, 0, 0)) ==
           CareResult::Applied);
    assert(c.Worn(WearSlot::Head) == 0);
    // Memory milestones map to their stickers.
    g.milestones = MemoryStore::kAllMilestones;
    const uint32_t m = c.EarnStickers(g);
    for (Sticker s : {Sticker::FirstBook, Sticker::FiveBooks, Sticker::TenBooks, Sticker::FirstGame,
                      Sticker::TenGames, Sticker::FirstLike, Sticker::SevenDays})
        assert(m & StickerBit(s));
    // Festivals and birthdays: their own sticker, from the device only.
    c.Apply(Ev(id++, CareEventKind::FestivalSeen, MemorySource::Device, 0, 0,
               static_cast<uint16_t>(Festival::MidAutumn)));
    c.Apply(Ev(id++, CareEventKind::FestivalSeen, MemorySource::Device, 0, 0,
               static_cast<uint16_t>(Festival::Birthday)));
    assert((c.Stickers() & StickerBit(Sticker::Festival)) && (c.Stickers() & StickerBit(Sticker::Birthday)));
    // Care star after 20 cares; every sticker is reachable.
    for (int i = 0; i < 20; ++i) c.Apply(Ev(id++, CareEventKind::Tickled, MemorySource::Device));
    c.Apply(Ev(id++, CareEventKind::Brushed, MemorySource::Device));
    c.Apply(Ev(id++, CareEventKind::Lulled, MemorySource::Device));
    for (int i = 0; i < 10; ++i) c.Apply(Ev(id++, CareEventKind::Fed, MemorySource::DialChoice, 0, 0, 1));
    c.Apply(Ev(id++, CareEventKind::HabitSet, MemorySource::Parent, 0, 0, 1));
    const uint32_t d1 = Minute(2027, 1, 1);
    for (int d = 0; d < 14; ++d)
        c.Apply(Ev(id++, CareEventKind::HabitDone, MemorySource::Button, d1 + d * 1440));
    c.EarnStickers(g);
    assert(c.Stickers() == kAllStickers);
    for (size_t i = 0; i < WardrobeCount(); ++i) assert(WardrobeUnlocked(WardrobeAt(i), c.Stickers()));
    for (uint8_t s = 0; s < kStickerCount; ++s)
        assert(std::string(StickerName(static_cast<Sticker>(s))) != "unknown");
    // Parent wipe: back to just met; birthday and the habit preset stay.
    c.Apply(Ev(id++, CareEventKind::BirthdaySet, MemorySource::Parent, 0, 3, 8));
    c.WipeChildData();
    assert(c.Stickers() == 0 && c.Worn(WearSlot::Head) == 0 && c.GetTotals().All() == 0 &&
           c.BirthMonth() == 3 && c.BirthDay() == 8 && c.GetHabit().habit == Habit::BrushTeeth &&
           c.GetHabit().total == 0 && c.MostFed() == Food::None);
}

void TestCodecStrict() {
    CareStore c{};
    uint8_t b[CareStore::kBytes];
    assert(c.Encode(b, sizeof(b)) && b[0] == 1);
    for (size_t i = 1; i < sizeof(b); ++i) assert(b[i] == 0);
    CareStore back{};
    assert(CareStore::Decode(b, sizeof(b), &back) && back == c);
    assert(!c.Encode(b, sizeof(b) - 1) && !CareStore::Decode(b, sizeof(b) - 1, &back));
    // A full store round-trips byte for byte.
    uint64_t id = 1;
    const uint32_t m = Minute(2026, 6, 1, 10);
    c.Apply(Ev(id++, CareEventKind::HabitSet, MemorySource::Parent, 0, 0, 3));
    c.Apply(Ev(id++, CareEventKind::BirthdaySet, MemorySource::Parent, 0, 6, 1));
    c.Apply(Ev(id++, CareEventKind::Fed, MemorySource::DialChoice, m, 0, 6));
    c.Apply(Ev(id++, CareEventKind::HabitDone, MemorySource::Button, m));
    c.EarnStickers(MemoryStore::Growth{});
    c.Apply(Ev(id++, CareEventKind::Dressed, MemorySource::DialChoice, 0, 0, 1));
    assert(c.Encode(b, sizeof(b)));
    assert(CareStore::Decode(b, sizeof(b), &back) && back == c);
    // Every single-bit flip is either rejected or decodes to something that
    // re-encodes to exactly those bytes (canonical form).
    size_t rejected = 0;
    for (size_t i = 0; i < sizeof(b); ++i) {
        for (int bit = 0; bit < 8; ++bit) {
            uint8_t x[CareStore::kBytes];
            std::memcpy(x, b, sizeof(x));
            x[i] ^= static_cast<uint8_t>(1u << bit);
            CareStore d = back;
            if (!CareStore::Decode(x, sizeof(x), &d)) {
                assert(d == back);  // untouched
                ++rejected;
                continue;
            }
            uint8_t y[CareStore::kBytes];
            assert(d.Encode(y, sizeof(y)) && std::memcmp(x, y, sizeof(x)) == 0);
        }
    }
    assert(rejected > 200);
    auto bad = [&](size_t at, uint8_t v) {
        uint8_t x[CareStore::kBytes];
        std::memcpy(x, b, sizeof(x));
        x[at] = v;
        return !CareStore::Decode(x, sizeof(x), &back);
    };
    assert(bad(0, 2));              // format
    assert(bad(1, 9));              // habit
    assert(bad(3, 31));             // June 31
    assert(bad(5, 1));              // scarf worn but locked
    assert(bad(4, 9));              // no such hat
    assert(bad(43, 0x80));          // unknown sticker bit
    assert(bad(73, 0));             // last food missing from the mask
    assert(bad(79, 2));             // habit done flag
    assert(bad(90, 1));             // reserved
    assert(bad(44, 9));             // streak above best
}

void TestFestivals() {
    auto at = [](uint16_t y, uint8_t m, uint8_t d) {
        CivilTime t{};
        t.year = y;
        t.month = m;
        t.day = d;
        t.hour = 10;
        return t;
    };
    assert(FestivalOn(at(2026, 1, 1), true, 0, 0) == Festival::NewYear);
    assert(FestivalOn(at(2026, 6, 1), true, 0, 0) == Festival::ChildrensDay);
    // Lunar table (checked against the lunardate tables).
    assert(FestivalOn(at(2026, 2, 17), true, 0, 0) == Festival::SpringFestival);
    assert(FestivalOn(at(2026, 2, 19), true, 0, 0) == Festival::SpringFestival);
    assert(FestivalOn(at(2026, 2, 20), true, 0, 0) == Festival::None);
    assert(FestivalOn(at(2026, 2, 16), true, 0, 0) == Festival::None);
    assert(FestivalOn(at(2026, 3, 3), true, 0, 0) == Festival::Lantern);
    assert(FestivalOn(at(2026, 6, 19), true, 0, 0) == Festival::DragonBoat);
    assert(FestivalOn(at(2026, 9, 25), true, 0, 0) == Festival::MidAutumn);
    assert(FestivalOn(at(2025, 1, 29), true, 0, 0) == Festival::SpringFestival);
    assert(FestivalOn(at(2025, 10, 6), true, 0, 0) == Festival::MidAutumn);
    assert(FestivalOn(at(2033, 6, 1), true, 0, 0) == Festival::ChildrensDay);  // = Dragon Boat 2033
    assert(FestivalOn(at(2034, 3, 5), true, 0, 0) == Festival::Lantern);
    assert(FestivalOn(at(2035, 9, 16), true, 0, 0) == Festival::MidAutumn);
    assert(FestivalOn(at(2028, 1, 26), true, 0, 0) == Festival::SpringFestival);
    // Spring Festival across a month boundary (2028: Jan 26..28).
    assert(FestivalOn(at(2028, 1, 28), true, 0, 0) == Festival::SpringFestival);
    // Every table year has its dates in order.
    for (uint16_t y = kLunarFirstYear; y <= kLunarLastYear; ++y) {
        GregorianDate s{}, d{}, a{};
        assert(LunarFestivalDates(y, &s, &d, &a));
        assert((s.month == 1 && s.day >= 21) || (s.month == 2 && s.day <= 20));
        assert(d.month == 5 || d.month == 6);
        assert(a.month == 9 || a.month == 10);
        // 2024 is in the table but before any plausible device clock.
        if (y >= 2025) assert(FestivalOn(at(y, a.month, a.day), true, 0, 0) == Festival::MidAutumn);
    }
    // Outside the table no lunar festival is guessed; Gregorian ones still work.
    assert(!LunarFestivalDates(2036, nullptr, nullptr, nullptr));
    assert(FestivalOn(at(2036, 1, 28), true, 0, 0) == Festival::None);
    assert(FestivalOn(at(2036, 1, 1), true, 0, 0) == Festival::NewYear);
    // Birthday wins; unknown clock -> nothing.
    assert(FestivalOn(at(2026, 6, 1), true, 6, 1) == Festival::Birthday);
    assert(FestivalOn(at(2026, 3, 8), true, 3, 8) == Festival::Birthday);
    // 29 February: on the 28th in common years, on the 29th in leap years.
    assert(FestivalOn(at(2027, 2, 28), true, 2, 29) == Festival::Birthday);
    assert(FestivalOn(at(2028, 2, 28), true, 2, 29) == Festival::None);
    assert(FestivalOn(at(2028, 2, 29), true, 2, 29) == Festival::Birthday);
    assert(FestivalOn(at(2026, 1, 1), false, 0, 0) == Festival::None);
    assert(FestivalOn(CivilTime{}, true, 1, 1) == Festival::None);  // 2000: implausible
}

void TestPhrasesDiaryMorning() {
    // Every line has reviewed text; every context the code asks for resolves
    // for every temperament.
    for (size_t i = 0; i < CarePhraseCount(); ++i) {
        const auto& p = CarePhraseAt(i);
        assert(p.text && std::strlen(p.text) > 3 && FindCarePhrase(p.id) == &p);
    }
    for (uint8_t t = 0; t <= 2; ++t) {
        const Temperament tt = static_cast<Temperament>(t);
        for (CareLine l : {CareLine::DiaryOpen, CareLine::DiaryClose, CareLine::MorningHello})
            assert(FindCarePhrase(CarePhraseId(l, 0, tt))->temperament == static_cast<int8_t>(t));
        for (uint8_t f = 1; f <= kFoodCount; ++f) assert(CarePhraseId(CareLine::DiaryFood, f, tt));
        for (uint8_t h = 1; h <= kHabitCount; ++h) {
            assert(CarePhraseId(CareLine::DiaryHabit, h, tt));
            assert(CarePhraseId(CareLine::HabitRemind, h, tt));
        }
        for (uint8_t f = 1; f <= kFestivalCount; ++f) assert(CarePhraseId(CareLine::FestivalHello, f, tt));
        for (uint8_t b = 1; b <= 4; ++b) assert(CarePhraseId(CareLine::DiaryCare, b, tt));
        for (uint8_t s = 0; s <= 2; ++s) assert(CarePhraseId(CareLine::FoodReact, s, tt));
        for (uint8_t f = 1; f <= 5; ++f) assert(CarePhraseId(CareLine::MorningFeel, f, tt));
    }
    // Diary: built from the ledger only, per temperament.
    const uint32_t m = Minute(2026, 11, 7, 18);
    const uint32_t day = CareDay(m);
    CareStore care{};
    MemoryStore memory{};
    uint16_t lines[kMaxDiaryLines];
    DiaryPage empty = MakeDiaryPage(care, memory, day);
    assert(empty.Empty());
    size_t n = ComposeDiary(empty, Temperament::Gentle, lines, kMaxDiaryLines);
    assert(n == 3 && lines[0] == 1002 && lines[1] == 1051 && lines[2] == 1062);
    uint64_t id = 1;
    care.Apply(Ev(id++, CareEventKind::HabitSet, MemorySource::Parent, 0, 0, 1));
    care.Apply(Ev(id++, CareEventKind::Fed, MemorySource::DialChoice, m, 0, 1));
    care.Apply(Ev(id++, CareEventKind::Bathed, MemorySource::Device, m));
    care.Apply(Ev(id++, CareEventKind::HabitDone, MemorySource::Button, m));
    MemoryEvent book{};
    book.id = id++;
    book.kind = MemoryEventKind::BookFinished;
    book.source = MemorySource::Device;
    book.day = day;
    book.option = 7;
    assert(memory.Apply(book) == MemoryResult::Applied);
    DiaryPage page = MakeDiaryPage(care, memory, day);
    assert(page.last_food == 1 && page.books == 1 && !page.games && page.cares == 2 &&
           page.habit_done);
    n = ComposeDiary(page, Temperament::Lively, lines, kMaxDiaryLines);
    assert(n == 6 && lines[0] == 1003 && lines[1] == 1011 && lines[2] == 1021 &&
           lines[3] == 1032 && lines[4] == 1041 && lines[5] == 1063);
    // The same input always gives the same page.
    uint16_t again[kMaxDiaryLines];
    assert(ComposeDiary(page, Temperament::Lively, again, kMaxDiaryLines) == n &&
           std::memcmp(lines, again, n * sizeof(uint16_t)) == 0);
    // Next day: yesterday's diary is not today's.
    assert(MakeDiaryPage(care, memory, day + 1).Empty());
    // Output buffer limits are respected.
    assert(ComposeDiary(page, Temperament::Lively, lines, 2) == 2);
    assert(ComposeDiary(page, Temperament::Lively, nullptr, 2) == 0);
    assert(CareBucket(0) == 0 && CareBucket(3) == 3 && CareBucket(250) == 4);
    // Good morning: hello + feeling + festival + habit.
    assert(FeelOf(300, 50, true) == Feel::Hot && FeelOf(150, 50, true) == Feel::Cold &&
           FeelOf(220, 20, true) == Feel::Dry && FeelOf(220, 90, true) == Feel::Humid &&
           FeelOf(220, 50, true) == Feel::Comfy && FeelOf(220, 50, false) == Feel::Unknown);
    n = ComposeMorning(Temperament::Curious, Feel::Cold, Festival::SpringFestival,
                       Habit::BrushTeeth, lines, kMaxDiaryLines);
    assert(n == 4 && lines[0] == 1101 && lines[1] == 1112 && lines[2] == 1122 && lines[3] == 1131);
    n = ComposeMorning(Temperament::Gentle, Feel::Unknown, Festival::None, Habit::None, lines,
                       kMaxDiaryLines);
    assert(n == 1 && lines[0] == 1102);
}

CareContext Ctx(uint32_t stickers = 0) {
    CareContext c{};
    c.temperament = Temperament::Lively;
    c.stickers = stickers;
    return c;
}

void TestFeedSession() {
    CareSession s;
    CareOutput out{};
    assert(s.Start(CareKind::Feed, Ctx(), 0, &out));
    assert(out.clip == 1161 && out.redraw && !out.record);
    CareView v = s.View();
    assert(v.kind == CareKind::Feed && v.option_count == 3 && v.highlight == 0 &&
           v.options[0] == static_cast<uint16_t>(CareArt::Fish));
    out = s.Input(GameInput::Down, 100);
    assert(out.redraw && s.View().highlight == 1);
    out = s.Input(GameInput::Up, 200);
    // Pick fish: Lively loves fish -> three hearts, the "love" line.
    out = s.Input(GameInput::Press, 300);
    assert(out.record && out.event.kind == CareEventKind::Fed &&
           out.event.source == MemorySource::DialChoice && out.event.option == 1 &&
           out.clip == 1153 && !out.finished);
    assert(s.View().hearts == 3 && s.View().phase == 1);
    assert(!s.Input(GameInput::Press, 400).record);  // reacting: no double feed
    assert(!s.Tick(300 + CareSession::kReactMs - 1).finished);
    out = s.Tick(300 + CareSession::kReactMs);
    assert(out.finished && !out.sleep && !s.Active());
    // Okay food: one heart.
    s.Start(CareKind::Feed, Ctx(), 0, &out);
    for (int i = 0; i < 2; ++i) s.Input(GameInput::Down, 1);
    out = s.Input(GameInput::Press, 2);
    assert(out.event.option == 3 && out.clip == 1151 && s.View().hearts == 1);
    // Long press leaves without feeding.
    s.Start(CareKind::Feed, Ctx(), 0, &out);
    out = s.Input(GameInput::Long, 10);
    assert(out.finished && !out.record);
    // Idle 60 s: home, nothing recorded.
    s.Start(CareKind::Feed, Ctx(), 0, &out);
    assert(!s.Tick(CareSession::kIdleExitMs - 1).finished);
    out = s.Tick(CareSession::kIdleExitMs);
    assert(out.finished && !out.record);
}

void TestBathTickleNoFalseTriggers() {
    CareSession s;
    CareOutput out{};
    s.Start(CareKind::Bath, Ctx(), 0, &out);
    assert(out.clip == 1171 && s.WantsMotion());
    // Resting board: hundreds of still windows and no gesture -> no progress.
    for (uint32_t t = 500; t < 50000; t += 500) {
        out = s.OnWindow(true, t);
        assert(!out.record && !out.redraw);
        out = s.OnGesture(Gesture::None, t);
        assert(!out.record);
    }
    assert(s.View().progress == 0 && s.View().phase == 0);
    // Hard shakes: a gentle reminder, never progress.
    out = s.OnGesture(Gesture::HardShake, 50000);
    assert(out.clip == 1231 && !out.record && s.View().progress == 0);
    // Face down before the bubbles: nothing yet.
    assert(!s.OnGesture(Gesture::FaceDown, 50100).record);
    s.OnGesture(Gesture::GentleShake, 50200);
    s.OnGesture(Gesture::GentleShake, 50300);
    assert(s.View().progress == 2 && s.View().phase == 0);
    out = s.OnGesture(Gesture::GentleShake, 50400);
    assert(out.clip == 1172 && s.View().phase == 1);  // rinse
    s.OnGesture(Gesture::GentleShake, 50500);  // more shaking: still rinse
    out = s.OnGesture(Gesture::FaceDown, 50600);
    assert(out.record && out.event.kind == CareEventKind::Bathed &&
           out.event.source == MemorySource::Device && out.clip == 1173);
    out = s.Tick(50600 + CareSession::kReactMs);
    assert(out.finished);
    // The dial does not bathe.
    s.Start(CareKind::Bath, Ctx(), 0, &out);
    for (int i = 0; i < 10; ++i) assert(!s.Input(GameInput::Press, 1).record);
    // Tickle: three gentle shakes; still windows and hard shakes do nothing.
    s.Start(CareKind::Tickle, Ctx(), 0, &out);
    for (uint32_t t = 0; t < 10000; t += 500) assert(!s.OnWindow(true, t).redraw);
    s.OnGesture(Gesture::HardShake, 1);
    s.OnGesture(Gesture::Pickup, 2);
    assert(s.View().progress == 0);
    s.OnGesture(Gesture::GentleShake, 3);
    s.OnGesture(Gesture::GentleShake, 4);
    out = s.OnGesture(Gesture::GentleShake, 5);
    assert(out.record && out.event.kind == CareEventKind::Tickled && out.clip == 1192);
}

void TestBrushLullTidyDiary() {
    CareSession s;
    CareOutput out{};
    s.Start(CareKind::Brush, Ctx(), 0, &out);
    assert(!s.WantsMotion());
    // Spinning one way is one stroke, not six.
    for (int i = 0; i < 20; ++i) s.Input(GameInput::Down, 1);
    assert(s.View().progress == 1);
    for (int i = 0; i < 4; ++i) s.Input(i % 2 ? GameInput::Down : GameInput::Up, 2);
    assert(s.View().progress == 5 && s.View().goal == CareSession::kBrushStrokes);
    out = s.Input(GameInput::Up, 3);
    assert(out.record && out.event.kind == CareEventKind::Brushed && out.clip == 1182);
    // Lullaby: face down, or resting for 3 s (6 windows); moving restarts.
    s.Start(CareKind::Lull, Ctx(), 0, &out);
    for (int i = 0; i < 5; ++i) assert(!s.OnWindow(true, 500u * i).record);
    s.OnWindow(false, 3000);
    assert(s.View().progress == 0);
    for (int i = 0; i < 5; ++i) assert(!s.OnWindow(true, 3500u + 500u * i).record);
    out = s.OnWindow(true, 6000);
    assert(out.record && out.event.kind == CareEventKind::Lulled && out.clip == 1202);
    out = s.Tick(6000 + CareSession::kReactMs);
    assert(out.finished && out.sleep);  // hands over to the sleep flow
    s.Start(CareKind::Lull, Ctx(), 0, &out);
    out = s.OnGesture(Gesture::FaceDown, 10);
    assert(out.record && out.event.kind == CareEventKind::Lulled);
    // Tidy-up: three presses put the toys away, then the diary.
    s.Start(CareKind::Tidy, Ctx(), 0, &out);
    assert(out.clip == 1211);
    s.Input(GameInput::Up, 1);
    s.Input(GameInput::Press, 2);
    s.Input(GameInput::Press, 3);
    assert(s.View().progress == 2);
    out = s.Input(GameInput::Press, 4);
    assert(out.clip == 1212 && !out.record);
    out = s.Tick(4 + CareSession::kReactMs);
    assert(out.finished && out.next == CareKind::Diary && !out.sleep);
    // Diary: shows the page; a press (or 45 s) goes on to the lullaby.
    CareContext c = Ctx();
    c.diary.last_food = 2;
    c.diary.books = 1;
    c.diary.cares = 9;
    s.Start(CareKind::Diary, c, 0, &out);
    CareView v = s.View();
    assert(v.kind == CareKind::Diary && v.food == 2 && v.book && !v.game && v.cares == 4);
    assert(!s.Tick(44999).finished);
    out = s.Tick(45000);
    assert(out.finished && !out.sleep && out.next == CareKind::Lull);
    s.Start(CareKind::Diary, c, 0, &out);
    out = s.Input(GameInput::Press, 5);
    assert(out.finished && !out.sleep && out.next == CareKind::Lull);
    // A lullaby nobody finishes still ends in bed (no reward recorded).
    s.Start(CareKind::Lull, c, 0, &out);
    out = s.Tick(CareSession::kIdleExitMs);
    assert(out.finished && out.sleep && !out.record);
    // No clock: the habit card cannot tick (the ledger needs a date).
    CareContext h = Ctx();
    h.habit = Habit::ReadBook;
    h.clock_known = false;
    s.Start(CareKind::Habit, h, 0, &out);
    out = s.Input(GameInput::Press, 1);
    assert(out.finished && !out.record);
}

void TestDressHabitAlbum() {
    CareSession s;
    CareOutput out{};
    // Nothing unlocked: a lock card, a press just leaves.
    s.Start(CareKind::Dress, Ctx(0), 0, &out);
    assert(s.View().option_count == 1 && s.View().options[0] == static_cast<uint16_t>(CareArt::Lock));
    out = s.Input(GameInput::Press, 1);
    assert(out.finished && !out.record);
    // Cap and scarf unlocked.
    const uint32_t st = StickerBit(Sticker::FirstFeed) | StickerBit(Sticker::FirstBrush);
    s.Start(CareKind::Dress, Ctx(st), 0, &out);
    CareView v = s.View();
    assert(v.option_count == 2 && v.options[0] == WardrobeArt(0) && v.options[1] == WardrobeArt(4));
    out = s.Input(GameInput::Press, 2);
    assert(out.record && out.event.kind == CareEventKind::Dressed &&
           out.event.source == MemorySource::DialChoice && out.event.slot == 0 &&
           out.event.option == 1 && !out.finished && out.clip == 1222);
    assert(s.View().worn[0] == 1);
    out = s.Input(GameInput::Press, 3);  // again: take it off
    assert(out.record && out.event.option == 0 && s.View().worn[0] == 0);
    s.Input(GameInput::Down, 4);
    out = s.Input(GameInput::Press, 5);
    assert(out.event.slot == static_cast<uint16_t>(WearSlot::Neck) && out.event.option == 1);
    out = s.Input(GameInput::Long, 6);
    assert(out.finished);
    // Habit card.
    CareContext h = Ctx();
    s.Start(CareKind::Habit, h, 0, &out);  // no habit chosen
    out = s.Input(GameInput::Press, 1);
    assert(out.finished && !out.record);
    h.habit = Habit::DrinkWater;
    s.Start(CareKind::Habit, h, 0, &out);
    assert(out.clip == 1134 && s.View().options[0] == HabitArt(Habit::DrinkWater));
    // The tick is posted first; praise only follows the ledger's answer.
    out = s.Input(GameInput::Press, 2);
    assert(out.record && out.event.kind == CareEventKind::HabitDone && !out.clip &&
           s.View().hearts == 0 && s.View().phase == 0);
    assert(!s.Input(GameInput::Press, 3).record);  // waiting: no second tick
    out = s.Confirm(true, 10);
    assert(out.clip == 1141 && out.redraw && s.View().hearts == 3 && s.View().phase == 1);
    assert(s.Tick(10 + CareSession::kReactMs).finished);
    // Refused by the ledger (already ticked, e.g. on another path): no praise.
    s.Start(CareKind::Habit, h, 0, &out);
    s.Input(GameInput::Press, 2);
    out = s.Confirm(false, 20);
    assert(out.finished && out.clip == 1142 && !out.record);
    // No answer at all: home quietly after kConfirmMs.
    s.Start(CareKind::Habit, h, 0, &out);
    s.Input(GameInput::Press, 2);
    assert(!s.Tick(2 + CareSession::kConfirmMs - 1).finished);
    out = s.Tick(2 + CareSession::kConfirmMs);
    assert(out.finished && !out.clip);
    // Other sessions ignore a stray answer.
    s.Start(CareKind::Brush, Ctx(), 0, &out);
    assert(!s.Confirm(true, 1).redraw && s.Active());
    h.habit_done_today = true;
    s.Start(CareKind::Habit, h, 0, &out);
    assert(s.View().phase == 1);
    out = s.Input(GameInput::Press, 2);
    assert(out.finished && !out.record && out.clip == 1142);
    // Album: pages of six; flips clamp; any press leaves.
    s.Start(CareKind::Album, Ctx(kAllStickers), 0, &out);
    v = s.View();
    assert(v.pages == kAlbumPages && v.page == 0 && v.stickers == kAllStickers && kAlbumPages == 4);
    assert(!s.Input(GameInput::Up, 1).redraw);
    for (int i = 0; i < 10; ++i) s.Input(GameInput::Down, 2);
    assert(s.View().page == kAlbumPages - 1);
    assert(s.Input(GameInput::Press, 3).finished);
    for (uint8_t k = 1; k <= static_cast<uint8_t>(CareKind::Album); ++k)
        assert(std::string(CareKindName(static_cast<CareKind>(k))) != "none");
}

}  // namespace

int main() {
    TestMinutesAndUnknownClock();
    TestNeedsDriftAndCare();
    TestGuards();
    TestTasteAndFavourite();
    TestTodayRollAndSaturation();
    TestHabits();
    TestStickersAndWardrobe();
    TestCodecStrict();
    TestFestivals();
    TestPhrasesDiaryMorning();
    TestFeedSession();
    TestBathTickleNoFalseTriggers();
    TestBrushLullTidyDiary();
    TestDressHabitAlbum();
    std::puts("PASS: V8 care rules, guards, codec, festivals, diary and care sessions");
    return 0;
}
