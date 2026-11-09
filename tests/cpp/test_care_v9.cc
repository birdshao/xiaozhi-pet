// V9 care deepening (docs/V9_MENU_DEEPENING_PROPOSAL.md slice 1C): care
// actions that differ by day (C1), seasonal foods and today's wish (C4),
// care <-> games links (G5), TuanTuan growing up (C3) and the lullaby in
// the bedtime ritual (S4). Pure rules: dates are passed in by the test.
#include "kid_app/care_calendar.h"
#include "kid_app/care_phrases.h"
#include "kid_app/care_session.h"
#include "kid_app/care_store.h"
#include "kid_app/growth_stage.h"
#include "kid_app/play_content.h"
#include "kid_app/scene_model.h"
#include "kid_app/melody_synth.h"
#include "kid_app/song_player.h"
#include "kid_app/refresh_planner.h"
#include "kid_app/rtc_clock.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

using namespace kid_app;

namespace {

constexpr Temperament kAllT[] = {Temperament::Curious, Temperament::Gentle, Temperament::Lively};

uint32_t Day(uint16_t y, uint8_t mo, uint8_t d) {
    CivilTime t{};
    t.year = y;
    t.month = mo;
    t.day = d;
    t.hour = 9;
    return CareDay(CareMinute(t, true));
}

CareContext Ctx(Temperament t, uint32_t day, uint8_t dirty = 0) {
    CareContext c{};
    c.temperament = t;
    c.day = day;
    c.dirty = dirty;
    return c;
}

int Bits(uint32_t v) {
    int n = 0;
    for (; v; v &= v - 1) ++n;
    return n;
}

// ---- C1 -------------------------------------------------------------------

void TestVariationIsDeterministic() {
    const uint32_t d = Day(2026, 11, 8);
    for (Temperament t : kAllT) {
        assert(CareSeed(d, t) == CareSeed(d, t));
        assert(CareSeed(d, t) != 0);
    }
    // Different temperaments or days give a different seed.
    assert(CareSeed(d, Temperament::Curious) != CareSeed(d, Temperament::Gentle));
    assert(CareSeed(d, Temperament::Curious) != CareSeed(d + 1, Temperament::Curious));
    // The cheer rotates 1, 2, 3 with the day; 0 (classic) without a date.
    std::set<uint8_t> cheers;
    for (uint32_t k = 0; k < 3; ++k) cheers.insert(CheerArg(d + k));
    assert(cheers.size() == 3 && !cheers.count(0));
    assert(CheerArg(0) == 0);
}

void TestBathSpotsFollowDirt() {
    assert(BathSpots(0) == 2 && BathSpots(1) == 2 && BathSpots(2) == 3 && BathSpots(3) == 5);
    assert(BathSpots(9) == 5);
    const uint32_t d = Day(2026, 11, 8);
    std::set<uint8_t> masks;
    for (uint32_t k = 0; k < 20; ++k) {
        for (uint8_t n = 1; n <= kCareSpotSlots; ++n) {
            const uint8_t m = SpotMask(CareSeed(d + k, Temperament::Lively), n);
            assert(Bits(m) == n);  // exactly n distinct spots out of 8
        }
        masks.insert(SpotMask(CareSeed(d + k, Temperament::Lively), 3));
    }
    assert(masks.size() > 3);  // the spots really move from day to day

    // A dated bath: one gentle shake scrubs one mud spot, then the rinse.
    for (uint8_t dirty = 0; dirty <= 3; ++dirty) {
        CareSession s;
        CareOutput out{};
        assert(s.Start(CareKind::Bath, Ctx(Temperament::Gentle, d, dirty), 0, &out));
        CareView v = s.View();
        const uint8_t spots = BathSpots(dirty);
        assert(v.goal == spots && Bits(v.variant) == spots && v.progress == 0);
        for (uint8_t i = 1; i < spots; ++i) {
            out = s.OnGesture(Gesture::GentleShake, i * 100u);
            assert(out.redraw && !out.record && s.View().progress == i && s.View().phase == 0);
        }
        // Hard shakes never scrub.
        s.OnGesture(Gesture::HardShake, 900);
        assert(s.View().progress == spots - 1);
        out = s.OnGesture(Gesture::GentleShake, 1000);
        assert(s.View().phase == 1 && out.clip == 1172);  // all gone: rinse
        out = s.OnGesture(Gesture::FaceDown, 1100);
        // Same event as V8: Bathed, Device, no payload.
        assert(out.record && out.event.kind == CareEventKind::Bathed &&
               out.event.source == MemorySource::Device && !out.event.slot && !out.event.option);
        assert(out.clip == CarePhraseId(CareLine::BathDone, CheerArg(d), Temperament::Gentle));
        assert(out.clip != 1173);  // a temperament line of the day, not the shared one
    }
}

void TestClassicFlowWithoutDate() {
    // No date (clock unknown, or a self-test script): the V8 flow exactly.
    CareSession s;
    CareOutput out{};
    s.Start(CareKind::Bath, Ctx(Temperament::Lively, 0, 3), 0, &out);
    assert(s.View().goal == CareSession::kBathBubbles && s.View().variant == 0);
    for (int i = 0; i < 3; ++i) out = s.OnGesture(Gesture::GentleShake, i);
    assert(s.View().phase == 1);
    out = s.OnGesture(Gesture::FaceDown, 10);
    assert(out.record && out.clip == 1173);
    s.Start(CareKind::Brush, Ctx(Temperament::Lively, 0), 0, &out);
    assert(s.View().goal == CareSession::kBrushStrokes && s.View().variant == 0);
    s.Start(CareKind::Tickle, Ctx(Temperament::Lively, 0), 0, &out);
    assert(s.View().variant == 0);
    for (int i = 0; i < 2; ++i) s.OnGesture(Gesture::GentleShake, i);
    out = s.OnGesture(Gesture::GentleShake, 5);
    assert(out.record && out.clip == 1192);
}

void TestBrushKnots() {
    const uint32_t d = Day(2026, 11, 8);
    std::set<uint8_t> counts;
    for (uint32_t k = 0; k < 30; ++k) {
        const uint8_t n = BrushKnots(CareSeed(d + k, Temperament::Curious));
        assert(n >= 2 && n <= 4);
        counts.insert(n);
    }
    assert(counts.size() == 3);
    for (uint32_t k = 0; k < 6; ++k) {
        CareSession s;
        CareOutput out{};
        s.Start(CareKind::Brush, Ctx(Temperament::Curious, d + k), 0, &out);
        const uint8_t knots = BrushKnots(CareSeed(d + k, Temperament::Curious));
        CareView v = s.View();
        assert(Bits(v.variant) == knots && v.goal == knots * 2);
        // Spinning one way untangles nothing; back and forth = one knot.
        for (int i = 0; i < 10; ++i) s.Input(GameInput::Down, 1);
        assert(s.View().progress == 1);
        uint8_t strokes = 1;
        GameInput dir = GameInput::Up;
        while (strokes + 1 < knots * 2) {
            out = s.Input(dir, 2);
            assert(!out.record);
            dir = dir == GameInput::Up ? GameInput::Down : GameInput::Up;
            ++strokes;
        }
        out = s.Input(dir, 3);
        assert(out.record && out.event.kind == CareEventKind::Brushed &&
               out.event.source == MemorySource::Device);
        assert(out.clip == CarePhraseId(CareLine::BrushDone, CheerArg(d + k), Temperament::Curious));
    }
}

void TestTickleFeather() {
    // The three rounds visit head, belly and feet once each, order by day.
    const uint32_t d = Day(2026, 11, 8);
    std::set<uint8_t> firsts;
    for (uint32_t k = 0; k < 12; ++k) {
        const uint32_t seed = CareSeed(d + k, Temperament::Gentle);
        std::set<uint8_t> spots;
        for (uint8_t r = 0; r < 3; ++r) spots.insert(TickleTarget(seed, r));
        assert(spots.size() == 3 && *spots.rbegin() <= 2);
        firsts.insert(TickleTarget(seed, 0));
    }
    assert(firsts.size() >= 2);
    // Every temperament has one fixed most-ticklish spot.
    assert(TicklishSpot(Temperament::Curious) != TicklishSpot(Temperament::Gentle));
    for (Temperament t : kAllT) assert(TicklishSpot(t) <= 2);

    for (Temperament t : kAllT) {
        CareSession s;
        CareOutput out{};
        s.Start(CareKind::Tickle, Ctx(t, d), 0, &out);
        assert(out.clip == CarePhraseId(CareLine::TickleSeek, 0, t));
        bool laughed = false;
        uint32_t now = 10;
        for (uint8_t round = 0; round < 3; ++round) {
            CareView v = s.View();
            const uint8_t target = TickleTargetOf(v.variant), feather = TickleFeatherOf(v.variant);
            assert(target == TickleTarget(CareSeed(d, t), round));
            assert(feather != target);  // the child has to move it there
            // Dial the feather to the spot (Up = towards the head).
            uint8_t f = feather;
            while (f != target) {
                out = s.Input(f > target ? GameInput::Up : GameInput::Down, now++);
                f = static_cast<uint8_t>(f > target ? f - 1 : f + 1);
                assert(out.redraw && TickleFeatherOf(s.View().variant) == f);
            }
            out = s.OnGesture(Gesture::GentleShake, now++);
            if (target == TicklishSpot(t)) {
                laughed = true;
                if (round < 2) {
                    assert(out.clip == CarePhraseId(CareLine::TickleLaugh, 0, t));
                    assert(TickleLaughOf(s.View().variant));  // the big-laugh frame
                }
            }
            if (round < 2) {
                assert(!out.record && s.View().progress == round + 1);
            } else {
                assert(out.record && out.event.kind == CareEventKind::Tickled &&
                       out.event.source == MemorySource::Device);
                if (target == TicklishSpot(t))
                    assert(out.clip == CarePhraseId(CareLine::TickleLaugh, 0, t));
                else
                    assert(out.clip == CarePhraseId(CareLine::TickleDone, CheerArg(d), t));
            }
        }
        assert(laughed);
    }
    // A shake with the feather elsewhere wiggles it one step towards the
    // itch: shaking alone still finishes (never stuck, never punished).
    CareSession s;
    CareOutput out{};
    s.Start(CareKind::Tickle, Ctx(Temperament::Lively, d), 0, &out);
    int shakes = 0;
    while (!out.record && shakes < 20) {
        out = s.OnGesture(Gesture::GentleShake, static_cast<uint32_t>(100 + shakes));
        ++shakes;
    }
    assert(out.record && shakes >= 4 && shakes <= 9);
    // A hard shake never tickles; the dial cannot leave 0..2.
    s.Start(CareKind::Tickle, Ctx(Temperament::Lively, d), 0, &out);
    s.OnGesture(Gesture::HardShake, 1);
    assert(s.View().progress == 0);
    for (int i = 0; i < 5; ++i) s.Input(GameInput::Up, 2);
    assert(TickleFeatherOf(s.View().variant) == 0);
    for (int i = 0; i < 5; ++i) s.Input(GameInput::Down, 3);
    assert(TickleFeatherOf(s.View().variant) == 2);
}

void TestNewLinesExist() {
    // 3 actions x 3 temperaments x 3 cheers, one laugh and one "seek" line
    // per temperament: all temperament-specific, all draft-flagged.
    for (Temperament t : kAllT) {
        for (CareLine line : {CareLine::BathDone, CareLine::BrushDone, CareLine::TickleDone})
            for (uint8_t arg = 1; arg <= 3; ++arg) {
                const uint16_t id = CarePhraseId(line, arg, t);
                assert(id >= 1300 && id < 1400);
                assert(FindCarePhrase(id)->temperament == static_cast<int8_t>(t));
            }
        assert(CarePhraseId(CareLine::TickleLaugh, 0, t) >= 1300);
        assert(CarePhraseId(CareLine::TickleSeek, 0, t) >= 1300);
    }
    // V8 ids keep their meaning.
    assert(CarePhraseId(CareLine::BathDone, 0, Temperament::Lively) == 1173);
    assert(!CarePhraseDraft(1173) && CarePhraseDraft(CarePhraseId(CareLine::TickleLaugh, 0,
                                                                  Temperament::Gentle)));
}

// ---- G5 -------------------------------------------------------------------

void TestDiaryNamesTheGame() {
    size_t count = 0;
    const PlayGameInfo* games = PlayGames(&count);
    // Every V8 game and the V4 games have a "today we ..." line and a toy.
    for (size_t i = 0; i < count; ++i) {
        const uint16_t id = games[i].id;
        assert(CarePhraseId(CareLine::DiaryGameNamed, static_cast<uint8_t>(id), Temperament::Gentle));
        assert(GameToyArt(id) == FindActivity(GameChoiceOf(games[i].game))->art);
        assert(GameToyArt(id) != 0);
    }
    for (uint16_t id = 1; id <= 5; ++id) {
        assert(CarePhraseId(CareLine::DiaryGameNamed, static_cast<uint8_t>(id), Temperament::Gentle));
        assert(GameToyArt(id) != 0);
    }
    // Content packs (100+) and unknown ids: the general line, the bear.
    assert(GameToyArt(0) == 0 && GameToyArt(150) == 0 && GameToyArt(6) == 0);

    DiaryPage page{};
    page.day = 1;
    page.games = 2;
    page.last_game = 23;  // maze
    uint16_t lines[kMaxDiaryLines] = {};
    size_t n = ComposeDiary(page, Temperament::Gentle, lines, kMaxDiaryLines);
    const uint16_t maze = CarePhraseId(CareLine::DiaryGameNamed, 23, Temperament::Gentle);
    bool named = false, general = false;
    for (size_t i = 0; i < n; ++i) {
        named |= lines[i] == maze;
        general |= lines[i] == 1022;
    }
    assert(named && !general);
    page.last_game = 140;  // a pack game: the V8 line
    n = ComposeDiary(page, Temperament::Gentle, lines, kMaxDiaryLines);
    general = false;
    for (size_t i = 0; i < n; ++i) general |= lines[i] == 1022;
    assert(general);

    // MakeDiaryPage carries today's last game from the memory.
    MemoryStore m{};
    MemoryEvent e{};
    e.id = 1;
    e.kind = MemoryEventKind::GameCompleted;
    e.source = MemorySource::Device;
    e.option = 25;
    e.day = 100;
    assert(m.Apply(e) == MemoryResult::Applied);
    CareStore c{};
    DiaryPage p = MakeDiaryPage(c, m, 100);
    assert(p.games == 1 && p.last_game == 25);
    p = MakeDiaryPage(c, m, 101);  // a stale day reads empty
    assert(!p.games && !p.last_game);
    p = MakeDiaryPage(c, m, 0);  // no clock: still counted, but never named
    assert(p.games == 1 && !p.last_game);

    // A diary that also says "I grew today" waits a little longer.
    CareContext grew = Ctx(Temperament::Gentle, 100);
    grew.diary.grew = 1;
    CareSession s;
    CareOutput out{};
    s.Start(CareKind::Diary, grew, 0, &out);
    assert(!s.Tick(45000).finished);
    assert(s.Tick(53000).finished);
}

void TestTidyToyFromToday() {
    CareContext c = Ctx(Temperament::Curious, 0);
    c.toy_art = GameToyArt(24);  // shake the dice
    CareSession s;
    CareOutput out{};
    s.Start(CareKind::Tidy, c, 0, &out);
    assert(s.View().toy == GameToyArt(24));
    c.toy_art = 0;
    s.Start(CareKind::Tidy, c, 0, &out);
    assert(s.View().toy == static_cast<uint16_t>(CareArt::ToyBear));
}

void TestBoredRelief() {
    for (Temperament t : kAllT) {
        assert(BoredReliefClip(0, t) == 0 && BoredReliefClip(1, t) == 0);
        const uint16_t id = BoredReliefClip(2, t);
        assert(id && id == BoredReliefClip(3, t) && CarePhraseDraft(id));
    }
}

// ---- pictures ----

int DiffRows(const SceneModel& a, const SceneModel& b, int* top, int* bottom) {
    static MonoFrame fa, fb;
    SceneComposer::Render(a, fa);
    SceneComposer::Render(b, fb);
    int n = 0;
    *top = MonoFrame::kHeight;
    *bottom = -1;
    for (int y = 0; y < MonoFrame::kHeight; ++y)
        for (int x = 0; x < MonoFrame::kWidth; ++x) {
            if (fa.IsBlack(x, y) == fb.IsBlack(x, y)) continue;
            ++n;
            if (y < *top) *top = y;
            if (y > *bottom) *bottom = y;
        }
    return n;
}

void AssertCornersClear(const SceneModel& m) {
    static MonoFrame f;
    SceneComposer::Render(m, f);
    for (int y = 0; y < SceneComposer::kStatusBottom; ++y)
        for (int x = 0; x < MonoFrame::kWidth; ++x)
            assert(!SceneComposer::InStatusBox(x, y) || !f.IsBlack(x, y));
}

SceneModel CareModel(const CareSession& s) {
    SceneModel m{};
    m.kind = SceneKind::Care;
    m.care = s.View();
    return m;
}

// Every step of a dated bath / brush / tickle redraws only the picture
// above the card rail and keeps the status corners clear.
void TestDailyPictures() {
    int top = 0, bottom = 0;
    for (uint32_t k = 0; k < 6; ++k) {
        const uint32_t d = Day(2026, 11, 8) + k;
        for (Temperament t : kAllT) {
            CareSession s;
            CareOutput out{};
            s.Start(CareKind::Bath, Ctx(t, d, static_cast<uint8_t>(k % 4)), 0, &out);
            SceneModel prev = CareModel(s);
            AssertCornersClear(prev);
            for (uint32_t i = 0; i < 8 && !out.record; ++i) {
                out = s.OnGesture(s.View().phase ? Gesture::FaceDown : Gesture::GentleShake, i);
                const SceneModel now = CareModel(s);
                AssertCornersClear(now);
                assert(DiffRows(prev, now, &top, &bottom) > 0);  // a spot goes, every time
                assert(bottom < SceneComposer::kGameRailTop);
                prev = now;
            }
            s.Start(CareKind::Brush, Ctx(t, d), 0, &out);
            prev = CareModel(s);
            for (uint32_t i = 0; i < 10 && !out.record; ++i) {
                out = s.Input(i % 2 ? GameInput::Up : GameInput::Down, i);
                const SceneModel now = CareModel(s);
                AssertCornersClear(now);
                if (DiffRows(prev, now, &top, &bottom)) assert(bottom < SceneComposer::kGameRailTop);
                prev = now;
            }
            assert(out.record);
            s.Start(CareKind::Tickle, Ctx(t, d), 0, &out);
            prev = CareModel(s);
            for (uint32_t i = 0; i < 12 && !out.record; ++i) {
                if (i % 3 == 2) out = s.OnGesture(Gesture::GentleShake, i);
                else out = s.Input(i % 2 ? GameInput::Up : GameInput::Down, i);
                const SceneModel now = CareModel(s);
                AssertCornersClear(now);
                if (out.redraw) {
                    assert(DiffRows(prev, now, &top, &bottom) > 0);
                    assert(bottom < SceneComposer::kGameRailTop);
                }
                prev = now;
            }
        }
    }
    // Two days with different spots draw different baths.
    CareSession a, b;
    CareOutput out{};
    uint32_t d2 = Day(2026, 11, 8) + 1;
    a.Start(CareKind::Bath, Ctx(Temperament::Gentle, Day(2026, 11, 8), 3), 0, &out);
    while (b.Start(CareKind::Bath, Ctx(Temperament::Gentle, d2, 3), 0, &out),
           b.View().variant == a.View().variant)
        ++d2;
    assert(DiffRows(CareModel(a), CareModel(b), &top, &bottom) > 0);
    // The tidy-up draws today's game prop instead of the bear.
    CareContext c = Ctx(Temperament::Curious, 0);
    a.Start(CareKind::Tidy, c, 0, &out);
    c.toy_art = GameToyArt(23);
    b.Start(CareKind::Tidy, c, 0, &out);
    AssertCornersClear(CareModel(b));
    assert(DiffRows(CareModel(a), CareModel(b), &top, &bottom) > 0 &&
           bottom < SceneComposer::kGameRailTop);
}

}  // namespace

