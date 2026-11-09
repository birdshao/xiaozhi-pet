// V4-07 motion games: tilt tracker, ball and peekaboo sessions, the game
// menu, rewards only for light deliberate motion, growth memory, and the
// scene regions the motion views touch.
#include "kid_app/memory_store.h"
#include "kid_app/motion_game.h"
#include "kid_app/pet_refresh_gate.h"
#include "kid_app/refresh_planner.h"
#include "kid_app/scene_model.h"
#include "kid_app/selftest_clock.h"

#include <array>
#include <cassert>
#include <set>
#include <string>
#include <cstdio>
#include <iostream>
#include <vector>

using namespace kid_app;

namespace {

MotionStats::Summary Window(int x, int y, int z, int dev = 20) {
    MotionStats::Summary s;
    s.samples = 25;
    s.mean_x_mg = static_cast<int16_t>(x);
    s.mean_y_mg = static_cast<int16_t>(y);
    s.mean_z_mg = static_cast<int16_t>(z);
    s.max_dev_mg = static_cast<uint16_t>(dev);
    return s;
}

MotionGameSession Started(MotionKind kind, uint32_t seed, uint32_t now = 1000) {
    MotionGameSession s;
    GameOutput out{};
    assert(s.Start(kind, seed, 3, now, &out));
    assert(out.redraw && out.clip == kGameTonePrompt && !out.finished && !out.right);
    return s;
}

// Tilts the ball to its hole: returns the output of the last tilt.
GameOutput RollToHole(MotionGameSession& s, uint32_t* now) {
    GameOutput out{};
    while (s.Ball() != s.Hole()) {
        *now += 600;
        out = s.OnGesture(s.Hole() < s.Ball() ? Gesture::TiltLeft : Gesture::TiltRight, *now);
        assert(out.redraw);
    }
    return out;
}

void TestTiltTracker() {
    TiltTracker t;
    assert(t.OnWindow(Window(0, 0, -983)) == Gesture::None);   // level, never reported first
    assert(t.OnWindow(Window(400, 0, -900)) == Gesture::TiltRight);
    assert(t.OnWindow(Window(500, 0, -850)) == Gesture::TiltRight);  // held: one per window
    assert(t.OnWindow(Window(220, 0, -950)) == Gesture::None);       // in between
    assert(t.OnWindow(Window(20, 0, -980)) == Gesture::Level);
    assert(t.OnWindow(Window(10, 0, -980)) == Gesture::None);        // Level only once
    assert(t.OnWindow(Window(-450, 0, -880)) == Gesture::TiltLeft);
    // Shaking, face down or on its side is no tilt.
    assert(t.OnWindow(Window(-450, 0, -880, 900)) == Gesture::None);
    assert(t.OnWindow(Window(450, 0, 900)) == Gesture::None);
    assert(t.OnWindow(Window(450, 0, 0)) == Gesture::None);
    assert(t.OnWindow(MotionStats::Summary{}) == Gesture::None);
    // Axis and sign are configuration.
    TiltConfig cfg;
    cfg.use_y = true;
    cfg.sign = -1;
    TiltTracker y(cfg);
    assert(y.OnWindow(Window(600, 400, -900)) == Gesture::TiltLeft);
    assert(y.OnWindow(Window(600, -400, -900)) == Gesture::TiltRight);
}

void TestBallDeterministicAndBounded() {
    // Same seed: same holes; holes are never the start cell, always within the track.
    for (uint32_t seed : {1u, 4242u, 99999u, 600000u}) {
        MotionGameSession a = Started(MotionKind::Ball, seed), b = Started(MotionKind::Ball, seed);
        uint32_t now = 1000;
        for (int round = 0; round < 3; ++round) {
            assert(a.Hole() == b.Hole());
            assert(a.Hole() != kBallStart && a.Hole() < kBallCells);
            assert(a.Ball() == kBallStart);
            RollToHole(a, &now);
            RollToHole(b, &now);
            now += MotionGameSession::kRightMs;
            a.Tick(now);
            b.Tick(now);
        }
        assert(!a.Active() && a.Result().end == GameEnd::Completed && a.Result().correct == 3);
    }
    // The ball stops at the ends and tilting there is not an event.
    for (uint32_t seed = 1; seed < 50; ++seed) {
        MotionGameSession s = Started(MotionKind::Ball, seed);
        if (s.Hole() < kBallStart) continue;  // roll away from the hole
        uint32_t now = 2000;
        for (int i = 0; i < 3; ++i) assert(s.OnGesture(Gesture::TiltLeft, now += 600).redraw);
        assert(s.Ball() == 0);
        const auto out = s.OnGesture(Gesture::TiltLeft, now += 600);
        assert(!out.redraw && !out.right && s.Ball() == 0);
        break;
    }
}

void TestBallGameFlow() {
    SelfTestClock::SetForSelfTest(100);
    MotionGameSession s = Started(MotionKind::Ball, 4242);
    uint32_t now = 1000;
    const GameView v0 = s.View();
    assert(v0.motion == 1 && v0.round == 1 && v0.rounds == 3 && v0.ball == kBallStart &&
           v0.hole == s.Hole() && v0.feedback == GameFeedback::None);
    // The wrong way, then level: no reward, the ball just moves.
    const uint8_t away = s.Hole() < kBallStart ? 1 : 0;
    auto out = s.OnGesture(away ? Gesture::TiltRight : Gesture::TiltLeft, now += 600);
    assert(out.redraw && !out.right && s.Ball() == kBallStart + (away ? 1 : -1));
    out = s.OnGesture(Gesture::Level, now += 600);
    assert(!out.redraw);
    // Shaking is never a reward or an event.
    for (Gesture g : {Gesture::HardShake, Gesture::GentleShake, Gesture::Pickup,
                      Gesture::FaceDown, Gesture::FaceUp}) {
        out = s.OnGesture(g, now += 100);
        assert(!out.redraw && !out.right && out.clip == 0);
    }
    out = RollToHole(s, &now);
    assert(out.right && out.clip == kGameClipRight);
    assert(s.View().feedback == GameFeedback::Right && s.Result().correct == 1);
    // During the reaction nothing moves and nothing is won twice.
    const uint8_t at = s.Ball();
    out = s.OnGesture(at == 0 ? Gesture::TiltRight : Gesture::TiltLeft, now += 100);
    assert(!out.redraw && !out.right && s.Ball() == at && s.Result().correct == 1);
    // Next round after kRightMs: ball back in the middle, new hole.
    out = s.Tick(now + MotionGameSession::kRightMs - 200);
    assert(!out.redraw);
    out = s.Tick(now + MotionGameSession::kRightMs);
    assert(out.redraw && out.clip == kGameTonePrompt && s.Ball() == kBallStart &&
           s.View().round == 2 && s.Result().played == 1);
}

void TestBallEnds() {
    // Long press: home, no growth.
    MotionGameSession s = Started(MotionKind::Ball, 5);
    auto out = s.Input(GameInput::Long, 5000);
    assert(out.finished && s.Result().end == GameEnd::Home && !s.Active());
    MemoryEvent e;
    assert(!GameCompletedMemoryEvent(s.Result(), 1, 100, &e));
    // The dial otherwise does nothing in a motion game.
    s = Started(MotionKind::Ball, 5);
    out = s.Input(GameInput::Press, 2000);
    assert(!out.redraw && !out.finished && s.Active());
    // Idle: question again after 12 s (once), home after 60 s.
    s = Started(MotionKind::Ball, 5, 0);
    assert(!s.Tick(11000).clip);
    assert(s.Tick(12000).clip == kGameTonePrompt);
    assert(!s.Tick(13000).clip);
    s.OnGesture(Gesture::Level, 30000);  // activity restarts the clock
    assert(!s.Tick(89000).finished);
    out = s.Tick(90000);
    assert(out.finished && out.clip == kGameClipBye && s.Result().end == GameEnd::Idle);
    // Timer wrap-around.
    s = Started(MotionKind::Ball, 5, 0xFFFFFF00u);
    assert(!s.Tick(0xFFFFFF00u + 59000).finished);
    assert(s.Tick(0xFFFFFF00u + 60000).finished);
    // Interrupted by the board.
    s = Started(MotionKind::Ball, 5);
    s.Interrupt();
    assert(!s.Active() && s.Result().end == GameEnd::Interrupted);
    // Self-test clock scales the idle timers only.
    assert(SelfTestClock::SetForSelfTest(50));
    s = Started(MotionKind::Ball, 5, 0);
    assert(s.Tick(6000).clip == kGameTonePrompt);
    assert(!s.Tick(29999).finished && s.Tick(30000).finished);
    assert(SelfTestClock::SetForSelfTest(100));
}

void TestPeekaboo() {
    MotionGameSession s = Started(MotionKind::Peekaboo, 9);
    uint32_t now = 1000;
    GameView v = s.View();
    assert(v.motion == 2 && !v.hidden && v.round == 1 && v.question_art == static_cast<uint16_t>(GameArt::Cat));
    // Face up with nothing hidden, shaking, tilting: no event.
    for (Gesture g : {Gesture::FaceUp, Gesture::HardShake, Gesture::TiltLeft, Gesture::Level}) {
        auto o = s.OnGesture(g, now += 100);
        assert(!o.redraw && !o.right);
    }
    // Hide, turn back too early: a gentle "again", no reward.
    auto out = s.OnGesture(Gesture::FaceDown, now += 500);
    assert(out.redraw && out.clip != 0 && s.View().hidden && !out.right);
    const uint32_t hid = now;
    out = s.OnGesture(Gesture::FaceUp, hid + MotionGameSession::kMinHideMs - 1);
    assert(out.redraw && !out.right && out.clip == kGameClipTryAgain &&
           s.View().feedback == GameFeedback::TryAgain && !s.View().hidden &&
           s.Result().correct == 0);
    // Hide again and keep it: found you.
    now = hid + 5000;
    out = s.OnGesture(Gesture::FaceDown, now);
    assert(s.View().hidden && s.View().feedback == GameFeedback::None);
    // Shaking while hidden changes nothing.
    out = s.OnGesture(Gesture::HardShake, now + 500);
    assert(!out.redraw && s.View().hidden);
    out = s.OnGesture(Gesture::FaceUp, now + MotionGameSession::kMinHideMs);
    assert(out.right && out.clip == kGameClipRight && s.View().feedback == GameFeedback::Right &&
           !s.View().hidden && s.Result().correct == 1);
    // Reaction: no second win; then the next round.
    now += MotionGameSession::kMinHideMs;
    out = s.OnGesture(Gesture::FaceDown, now + 100);
    assert(!out.redraw && !s.View().hidden);
    out = s.Tick(now + MotionGameSession::kRightMs);
    assert(out.redraw && s.View().round == 2 && s.View().feedback == GameFeedback::None);
    // Finish the other two rounds.
    for (int round = 2; round <= 3; ++round) {
        now += 10000;
        s.OnGesture(Gesture::FaceDown, now);
        out = s.OnGesture(Gesture::FaceUp, now + 3000);
        assert(out.right);
        out = s.Tick(now + 3000 + MotionGameSession::kRightMs);
        assert(out.finished == (round == 3));
    }
    assert(s.Result().end == GameEnd::Completed && s.Result().correct == 3 &&
           s.Result().played == 3 && s.Result().game_id == kPeekabooGameId);
    MemoryEvent e;
    assert(GameCompletedMemoryEvent(s.Result(), 7, 100, &e));
    assert(e.option == kPeekabooGameId && e.source == MemorySource::Device);
    MemoryStore store;
    assert(store.Apply(e) == MemoryResult::Applied);
}

void TestPeekabooTimers() {
    // Waiting for the hide: question again, then home when nobody plays.
    MotionGameSession s = Started(MotionKind::Peekaboo, 9, 0);
    assert(s.Tick(12000).clip == kGameTonePrompt);
    assert(s.Tick(60000).finished && s.Result().end == GameEnd::Idle);
    // Hidden: the face-down hold has its own, longer limit (not the 60 s idle exit).
    s = Started(MotionKind::Peekaboo, 9, 0);
    s.OnGesture(Gesture::FaceDown, 1000);
    assert(!s.Tick(1000 + 100000).finished && !s.Tick(1000 + 100000).redraw);
    auto out = s.Tick(1000 + MotionGameSession::kHiddenMaxMs);
    assert(out.finished && s.Result().end == GameEnd::Idle && s.Result().correct == 0);
    // Long press while hidden: home.
    s = Started(MotionKind::Peekaboo, 9);
    s.OnGesture(Gesture::FaceDown, 2000);
    assert(s.Input(GameInput::Long, 2500).finished && s.Result().end == GameEnd::Home);
}

void TestGrowthOnlyForFinishedWins() {
    // A completed ball game with wins counts; id 2.
    MotionGameSession s = Started(MotionKind::Ball, 11);
    uint32_t now = 1000;
    for (int i = 0; i < 3; ++i) {
        RollToHole(s, &now);
        now += MotionGameSession::kRightMs;
        s.Tick(now);
    }
    MemoryEvent e;
    assert(s.Result().end == GameEnd::Completed);
    assert(GameCompletedMemoryEvent(s.Result(), 3, 50, &e) && e.option == kBallGameId);
    // Interrupted mid-way: no growth, even with a win.
    s = Started(MotionKind::Ball, 11);
    RollToHole(s, &now);
    s.Interrupt();
    assert(!GameCompletedMemoryEvent(s.Result(), 3, 50, &e));
}

// V9 1A: the play menu has two levels. Level one is the categories (today's
// pick, thinking cards, moving, sound, the sticker album); level two is the
// games of one category. Long press: back one level, then home.
const uint16_t kStar = static_cast<uint16_t>(GameArt::Star);
const uint16_t kBallArt = static_cast<uint16_t>(GameArt::BallBig);
const uint16_t kCatArt = static_cast<uint16_t>(GameArt::Cat);
const uint16_t kCandleArt = static_cast<uint16_t>(MicArt::CandleLit);
const uint16_t kHandArt = static_cast<uint16_t>(MicArt::Hand);
const uint16_t kStickerArt = static_cast<uint16_t>(CareArt::Sticker);
constexpr uint16_t M(MenuArt a) { return static_cast<uint16_t>(a); }

const std::array<GameChoice, 3> kPick = {GameChoice::Shapes, GameChoice::Ball, GameChoice::Parrot};

void TestMenuTopLevel() {
    GameMenu m;
    assert(!m.Active() && m.View() == GameView{});
    // Without a pick the top level has no "today" card: four cards.
    auto out = m.Start(1000);
    assert(out.redraw && out.clip == kGameTonePrompt && m.Active());
    assert(m.Level() == MenuLevel::Top && m.Group() == ActivityGroup::Play);
    GameView v = m.View();
    assert(v.option_count == 3 && v.highlight == 0 && v.motion == 0 && v.rounds == 0);
    assert(v.options[0] == M(MenuArt::Brain) && v.options[1] == M(MenuArt::Move) &&
           v.options[2] == M(MenuArt::Sound));
    out = m.Input(GameInput::Up, 1100);  // clamped
    assert(!out.redraw && !out.clip);
    out = m.Input(GameInput::Down, 1200);
    assert(out.redraw && out.clip != 0 && m.View().highlight == 1);
    m.Input(GameInput::Down, 1210);
    out = m.Input(GameInput::Down, 1220);  // the sticker album, the window slides
    v = m.View();
    assert(out.redraw && v.highlight == 2 && v.options[0] == M(MenuArt::Move) &&
           v.options[1] == M(MenuArt::Sound) && v.options[2] == kStickerArt);
    out = m.Input(GameInput::Down, 1230);  // clamped at the end
    assert(!out.redraw);
    // The album is a leaf: press picks it straight away.
    out = m.Input(GameInput::Press, 1300);
    assert(out.chosen == GameChoice::Stickers && !m.Active() && !out.finished);

    // With a pick, "today" is the first card.
    m.SetRecommendation(kPick);
    m.Start(2000);
    v = m.View();
    assert(v.options[0] == M(MenuArt::Today) && v.options[1] == M(MenuArt::Brain) &&
           v.options[2] == M(MenuArt::Move) && v.highlight == 0);
    // Start can open on another top card (index in the top list).
    m.Start(2000, 1);
    assert(m.View().options[m.View().highlight] == M(MenuArt::Brain));
    // Long press at the top, and idle, go home.
    m.Start(0);
    out = m.Input(GameInput::Long, 50);
    assert(out.finished && m.End() == GameEnd::Home && !m.Active());
    m.Start(0);
    m.Input(GameInput::Press, 10);  // into "today"
    assert(m.Level() == MenuLevel::Games);
    assert(!m.Tick(10 + 59999).finished);
    out = m.Tick(10 + 60000);  // idle also ends from level two
    assert(out.finished && m.End() == GameEnd::Idle);
    m.Start(0);
    m.Interrupt();
    assert(!m.Active() && m.End() == GameEnd::Interrupted);
    assert(m.Input(GameInput::Press, 1).chosen == GameChoice::None && !m.Tick(999999).finished);
}

void TestMenuGamesLevel() {
    GameMenu m;
    m.SetRecommendation(kPick);
    m.Start(1000);
    // Today's pick: three games, in the order given.
    auto out = m.Input(GameInput::Press, 1100);
    assert(out.redraw && out.level_changed && out.clip != 0 && m.Active());
    assert(m.Level() == MenuLevel::Games && m.Category() == ActivityCategory::Today);
    GameView v = m.View();
    assert(v.option_count == 3 && v.highlight == 0);
    assert(v.options[0] == FindActivity(GameChoice::Shapes)->art &&
           v.options[1] == FindActivity(GameChoice::Ball)->art &&
           v.options[2] == FindActivity(GameChoice::Parrot)->art);
    // Long press: back to the top, the highlight stays on "today"; again: home.
    out = m.Input(GameInput::Long, 1200);
    assert(out.redraw && out.level_changed && !out.finished && m.Active());
    assert(m.Level() == MenuLevel::Top && m.View().highlight == 0 &&
           m.View().options[0] == M(MenuArt::Today));
    out = m.Input(GameInput::Long, 1300);
    assert(out.finished && m.End() == GameEnd::Home);
    // Pick the second pick.
    m.Start(2000);
    m.Input(GameInput::Press, 2100);
    m.Input(GameInput::Down, 2200);
    assert(m.Input(GameInput::Press, 2300).chosen == GameChoice::Ball && !m.Active());

    // The thinking cards: the quiz first, table order, a sliding window.
    m.Start(3000, 1);
    out = m.Input(GameInput::Press, 3100);
    assert(m.Category() == ActivityCategory::Brain);
    v = m.View();
    assert(v.options[0] == kStar && v.highlight == 0);
    const size_t brain = ActivityCountIn(ActivityCategory::Brain);
    assert(brain >= 10);
    for (size_t i = 1; i < brain; ++i) {
        out = m.Input(GameInput::Down, 3200 + i);
        assert(out.redraw && !out.level_changed);
        v = m.View();
        assert(v.highlight < 3 &&
               v.options[v.highlight] == ActivityInCategory(ActivityCategory::Brain, i)->art);
    }
    out = m.Input(GameInput::Down, 3999);  // clamped at the last card
    assert(!out.redraw && m.View().highlight == 2);
    assert(m.Input(GameInput::Press, 4000).chosen ==
           ActivityInCategory(ActivityCategory::Brain, brain - 1)->id);

    // Moving games: the ball first, then peekaboo.
    m.Start(5000, 2);
    m.Input(GameInput::Press, 5100);
    assert(m.Category() == ActivityCategory::Move);
    v = m.View();
    assert(v.options[0] == kBallArt && v.options[1] == kCatArt);
    m.Input(GameInput::Down, 5200);
    assert(m.Input(GameInput::Press, 5300).chosen == GameChoice::Peekaboo);
    // Sound: candles, clap, then the parrot and the songs.
    m.Start(6000, 3);
    m.Input(GameInput::Press, 6100);
    assert(m.Category() == ActivityCategory::Sound);
    v = m.View();
    assert(v.options[0] == kCandleArt && v.options[1] == kHandArt);
    // Entering a level resets the highlight to the first card.
    m.Input(GameInput::Down, 6150);
    m.Input(GameInput::Long, 6200);
    m.Input(GameInput::Press, 6300);
    assert(m.View().highlight == 0);
}

void TestMenuStartOn() {
    // After a refusal the menu opens on the game's own page, on its card.
    GameMenu m;
    m.SetRecommendation(kPick);
    auto out = m.StartOn(1000, GameChoice::Maze);
    assert(out.redraw && m.Active() && m.Level() == MenuLevel::Games &&
           m.Category() == ActivityCategory::Move);
    const GameView v = m.View();
    assert(v.options[v.highlight] == FindActivity(GameChoice::Maze)->art);
    assert(m.Input(GameInput::Press, 1100).chosen == GameChoice::Maze);
    // Long press from there: the top, highlight on the category it came from.
    m.StartOn(2000, GameChoice::Maze);
    m.Input(GameInput::Long, 2100);
    assert(m.Level() == MenuLevel::Top && m.View().options[m.View().highlight] == M(MenuArt::Move));
    // The album has no page: it opens the top level on its card.
    m.StartOn(3000, GameChoice::Stickers);
    assert(m.Level() == MenuLevel::Top && m.View().options[m.View().highlight] == kStickerArt);
    // Every Play row can be reopened on its own card.
    for (size_t i = 0; i < ActivityCount(ActivityGroup::Play); ++i) {
        const Activity* a = ActivityAt(ActivityGroup::Play, i);
        m.StartOn(0, a->id);
        const GameView sv = m.View();
        assert(sv.options[sv.highlight] == a->art);
        assert(m.Input(GameInput::Press, 1).chosen == a->id);
    }
    // Not a Play row: nothing opens.
    out = m.StartOn(0, GameChoice::Feed);
    assert(out.finished && !m.Active() && m.End() == GameEnd::Home);
    out = m.StartOn(0, GameChoice::None);
    assert(out.finished && !m.Active());
}

void TestMenuCareStaysFlat() {
    // The care menu is still one flat list of cards.
    GameMenu m;
    auto out = m.Start(0, 0, ActivityGroup::Care);
    assert(out.redraw && m.Level() == MenuLevel::Games && m.Group() == ActivityGroup::Care);
    assert(m.View().options[0] == FindActivity(GameChoice::Feed)->art);
    for (size_t i = 1; i < ActivityCount(ActivityGroup::Care); ++i)
        assert(m.Input(GameInput::Down, 10 + i).redraw);
    assert(m.Input(GameInput::Press, 99).chosen == GameChoice::HandWash);
    // Long press goes straight home (no level above).
    m.Start(0, 0, ActivityGroup::Care);
    out = m.Input(GameInput::Long, 5);
    assert(out.finished && m.End() == GameEnd::Home && !out.level_changed);
}

void TestMenu() {
    TestMenuTopLevel();
    TestMenuGamesLevel();
    TestMenuStartOn();
    TestMenuCareStaysFlat();
}

// The table's category column: every play row has one, care rows none, ids
// and order are untouched.
void TestCategories() {
    size_t brain = 0, move = 0, sound = 0, album = 0;
    for (const Activity& a : kActivities) {
        if (a.group == ActivityGroup::Care) {
            assert(a.category == ActivityCategory::None);
            continue;
        }
        assert(a.category != ActivityCategory::None && a.category != ActivityCategory::Today);
        brain += a.category == ActivityCategory::Brain;
        move += a.category == ActivityCategory::Move;
        sound += a.category == ActivityCategory::Sound;
        album += a.category == ActivityCategory::Album;
    }
    assert(brain + move + sound + album == ActivityCount(ActivityGroup::Play));
    assert(ActivityCountIn(ActivityCategory::Brain) == brain && brain == 14);
    assert(move == 7 && sound == 4 && album == 1);
    // Motion games live under "move", the album alone under "album".
    for (const Activity& a : kActivities)
        if (a.group == ActivityGroup::Play && a.needs == ActivityNeeds::Motion)
            assert(a.category == ActivityCategory::Move);
    assert(FindActivity(GameChoice::Stickers)->category == ActivityCategory::Album);
    assert(FindActivity(GameChoice::Quiz)->category == ActivityCategory::Brain);
    assert(FindActivity(GameChoice::Parrot)->category == ActivityCategory::Sound);
    assert(ActivityInCategory(ActivityCategory::Brain, 0)->id == GameChoice::Quiz);
    assert(!ActivityInCategory(ActivityCategory::Brain, brain));
    // Ids did not move.
    assert(static_cast<uint8_t>(GameChoice::Quiz) == 1 &&
           static_cast<uint8_t>(GameChoice::Songs) == 32);
    // The category pictures are valid, distinct and clear of every other range.
    std::set<uint16_t> arts;
    for (MenuArt a : {MenuArt::Today, MenuArt::Brain, MenuArt::Move, MenuArt::Sound}) {
        const uint16_t id = static_cast<uint16_t>(a);
        assert(MenuArtValid(id) && arts.insert(id).second);
        assert(id >= 200 && id <= 299);
        assert(!GameArtIsOption(id) && !MicArtValid(id) && !CareArtValid(id) &&
               !PlayArtValid(id) && !SoundArtValid(id));
    }
    assert(!MenuArtValid(0) && !MenuArtValid(205) && !MenuArtValid(200));
    assert(std::string(ActivityCategoryName(ActivityCategory::Brain)) == "brain" &&
           std::string(ActivityCategoryName(ActivityCategory::None)) == "none");
}

// V8 shared activity list: the menu is driven by the table, ids are the
// GameChoice values, names and card indexes come from the same rows.
void TestActivityList() {
    static_assert(ActivityTableValid(), "table");
    assert(ActivityCount(ActivityGroup::Play) >= kGameChoiceCount);
    for (uint8_t i = 0; i < kGameChoiceCount; ++i) {
        const Activity* a = ActivityAt(ActivityGroup::Play, i);
        assert(a && static_cast<uint8_t>(a->id) == i + 1);  // V4 order kept
        assert(ActivityIndexOf(a->id) == i && FindActivity(a->id) == a);
    }
    assert(!ActivityAt(ActivityGroup::Play, ActivityCount(ActivityGroup::Play)));
    assert(FindActivity(GameChoice::Ball)->needs == ActivityNeeds::Motion);
    assert(FindActivity(GameChoice::Clap)->needs == ActivityNeeds::Mic);
    assert(std::string(GameChoiceName(GameChoice::Quiz)) == "quiz");
    assert(std::string(GameChoiceName(GameChoice::None)) == "none");
    // Every Play row is reachable from the menu (reopened on its own card).
    GameMenu m;
    for (size_t i = 0; i < ActivityCount(ActivityGroup::Play); ++i) {
        m.StartOn(0, ActivityAt(ActivityGroup::Play, i)->id);
        const GameView v = m.View();
        assert(v.options[v.highlight] == ActivityAt(ActivityGroup::Play, i)->art);
        assert(m.Input(GameInput::Press, 1).chosen == ActivityAt(ActivityGroup::Play, i)->id);
    }
    assert(m.Group() == ActivityGroup::Play);
    // The V8 care menu is the same menu on the Care group.
    // Six care cards of session 1, then the two routines of session 3 (tooth brushing, hand washing).
    assert(ActivityCount(ActivityGroup::Care) == 8);
    assert(ActivityAt(ActivityGroup::Care, 6)->id == GameChoice::ToothBrush &&
           ActivityAt(ActivityGroup::Care, 7)->id == GameChoice::HandWash);
    const GameChoice care[] = {GameChoice::Feed, GameChoice::Bath, GameChoice::Brush,
                               GameChoice::Tickle, GameChoice::Dress, GameChoice::Habit};
    for (uint8_t i = 0; i < 6; ++i) {
        m.Start(0, i, ActivityGroup::Care);
        assert(m.Group() == ActivityGroup::Care && ActivityIndexOf(care[i]) == i);
        assert(m.Input(GameInput::Press, 1).chosen == care[i]);
    }
    assert(FindActivity(GameChoice::Bath)->needs == ActivityNeeds::Motion);
    assert(FindActivity(GameChoice::Feed)->group == ActivityGroup::Care);
}

// A group with no rows never opens: the board sees `finished` and leaves
// the game frame instead of sticking in a menu that takes no input.
void TestEmptyGroup() {
    GameMenu m;
    const auto out = m.Start(0, 0, static_cast<ActivityGroup>(7));
    assert(out.finished && !out.redraw && !m.Active() && m.End() == GameEnd::Home);
    assert(m.View() == GameView{});
    assert(!m.Input(GameInput::Press, 1).redraw && !m.Tick(999999).finished);
}

SceneModel GameScene(const GameView& view) {
    SceneModel m{};
    m.kind = SceneKind::Game;
    m.game = view;
    return m;
}

int DiffRows(const SceneModel& a, const SceneModel& b, int* top, int* bottom) {
    MonoFrame fa, fb;
    SceneComposer::Render(a, fa);
    SceneComposer::Render(b, fb);
    int count = 0;
    *top = MonoFrame::kHeight;
    *bottom = -1;
    for (int y = 0; y < MonoFrame::kHeight; ++y)
        for (int x = 0; x < MonoFrame::kWidth; ++x)
            if (fa.IsBlack(x, y) != fb.IsBlack(x, y)) {
                ++count;
                if (y < *top) *top = y;
                if (y > *bottom) *bottom = y;
            }
    return count;
}

void TestScenes() {
    const int rail = SceneComposer::kGameRailTop;
    GameView ball{};
    ball.motion = 1;
    ball.round = 1;
    ball.rounds = 3;
    ball.ball = kBallStart;
    ball.hole = 0;
    ball.question_art = static_cast<uint16_t>(GameArt::BallBig);
    int top = 0, bottom = 0;
    // Every ball and hole position draws and moving the ball touches the
    // track only (the rail budget), nothing above it.
    for (uint8_t hole : {0, 1, 5, 6}) {
        for (uint8_t at = 0; at < kBallCells; ++at) {
            GameView a = ball, b = ball;
            a.hole = b.hole = hole;
            a.ball = at;
            b.ball = static_cast<uint8_t>((at + 1) % kBallCells);
            const int n = DiffRows(GameScene(a), GameScene(b), &top, &bottom);
            assert(n > 100 && top >= rail && bottom < MonoFrame::kHeight);
            assert(a.RailDiffers(b) && !a.QuestionDiffers(b));
        }
    }
    // The ball in the hole and the reward reaction.
    GameView won = ball;
    won.ball = won.hole = 6;
    won.feedback = GameFeedback::Right;
    assert(DiffRows(GameScene(ball), GameScene(won), &top, &bottom) > 100);
    assert(ball.QuestionDiffers(won) && ball.RailDiffers(won));
    // Peekaboo: hiding changes the pet and the rail card, not other views.
    GameView peek{};
    peek.motion = 2;
    peek.round = 1;
    peek.rounds = 3;
    peek.question_art = static_cast<uint16_t>(GameArt::Cat);
    GameView hidden = peek;
    hidden.hidden = true;
    assert(DiffRows(GameScene(peek), GameScene(hidden), &top, &bottom) > 100);
    assert(top < rail && bottom >= rail);
    assert(peek.RailDiffers(hidden) && peek.QuestionDiffers(hidden));
    assert(peek != hidden && peek == peek);
    // Ball and peekaboo are different views; the quiz cards still differ from both.
    GameView quiz{};
    quiz.option_count = 3;
    quiz.question_art = static_cast<uint16_t>(GameArt::Heart);
    quiz.options = {static_cast<uint16_t>(GameArt::Star), static_cast<uint16_t>(GameArt::BallBig),
                    static_cast<uint16_t>(GameArt::Cat)};
    assert(quiz != ball && quiz != peek && ball != peek);
    // No ink in the status corner boxes (same two corners as every scene).
    for (const GameView& g : {ball, won, peek, hidden, quiz}) {
        MonoFrame f;
        SceneComposer::Render(GameScene(g), f);
        for (int y = MonoFrame::kHeight - 12; y < MonoFrame::kHeight; ++y)
            for (int x : {0, 1, 2, 3, MonoFrame::kWidth - 4, MonoFrame::kWidth - 1})
                assert(!f.IsBlack(x, y));
    }
    // Refresh planning and the gate see motion views.
    SceneModel a = GameScene(ball), b = a;
    b.game.ball = 4;
    PetRefreshGate gate(0);
    SceneModel got{};
    assert(gate.Request(a) && gate.Acquire(0, &got));
    gate.Complete(true, 0);
    assert(!gate.Request(a));  // same view: skipped
    assert(gate.Request(b));   // the ball moved: drawn
}

// The category cards draw: distinct pictures, ink only inside the card rail.
void TestMenuArtScene() {
    std::set<std::string> drawn;
    for (MenuArt art : {MenuArt::Today, MenuArt::Brain, MenuArt::Move, MenuArt::Sound}) {
        GameView g{};
        g.option_count = 3;
        g.highlight = 1;
        g.question_art = static_cast<uint16_t>(GameArt::Heart);
        g.options = {M(MenuArt::Brain), static_cast<uint16_t>(art), kStickerArt};
        MonoFrame f;
        SceneComposer::Render(GameScene(g), f);
        std::string bits;
        int ink = 0;
        for (int y = SceneComposer::kGameRailTop; y < MonoFrame::kHeight; ++y)
            for (int x = 0; x < MonoFrame::kWidth; ++x) {
                const bool b = f.IsBlack(x, y);
                ink += b;
                bits.push_back(b ? '1' : '0');
            }
        assert(ink > 500);
        drawn.insert(bits);
        for (int y = MonoFrame::kHeight - 12; y < MonoFrame::kHeight; ++y)
            for (int x : {0, 1, 2, 3, MonoFrame::kWidth - 4, MonoFrame::kWidth - 1})
                assert(!f.IsBlack(x, y));
    }
    assert(drawn.size() == 4);  // each category card looks different
    // Moving the highlight between two top cards touches the rail only.
    GameView a{}, b{};
    a.option_count = b.option_count = 3;
    a.options = b.options = {M(MenuArt::Today), M(MenuArt::Brain), M(MenuArt::Move)};
    a.question_art = b.question_art = static_cast<uint16_t>(GameArt::Heart);
    b.highlight = 1;
    int top = 0, bottom = 0;
    assert(DiffRows(GameScene(a), GameScene(b), &top, &bottom) > 0);
    assert(top >= SceneComposer::kGameRailTop);
    assert(a.RailDiffers(b) && !a.QuestionDiffers(b));
}

}  // namespace

int main() {
    TestTiltTracker();
    TestBallDeterministicAndBounded();
    TestBallGameFlow();
    TestBallEnds();
    TestPeekaboo();
    TestPeekabooTimers();
    TestGrowthOnlyForFinishedWins();
    TestMenu();
    TestActivityList();
    TestEmptyGroup();
    TestCategories();
    TestMenuArtScene();
    TestScenes();
    // The holes for the hardware script (tools/selftest/v4_motion_games.pettest).
    MotionGameSession s = Started(MotionKind::Ball, 4242);
    uint32_t now = 1000;
    std::printf("ball seed 4242 holes:");
    for (int i = 0; i < 3; ++i) {
        std::printf(" %u", s.Hole());
        RollToHole(s, &now);
        now += MotionGameSession::kRightMs;
        s.Tick(now);
    }
    std::printf("\n");
    std::cout << "motion game tests passed\n";
    return 0;
}
