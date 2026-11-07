// V4-07 motion games: tilt tracker, ball and peekaboo sessions, the game
// menu, rewards only for light deliberate motion, growth memory, and the
// scene regions the motion views touch.
#include "kid_app/memory_store.h"
#include "kid_app/motion_game.h"
#include "kid_app/pet_refresh_gate.h"
#include "kid_app/refresh_planner.h"
#include "kid_app/scene_model.h"
#include "kid_app/selftest_clock.h"

#include <cassert>
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

void TestMenu() {
    GameMenu m;
    assert(!m.Active() && m.View() == GameView{});
    auto out = m.Start(1000);
    assert(out.redraw && out.clip == kGameTonePrompt && m.Active());
    GameView v = m.View();
    assert(v.option_count == 3 && v.highlight == 0 && v.motion == 0 && v.rounds == 0 &&
           v.options[0] == static_cast<uint16_t>(GameArt::Star) &&
           v.options[1] == static_cast<uint16_t>(GameArt::BallBig) &&
           v.options[2] == static_cast<uint16_t>(GameArt::Cat));
    out = m.Input(GameInput::Up, 1100);  // clamped
    assert(!out.redraw && !out.clip);
    out = m.Input(GameInput::Down, 1200);
    assert(out.redraw && out.clip == GameArtTone(static_cast<uint16_t>(GameArt::BallBig)) &&
           m.View().highlight == 1);
    // Six cards (five games and the V8 sticker album), three at a time: the
    // window slides with the highlight.
    const uint16_t kStar = static_cast<uint16_t>(GameArt::Star),
                   kBall = static_cast<uint16_t>(GameArt::BallBig),
                   kCat = static_cast<uint16_t>(GameArt::Cat),
                   kCandle = static_cast<uint16_t>(MicArt::CandleLit),
                   kHand = static_cast<uint16_t>(MicArt::Hand);
    out = m.Input(GameInput::Down, 1300);  // peekaboo: the window moves, highlight in the middle
    v = m.View();
    assert(out.redraw && v.highlight == 1 && v.options[0] == kBall && v.options[1] == kCat &&
           v.options[2] == kCandle);
    out = m.Input(GameInput::Down, 1350);  // candles
    v = m.View();
    assert(out.redraw && out.clip != 0 && v.highlight == 1 && v.options[0] == kCat &&
           v.options[1] == kCandle && v.options[2] == kHand);
    out = m.Input(GameInput::Down, 1360);  // clap: the window slides on, highlight in the middle
    v = m.View();
    assert(out.redraw && v.highlight == 1 && v.options[0] == kCandle && v.options[1] == kHand);
    // Walk to the last card (the V8 games follow): last window, highlight on the last card.
    const uint8_t cards = static_cast<uint8_t>(ActivityCount(ActivityGroup::Play));
    for (uint8_t i = 5; i < cards; ++i) out = m.Input(GameInput::Down, 1400 + i);
    v = m.View();
    assert(out.redraw && v.highlight == 2);
    out = m.Input(GameInput::Down, 1500);  // clamped
    assert(!out.redraw && m.View().highlight == 2);
    out = m.Input(GameInput::Press, 1600);
    assert(out.chosen == ActivityAt(ActivityGroup::Play, cards - 1)->id && !m.Active() &&
           !out.finished);
    // Picks map to the five games; Start can preselect (after a refusal).
    const GameChoice want[] = {GameChoice::Quiz, GameChoice::Ball, GameChoice::Peekaboo,
                               GameChoice::Candle, GameChoice::Clap};
    for (uint8_t i = 0; i < kGameChoiceCount; ++i) {
        m.Start(0, i);
        const GameView sv = m.View();
        assert(sv.highlight < 3 && sv.options[sv.highlight] != 0);
        assert(m.Input(GameInput::Press, 10).chosen == want[i]);
    }
    m.Start(0);
    assert(m.View().options[0] == kStar && m.View().highlight == 0);
    // Long press and idle go home.
    m.Start(0);
    out = m.Input(GameInput::Long, 50);
    assert(out.finished && m.End() == GameEnd::Home && !m.Active());
    m.Start(0);
    assert(!m.Tick(59999).finished);
    out = m.Tick(60000);
    assert(out.finished && m.End() == GameEnd::Idle);
    // Interrupted by the board.
    m.Start(0);
    m.Interrupt();
    assert(!m.Active() && m.End() == GameEnd::Interrupted);
    // Dead menu ignores everything.
    assert(m.Input(GameInput::Press, 1).chosen == GameChoice::None && !m.Tick(999999).finished);
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
    // Every Play row is reachable from the menu, in table order.
    GameMenu m;
    for (size_t i = 0; i < ActivityCount(ActivityGroup::Play); ++i) {
        m.Start(0, static_cast<uint8_t>(i));
        const GameView v = m.View();
        assert(v.options[v.highlight] == ActivityAt(ActivityGroup::Play, i)->art);
        assert(m.Input(GameInput::Press, 1).chosen == ActivityAt(ActivityGroup::Play, i)->id);
    }
    assert(m.Group() == ActivityGroup::Play);
    // The V8 care menu is the same menu on the Care group.
    assert(ActivityCount(ActivityGroup::Care) == 6);
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