void TestV9Batch2();  // C3 / C4 / S4, below

int main() {
    TestVariationIsDeterministic();
    TestBathSpotsFollowDirt();
    TestClassicFlowWithoutDate();
    TestBrushKnots();
    TestTickleFeather();
    TestNewLinesExist();
    TestDiaryNamesTheGame();
    TestTidyToyFromToday();
    TestBoredRelief();
    TestDailyPictures();
    TestV9Batch2();
    std::puts("test_care_v9: ok");
    return 0;
}

// ---- C3 ---------------------------------------------------------------------

namespace {

MemoryEvent Mem(uint64_t id, MemoryEventKind kind, uint32_t day, uint16_t option) {
    MemoryEvent e{};
    e.id = id;
    e.kind = kind;
    e.source = MemorySource::Device;
    e.day = day;
    e.option = option;
    return e;
}

void TestGrowthStageRule() {
    // Score = active days + cares / 10 + books + games; 4 stages.
    assert(GrowthScore(0, 0, 0, 0) == 0);
    assert(GrowthScore(7, 3, 5, 25) == 7 + 2 + 3 + 5);
    assert(GrowthStageOf(0) == 0);
    assert(GrowthStageOf(kGrowthStageScore[1] - 1) == 0 && GrowthStageOf(kGrowthStageScore[1]) == 1);
    assert(GrowthStageOf(kGrowthStageScore[2]) == 2 && GrowthStageOf(kGrowthStageScore[3]) == 3);
    assert(GrowthStageOf(0xFFFFFFFFu) == kGrowthStages - 1);
    // Monotone: a bigger score never gives a smaller stage (never shrinks).
    uint8_t last = 0;
    for (uint32_t s = 0; s < 2000; ++s) {
        assert(GrowthStageOf(s) >= last);
        last = GrowthStageOf(s);
    }
    // About a week of ordinary days (1 book, 2 games, 5 cares) is stage 1,
    // about a month stage 2, about three months stage 3.
    auto days = [](uint32_t n) { return GrowthScore(n, n, 2 * n, 5 * n); };
    assert(GrowthStageOf(days(5)) == 0 && GrowthStageOf(days(7)) == 1);
    assert(GrowthStageOf(days(25)) == 1 && GrowthStageOf(days(30)) == 2);
    assert(GrowthStageOf(days(80)) == 2 && GrowthStageOf(days(90)) == 3);
}

void TestGrowthFromLedger() {
    CareStore care{};
    MemoryStore mem{};
    assert(GrowthStageNow(care, mem) == 0);
    // Play a game a day: books and games and active days add up.
    uint64_t id = 1;
    uint32_t day = 1000;
    while (GrowthStageNow(care, mem) == 0) {
        assert(mem.Apply(Mem(id++, MemoryEventKind::GameCompleted, day, 23)) ==
               MemoryResult::Applied);
        assert(mem.Apply(Mem(id++, MemoryEventKind::BookFinished, day, 7)) ==
               MemoryResult::Applied);
        ++day;
    }
    const uint32_t grew_on = day - 1;
    // The diary of that day knows; the next morning still knows; the day
    // after that, it is old news.
    assert(GrewOn(care, mem, grew_on, false) == 1);
    assert(GrewOn(care, mem, grew_on + 1, true) == 1);
    assert(GrewOn(care, mem, grew_on + 1, false) == 0);
    assert(GrewOn(care, mem, grew_on + 2, true) == 0);
    // Care counts too (ten cares = one point), and never makes it smaller.
    const uint8_t before = GrowthStageNow(care, mem);
    const uint32_t score_before = GrowthScoreNow(care, mem);
    for (int i = 0; i < 50; ++i) {
        CareEvent e{};
        e.id = id++;
        e.kind = CareEventKind::Tickled;
        e.source = MemorySource::Device;
        e.minute = day * 1440u + 600;
        assert(care.Apply(e) == CareResult::Applied);
    }
    assert(GrowthScoreNow(care, mem) == score_before + 5);  // 50 cares = 5 points
    assert(GrowthStageNow(care, mem) >= before);
    // A parent wipe starts again from the little one (like the stickers).
    care.WipeChildData();
    MemoryEvent wipe = Mem(id++, MemoryEventKind::MemoryCleared, day, 0);
    wipe.source = MemorySource::Parent;
    assert(mem.Apply(wipe) == MemoryResult::Applied);
    assert(GrowthScoreNow(care, mem) == 0 && GrowthStageNow(care, mem) == 0);
    assert(GrowthStageNow(CareStore{}, MemoryStore{}) == 0);
}

void TestGrowthLines() {
    for (Temperament t : kAllT) {
        for (uint8_t stage = 1; stage < kGrowthStages; ++stage) {
            const uint16_t morning = CarePhraseId(CareLine::GrowMorning, stage, Temperament::Gentle);
            const uint16_t diary = CarePhraseId(CareLine::GrowDiary, stage, t);
            assert(morning && diary && CarePhraseDraft(morning) && CarePhraseDraft(diary));
        }
        assert(!CarePhraseId(CareLine::GrowMorning, 0, t));
    }
    // The diary says it just before good night; the morning after the festival.
    DiaryPage page{};
    page.day = 5;
    page.books = 1;
    page.grew = 2;
    uint16_t lines[kMaxDiaryLines] = {};
    size_t n = ComposeDiary(page, Temperament::Lively, lines, kMaxDiaryLines);
    assert(n >= 3 && lines[n - 2] == CarePhraseId(CareLine::GrowDiary, 2, Temperament::Lively));
    assert(lines[n - 1] == CarePhraseId(CareLine::DiaryClose, 0, Temperament::Lively));
    n = ComposeMorning(Temperament::Lively, Feel::Comfy, Festival::None, Habit::None, lines,
                       kMaxDiaryLines, 3);
    assert(n == 3 && lines[2] == CarePhraseId(CareLine::GrowMorning, 3, Temperament::Lively));
    n = ComposeMorning(Temperament::Lively, Feel::Comfy, Festival::None, Habit::None, lines,
                       kMaxDiaryLines);
    assert(n == 2);  // V8 callers: unchanged
}

void TestGrowthPicture() {
    // The look word carries the stage; nothing else moves.
    HomeLook look{};
    look.worn = {4, 3, 2, 1, 2};
    look.need = 4;
    look.need_level = 3;
    look.festival = 7;
    for (uint8_t s = 0; s < kGrowthStages; ++s) {
        look.stage = s;
        assert(UnpackLook(PackLook(look)) == look);
    }
    // Home: each stage draws a bigger TuanTuan, only in the pet area.
    int top = 0, bottom = 0;
    SceneModel prev{};
    AssertCornersClear(prev);
    for (uint8_t s = 1; s < kGrowthStages; ++s) {
        SceneModel m{};
        m.look.stage = s;
        AssertCornersClear(m);
        assert(DiffRows(prev, m, &top, &bottom) > 0 && bottom < SceneComposer::kRailTop);
        prev = m;
        // With every room item and a festival too.
        SceneModel full = m;
        full.look.worn = {3, 1, 2, 1, 2};
        full.look.festival = 6;
        full.look.need = 1;
        full.look.need_level = 3;
        AssertCornersClear(full);
    }
    // Stage 0 is the V8 picture exactly.
    SceneModel v8{}, zero{};
    zero.look.stage = 0;
    assert(DiffRows(v8, zero, &top, &bottom) == 0);
}

// ---- S4 ---------------------------------------------------------------------

void TestBedtimeSong() {
    CareContext c = Ctx(Temperament::Gentle, Day(2026, 11, 8));
    CareSession s;
    CareOutput out{};
    // No lullaby on the card: the diary goes straight to the lull (V8).
    s.Start(CareKind::Diary, c, 0, &out);
    out = s.Input(GameInput::Press, 1);
    assert(out.finished && out.next == CareKind::Lull && !out.song);
    // With one: the song question comes first, same on the 45 s timeout.
    c.lullaby = true;
    s.Start(CareKind::Diary, c, 0, &out);
    out = s.Input(GameInput::Press, 1);
    assert(out.finished && out.next == CareKind::Bedsong);
    s.Start(CareKind::Diary, c, 0, &out);
    out = s.Tick(45000);
    assert(out.finished && out.next == CareKind::Bedsong);
    // The question: two cards, the moon with a note and TuanTuan asleep.
    assert(s.Start(CareKind::Bedsong, c, 0, &out));
    assert(out.clip == CarePhraseId(CareLine::SongAsk, 0, Temperament::Gentle) && !out.record);
    CareView v = s.View();
    assert(v.kind == CareKind::Bedsong && v.option_count == 2 && v.highlight == 0 &&
           v.options[0] == static_cast<uint16_t>(SoundArt::Lullaby) &&
           v.options[1] == static_cast<uint16_t>(PlayArt::Sleep));
    out = s.Input(GameInput::Press, 2);  // "yes": play it, then sleep
    assert(out.finished && out.song && out.next == CareKind::None && !out.record);
    assert(out.clip == 0);  // the music answers, no line over it
    // "No": the V8 lull. No answer: also the lull. Never a preference.
    s.Start(CareKind::Bedsong, c, 0, &out);
    s.Input(GameInput::Down, 3);
    assert(s.View().highlight == 1);
    out = s.Input(GameInput::Press, 4);
    assert(out.finished && !out.song && out.next == CareKind::Lull && !out.record);
    s.Start(CareKind::Bedsong, c, 0, &out);
    assert(!s.Tick(29999).finished);
    out = s.Tick(30000);
    assert(out.finished && !out.song && out.next == CareKind::Lull && !out.record);
    // Long press leaves home like every care session.
    s.Start(CareKind::Bedsong, c, 0, &out);
    out = s.Input(GameInput::Long, 5);
    assert(out.finished && !out.song && out.next == CareKind::None);
    // The picture: cards on the rail, corners clear.
    s.Start(CareKind::Bedsong, c, 0, &out);
    SceneModel m = CareModel(s);
    AssertCornersClear(m);
    SceneModel moved = m;
    moved.care.highlight = 1;
    int top = 0, bottom = 0;
    assert(DiffRows(m, moved, &top, &bottom) > 0 && top >= SceneComposer::kGameRailTop);
}

void TestBedtimeSongPlayer() {
    SongCatalog songs;
    songs.Offer("song_twinkle");
    SongContext ctx{};
    SongPlayer p;
    SongOutput out{};
    // Only nursery rhymes: nothing to play at bedtime.
    assert(!p.OpenBedtime(0, ctx, songs, &out) && !p.Active());
    songs.Offer("lullaby_moon");
    songs.Offer("lullaby_brahms");
    // The first lullaby, 10 minutes, loading at once (no list, no timer pick).
    assert(p.OpenBedtime(0, ctx, songs, &out));
    assert(p.Active() && out.load == 1 && std::string(p.Chosen().id) == "lullaby_brahms");
    out = p.Loaded(2, 10);
    assert(out.play_page == 1 && p.Bedtime() && p.View(10).timer_minutes == 10);
    // A press while it plays: silence and bed (no list at bedtime).
    out = p.Input(GameInput::Press, 20);
    assert(out.stop_audio && out.sleep_now && out.leave && !p.Active());
    // The 10 minutes run out: silence, then the sleep flow.
    assert(p.OpenBedtime(100, ctx, songs, &out));
    p.Loaded(1, 100);
    out = p.Tick(100 + 10 * 60000u);
    assert(out.stop_audio && out.sleep_now && out.leave && !p.Active());
    // An unreadable book: straight to bed as well.
    assert(p.OpenBedtime(0, ctx, songs, &out));
    out = p.Loaded(0, 5);
    assert(out.sleep_now && out.leave && !p.Active());
    assert(p.OpenBedtime(0, ctx, songs, &out));
    out = p.Tick(SongPlayer::kLoadTimeoutMs);
    assert(out.sleep_now && out.leave && !p.Active());
    // The ordinary player afterwards is the V8 one (a failed book: list).
    assert(p.Open(0, ctx, songs, &out));
    p.Input(GameInput::Press, 1);  // lullaby_brahms -> timer
    out = p.Input(GameInput::Press, 2);
    out = p.Loaded(0, 3);
    assert(!out.sleep_now && p.View(3).phase == SongPhase::List);
    // Low battery or a voice turn: refused like Open.
    SongPlayer q;
    ctx.low_battery = true;
    assert(!q.OpenBedtime(0, ctx, songs, &out) && !q.Active());
}

}  // namespace

// ---- C4 ---------------------------------------------------------------------

namespace {

CivilTime Civil(uint16_t y, uint8_t mo, uint8_t d) {
    CivilTime t{};
    t.year = y;
    t.month = mo;
    t.day = d;
    t.hour = 10;
    return t;
}

void TestSeasonWindows() {
    // Every day of the lunar table: the windows never overlap, each comes
    // once a year around its festival, and nothing outside them.
    // (2024 is in the table but before PlausibleWallClock: never "now".)
    for (uint16_t y = 2025; y <= kLunarLastYear; ++y) {
        GregorianDate spring{}, dragon{}, autumn{};
        assert(LunarFestivalDates(y, &spring, &dragon, &autumn));
        int dumpling = 0, zongzi = 0, mooncake = 0;
        for (int64_t k = 0; k < 366; ++k) {
            const CivilTime t = CivilFromEpoch(EpochFromCivil(Civil(y, 1, 1)) + k * 86400);
            if (t.year != y) break;
            const Season s = SeasonOn(t, true);
            assert(!(s.dumpling && s.treat != Treat::None));  // never two at once
            dumpling += s.dumpling;
            zongzi += s.treat == Treat::Zongzi;
            mooncake += s.treat == Treat::Mooncake;
        }
        // Dumplings: a week before the Spring Festival to the Lantern day
        // (22 days, minus what falls in the previous year: never < 15);
        // zongzi and mooncakes: a week before to two days after.
        assert(dumpling >= 15 && dumpling <= 22);
        assert(zongzi == 10 && mooncake == 10);
        // The festival days themselves are in their window.
        assert(SeasonOn(Civil(y, spring.month, spring.day), true).dumpling);
        assert(SeasonOn(Civil(y, dragon.month, dragon.day), true).treat == Treat::Zongzi);
        assert(SeasonOn(Civil(y, autumn.month, autumn.day), true).treat == Treat::Mooncake);
    }
    // An ordinary day, an unknown clock, a year outside the table: none.
    const Season none{};
    assert(SeasonOn(Civil(2026, 4, 1), true) == none);
    assert(SeasonOn(Civil(2026, 2, 17), false) == none);
    assert(SeasonOn(Civil(2040, 2, 12), true) == none);
}

CareContext FeedCtx(Temperament t, uint32_t day, Season season) {
    CareContext c = Ctx(t, day);
    c.season = season;
    c.wish = TodayWish(day, t, season);
    return c;
}

void TestSeasonalFeedCards() {
    const uint32_t d = Day(2026, 4, 1);
    CareSession s;
    CareOutput out{};
    // Out of season: the six V8 foods, nothing else.
    s.Start(CareKind::Feed, FeedCtx(Temperament::Lively, d, Season{}), 0, &out);
    std::set<uint16_t> arts;
    for (int i = 0; i < 10; ++i) {
        for (uint8_t k = 0; k < s.View().option_count; ++k) arts.insert(s.View().options[k]);
        s.Input(GameInput::Down, 1);
    }
    assert(arts.size() == kFoodCount && !arts.count(617) && !arts.count(618));
    // Spring: dumplings come first; Dragon Boat / Mid-Autumn: the treat.
    Season spring{};
    spring.dumpling = true;
    s.Start(CareKind::Feed, FeedCtx(Temperament::Lively, d, spring), 0, &out);
    assert(s.View().options[0] == FoodArt(Food::Dumpling));
    Season boat{};
    boat.treat = Treat::Zongzi;
    s.Start(CareKind::Feed, FeedCtx(Temperament::Lively, d, boat), 0, &out);
    const CareView zv = s.View();
    assert(zv.options[0] == FoodArt(Food::FestiveTreat) && zv.variant == TreatVariant(Treat::Zongzi));
    out = s.Input(GameInput::Press, 1);
    // The same record either way: Fed, dial choice, option 8.
    assert(out.record && out.event.kind == CareEventKind::Fed &&
           out.event.source == MemorySource::DialChoice && out.event.option == 8);
    Season autumn{};
    autumn.treat = Treat::Mooncake;
    s.Start(CareKind::Feed, FeedCtx(Temperament::Lively, d, autumn), 0, &out);
    const CareView mv = s.View();
    assert(mv.options[0] == zv.options[0] && mv.variant != zv.variant);
    out = s.Input(GameInput::Press, 1);
    assert(out.record && out.event.option == 8);
    // One id, two pictures: the card differs, the record does not.
    int top = 0, bottom = 0;
    SceneModel a{}, b{};
    a.kind = b.kind = SceneKind::Care;
    a.care = zv;
    b.care = mv;
    AssertCornersClear(a);
    AssertCornersClear(b);
    assert(DiffRows(a, b, &top, &bottom) > 0 && top >= SceneComposer::kGameRailTop);
}

void TestTodayWish() {
    const uint32_t d = Day(2026, 4, 1);
    std::set<uint8_t> wishes;
    for (uint32_t k = 0; k < 30; ++k)
        for (Temperament t : kAllT) {
            const Food w = TodayWish(d + k, t, Season{});
            assert(w >= Food::Fish && w <= Food::RiceBall);  // out of season: everyday food
            assert(w == TodayWish(d + k, t, Season{}));      // deterministic
            wishes.insert(static_cast<uint8_t>(w));
        }
    assert(wishes.size() >= 4);
    assert(TodayWish(0, Temperament::Gentle, Season{}) == Food::None);  // no date, no wish
    // In season the treat may be the wish.
    Season boat{};
    boat.treat = Treat::Zongzi;
    bool treat = false;
    for (uint32_t k = 0; k < 40; ++k)
        treat |= TodayWish(d + k, Temperament::Curious, boat) == Food::FestiveTreat;
    assert(treat);
    // Feeding the wish: three hearts and "how did you know!" (any taste).
    for (Temperament t : kAllT) {
        CareContext c = FeedCtx(t, d, Season{});
        CareSession s;
        CareOutput out{};
        s.Start(CareKind::Feed, c, 0, &out);
        while (FoodArt(c.wish) != s.View().options[s.View().highlight]) s.Input(GameInput::Down, 1);
        out = s.Input(GameInput::Press, 2);
        assert(out.record && out.event.option == static_cast<uint16_t>(c.wish));
        assert(s.View().hearts == 3 && out.clip == CarePhraseId(CareLine::WishHit, 0, t));
        assert(CarePhraseDraft(out.clip));
    }
}

void TestSeasonalStore() {
    // The store takes foods 7 and 8 (dial choice only), today's mask and
    // last food carry them, the "most fed" counts do not; layout unchanged.
    CareStore c{};
    CareEvent e{};
    e.id = 1;
    e.kind = CareEventKind::Fed;
    e.source = MemorySource::DialChoice;
    e.minute = Day(2026, 6, 19) * 1440u + 600;
    e.option = 8;
    assert(c.Apply(e) == CareResult::Applied);
    assert(c.GetToday().last_food == 8 && (c.GetToday().foods & 0x80) && c.GetToday().feeds == 1);
    assert(c.GetTotals().feeds == 1 && c.MostFed() == Food::None);
    for (uint8_t f = 1; f <= kFoodCount; ++f) assert(c.FoodCount(static_cast<Food>(f)) == 0);
    e.id = 2;
    e.option = 7;
    assert(c.Apply(e) == CareResult::Applied && (c.GetToday().foods & 0x40));
    e.id = 3;
    e.option = 9;
    assert(c.Apply(e) == CareResult::Rejected);
    e.option = 7;
    for (MemorySource src : {MemorySource::Voice, MemorySource::Network, MemorySource::Device}) {
        e.source = src;
        assert(c.Apply(e) == CareResult::Rejected);
    }
    // Round trip: the same 96 bytes decode back, strictly.
    uint8_t b[CareStore::kBytes];
    assert(c.Encode(b, sizeof(b)));
    CareStore back{};
    assert(CareStore::Decode(b, sizeof(b), &back) && back == c);
    // A last food beyond 8 is still refused.
    uint8_t bad[CareStore::kBytes];
    std::memcpy(bad, b, sizeof(b));
    bad[73] = 9;
    assert(!CareStore::Decode(bad, sizeof(bad), &back));
    // Diary: today's seasonal food by name (the treat by its season).
    DiaryPage p{};
    p.day = 1;
    p.last_food = 8;
    p.treat = Treat::Mooncake;
    uint16_t lines[kMaxDiaryLines] = {};
    size_t n = ComposeDiary(p, Temperament::Gentle, lines, kMaxDiaryLines);
    assert(n >= 3 && lines[1] == CarePhraseId(CareLine::DiaryFood, 9, Temperament::Gentle));
    p.treat = Treat::Zongzi;
    ComposeDiary(p, Temperament::Gentle, lines, kMaxDiaryLines);
    assert(lines[1] == CarePhraseId(CareLine::DiaryFood, 8, Temperament::Gentle));
    p.last_food = 7;
    ComposeDiary(p, Temperament::Gentle, lines, kMaxDiaryLines);
    assert(lines[1] == CarePhraseId(CareLine::DiaryFood, 7, Temperament::Gentle) && lines[1]);
}

void TestWishBubble() {
    // The hungry bubble draws today's wish; the look word keeps it, and the
    // stage bits 30..31 next to it.
    HomeLook look{};
    look.need = 1;
    look.need_level = 2;
    look.stage = 3;
    int top = 0, bottom = 0;
    SceneModel plain{};
    plain.look = look;
    for (uint8_t w = 1; w <= kWishPictures; ++w) {
        look.wish = w;
        assert(UnpackLook(PackLook(look)) == look);
        assert(((PackLook(look) >> 30) & 3u) == 3u);
        SceneModel m{};
        m.look = look;
        AssertCornersClear(m);
        // No wish draws the V8 fish; a fish wish is that same picture.
        const int d = DiffRows(plain, m, &top, &bottom);
        assert(w == 1 ? d == 0 : d > 0 && bottom < SceneComposer::kRailTop);
    }
    // Pictures: zongzi and mooncake differ for the same food id 8.
    assert(WishPicture(Food::FestiveTreat, Treat::Zongzi) !=
           WishPicture(Food::FestiveTreat, Treat::Mooncake));
    assert(WishPicture(Food::Milk, Treat::None) == 2 && WishPicture(Food::None, Treat::None) == 0);
    // A wish without the hungry bubble draws nothing.
    SceneModel calm{}, calm_wish{};
    calm_wish.look.wish = 3;
    assert(DiffRows(calm, calm_wish, &top, &bottom) == 0);
    // Out-of-range wish values are dropped, never smeared into the stage.
    HomeLook bad{};
    bad.wish = 31;
    bad.stage = 2;
    assert(UnpackLook(PackLook(bad)).wish == 0 && UnpackLook(PackLook(bad)).stage == 2);
}

void TestLookBitsAndRefresh() {
    // Every field keeps its own bits; a stage change alone is a look change
    // the refresh planner redraws (pet area partial).
    HomeLook look{};
    look.worn = {4, 3, 2, 1, 2};
    look.need = 4;
    look.need_level = 3;
    look.festival = 7;
    look.wish = 9;
    for (uint8_t s = 0; s < 4; ++s) {
        HomeLook l = look;
        l.stage = s;
        const HomeLook u = UnpackLook(PackLook(l));
        assert(u == l && u.need == 4 && u.festival == 7 && u.wish == 9);
    }
    // A look without a stage (stage defaults to 0) packs nothing into 30..31.
    HomeLook v8 = look;
    v8.stage = 0;
    assert(UnpackLook(PackLook(v8)).stage == 0 && (PackLook(v8) >> 30) == 0);
    for (size_t i = 0; i < WardrobeCount(); ++i) assert(WardrobeAt(i).item <= 7);  // 3-bit slots
    SceneModel a{}, b{};
    b.look.stage = 1;
    assert(a.look != b.look);
    RefreshPlanner planner;
    planner.Completed(a, RefreshMode::Full, true);
    assert(planner.Plan(b) == RefreshMode::Partial);
    planner.Completed(b, RefreshMode::Partial, true);
    assert(planner.PetPartials() == 1 && planner.RailPartials() == 0);
}

// V9-3A built-in melodies: with no card at all the ritual still has its
// lullaby (the board offers exactly these, AddBuiltinSongs), and a broken
// card lullaby falls back to its built-in tune instead of going to bed.
void TestBedtimeBuiltinLullaby() {
    SongCatalog builtins;
    size_t lullaby = BuiltinMelodyCount();
    for (size_t i = 0; i < BuiltinMelodyCount(); ++i) {
        builtins.OfferBuiltin(static_cast<uint8_t>(i), BuiltinMelodyAt(i).id);
        if (BuiltinMelodyAt(i).lullaby && lullaby == BuiltinMelodyCount()) lullaby = i;
    }
    assert(lullaby < BuiltinMelodyCount());  // 3A ships a built-in lullaby
    assert(builtins.HasLullaby());          // so the diary asks, card or no card
    SongContext none{};
    none.card_mounted = false;
    SongPlayer p;
    SongOutput out{};
    assert(p.OpenBedtime(0, none, builtins, &out) && out.load);
    assert(p.Chosen().builtin && p.Chosen().kind == SongKind::Lullaby);
    out = p.Loaded(1, 1);  // a built-in melody is one page, looped
    assert(out.play_page == 1 && p.Bedtime() && p.View(1).timer_minutes == 10);
    out = p.PageFinished(2);
    assert(out.play_page == 1 && !out.leave);
    out = p.Tick(1 + 10 * 60000u);
    assert(out.stop_audio && out.sleep_now && out.leave && !p.Active());
    // Nursery rhymes only (no lullaby anywhere): no question.
    SongCatalog rhymes;
    rhymes.Offer("song_twinkle");
    assert(!rhymes.HasLullaby() && !SongCatalog{}.HasLullaby());
    // The card's book of the same lullaby is broken: the melody plays.
    SongCatalog card;
    const char* id = BuiltinMelodyAt(lullaby).id;
    assert(card.Offer(id));
    card.OfferBuiltin(static_cast<uint8_t>(lullaby), id);  // remembered as its fallback
    assert(p.OpenBedtime(0, SongContext{}, card, &out) && out.load && !p.Chosen().builtin);
    out = p.Loaded(0, 5);
    assert(out.load && !out.sleep_now && p.Chosen().builtin == lullaby + 1);
    out = p.Loaded(1, 6);
    assert(out.play_page == 1 && p.Bedtime());
}

}  // namespace

void TestV9Batch2() {
    TestBedtimeBuiltinLullaby();
    TestSeasonWindows();
    TestSeasonalFeedCards();
    TestTodayWish();
    TestSeasonalStore();
    TestWishBubble();
    TestLookBitsAndRefresh();
    TestGrowthStageRule();
    TestGrowthFromLedger();
    TestGrowthLines();
    TestGrowthPicture();
    TestBedtimeSong();
    TestBedtimeSongPlayer();
}
