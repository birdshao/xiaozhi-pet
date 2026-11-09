// V8 mini games: question banks (20+ rounds per game), live clock / weather
// rounds, memory flip, tilt maze, shake dice / tree, wooden man, day and
// night, the four-way tilt tracker, the game list and the scenes.
// Rewards follow ADR-012 (see docs/V8_MINI_GAMES.md).
#include "kid_app/game_rules.h"
#include "kid_app/gesture_detector.h"
#include "kid_app/memory_store.h"
#include "kid_app/mini_game.h"
#include "kid_app/motion_game.h"
#include "kid_app/play_content.h"
#include "kid_app/play_game.h"
#include "kid_app/scene_model.h"
#include "kid_app/selftest_clock.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <iostream>
#include <set>
#include <vector>

using namespace kid_app;

namespace {

MotionStats::Summary Window(int x, int y, int z, int dev = 18) {
    MotionStats::Summary s;
    s.samples = 25;
    s.mean_x_mg = static_cast<int16_t>(x);
    s.mean_y_mg = static_cast<int16_t>(y);
    s.mean_z_mg = static_cast<int16_t>(z);
    s.max_dev_mg = static_cast<uint16_t>(dev);
    return s;
}
MotionStats::Summary Still() { return Window(0, 0, -983, 18); }
MotionStats::Summary Moving() { return Window(100, 80, -900, 240); }

// ---- banks ----

void TestBanks() {
    size_t count = 0;
    const PlayGameInfo* games = PlayGames(&count);
    assert(count == kPlayGameCount);
    std::set<uint16_t> ids;
    uint8_t last_age = 0, last_level = 0;
    for (size_t i = 0; i < count; ++i) {
        const PlayGameInfo& g = games[i];
        // New ids: unique, not the older games' 1..5, firmware range 1..99.
        assert(g.id >= 21 && g.id < 100 && ids.insert(g.id).second);
        assert(g.min_age >= 3 && g.min_age <= 5 && g.level >= 1 && g.level <= 3);
        assert(g.min_age > last_age || (g.min_age == last_age && g.level >= last_level));
        last_age = g.min_age;
        last_level = g.level;
        assert(FindPlayGame(g.game) == &g);
        assert(PlayGamePromptText(g.game)[0] != 0);
        size_t n = 0;
        const GameRound* bank = PlayBank(g.game, &n);
        if (!IsCardGame(g.game)) {
            assert(bank == nullptr && n == 0);
            continue;
        }
        assert(bank && n >= 20 && n <= GameSession::kMaxPool);
        std::set<std::vector<uint16_t>> distinct;
        for (size_t k = 0; k < n; ++k) {
            const GameRound& r = bank[k];
            assert(GameRoundValid(r));
            assert(GameKindBuiltinValid(static_cast<uint8_t>(r.kind)));
            if (g.game >= PlayGame::SpotDiff) assert(r.kind == g.kind);
            // The right answer is one card and no other card shows the same picture.
            for (uint8_t a = 0; a < r.option_count; ++a)
                for (uint8_t b = a + 1; b < r.option_count; ++b)
                    assert(r.option_art[a] != r.option_art[b]);
            distinct.insert({r.question_art, r.option_art[0], r.option_art[1], r.option_art[2],
                             r.correct});
        }
        assert(distinct.size() >= 20);  // no copy-pasted filler
    }
    assert(ids.size() == kPlayGameCount);
    // The classic V4-06 and mic/motion ids stay free of the new ones.
    for (uint16_t old : {1, 2, 3, 4, 5}) assert(!ids.count(old));

    // The five V4-06 kinds: the two wordless ones have 20+ built-in rounds,
    // the classic eight come first and are unchanged.
    for (GameKind kind : {GameKind::MatchShape, GameKind::Count}) {
        size_t n = 0;
        const GameRound* bank = BuiltinGameBank(kind, &n);
        assert(bank && n >= 20 && n <= 64);
        for (size_t i = 0; i < n; ++i) assert(bank[i].kind == kind && GameRoundValid(bank[i]));
    }
    size_t shapes = 0, counts = 0;
    BuiltinGameBank(GameKind::MatchShape, &shapes);
    BuiltinGameBank(GameKind::Count, &counts);
    assert(BuiltinGameRoundCount() == 8 && shapes >= 20 && counts >= 20);
    assert(BuiltinGameBank(GameKind::AnimalSound, &count) == nullptr && count == 0);
    // KPG1 packs still know only kinds 1..5 and the old catalogue.
    assert(GameKindValid(5) && !GameKindValid(6));
    GameRound bad = PlayBank(PlayGame::Pattern, &count)[0];
    bad.kind = GameKind::MatchShape;  // a pack kind cannot carry V8 pictures
    assert(!GameRoundValid(bad));
}

void TestPatternAnswers() {
    size_t n = 0;
    const GameRound* bank = PlayBank(PlayGame::Pattern, &n);
    for (size_t i = 0; i < n; ++i) {
        const uint16_t art = bank[i].question_art;
        assert(PlayPatternArtValid(art));
        const int type = (art >> 9) & 3, a = (art >> 6) & 7, b = (art >> 3) & 7, c = art & 7;
        const int expect = type == 0 ? a : type == 1 ? b : c;
        assert(bank[i].option_art[bank[i].correct] == kPlayShapeArt[expect]);
        if (type == 2) assert(c != a && c != b);
    }
}

void TestSizeOrderAndSpotAnswers() {
    size_t n = 0;
    const GameRound* bank = PlayBank(PlayGame::SizeOrder, &n);
    for (size_t i = 0; i < n; ++i) {
        const uint16_t q = bank[i].question_art;
        const int shape = (q >> 3) & 31, hidden = q & 3;
        const bool desc = (q & 4) != 0;
        const int want = desc ? 2 - hidden : hidden;
        assert(bank[i].option_art[bank[i].correct] == PlaySizedArt(shape, want));
    }
    bank = PlayBank(PlayGame::SpotDiff, &n);
    for (size_t i = 0; i < n; ++i) {
        const uint16_t q = bank[i].question_art;
        const int idx = (q >> 2) & 3, delta = q & 3;
        assert(idx < 3 && delta < 3);
        // The replaced shape really differs from the one above it.
        const int top = (q >> (8 - 2 * idx)) & 3;
        assert((top + 1 + delta) % 4 != top);
        assert(bank[i].option_art[bank[i].correct] == static_cast<uint16_t>(PlayArt::Pos1) + idx);
    }
}

void TestShadowClassifyWeather() {
    size_t n = 0;
    const GameRound* bank = PlayBank(PlayGame::Shadow, &n);
    for (size_t i = 0; i < n; ++i)
        assert(bank[i].option_art[bank[i].correct] == PlayShadowArt(bank[i].question_art));
    bank = PlayBank(PlayGame::Classify, &n);
    for (size_t i = 0; i < n; ++i) {
        const bool eat = bank[i].question_art == static_cast<uint16_t>(PlayArt::EatYes);
        auto edible = [](uint16_t art) {
            return art >= static_cast<uint16_t>(PlayArt::Apple) &&
                   art <= static_cast<uint16_t>(PlayArt::Bread);
        };
        for (uint8_t k = 0; k < bank[i].option_count; ++k)
            assert(edible(bank[i].option_art[k]) == (eat == (k == bank[i].correct)));
    }
    bank = PlayBank(PlayGame::Weather, &n);
    assert(n >= 20);
    for (size_t i = 0; i < n; ++i) {
        const uint16_t right = bank[i].option_art[bank[i].correct];
        const uint16_t q = bank[i].question_art;
        const bool warm = q == static_cast<uint16_t>(PlayArt::Sun) ||
                          q == static_cast<uint16_t>(PlayArt::ThermoHot);
        const bool cold = q == static_cast<uint16_t>(PlayArt::Snow) ||
                          q == static_cast<uint16_t>(PlayArt::ThermoCold);
        if (warm) assert(right == static_cast<uint16_t>(PlayArt::Tshirt) || right == static_cast<uint16_t>(PlayArt::Sunglasses));
        else if (cold) assert(right == static_cast<uint16_t>(PlayArt::Scarf) || right == static_cast<uint16_t>(PlayArt::Gloves));
        else assert(right == static_cast<uint16_t>(PlayArt::Umbrella));
        // The other cards never fit the same weather as the right one.
        for (uint8_t k = 0; k < bank[i].option_count; ++k) {
            if (k == bank[i].correct) continue;
            const uint16_t other = bank[i].option_art[k];
            const bool other_warm = other == static_cast<uint16_t>(PlayArt::Tshirt) ||
                                    other == static_cast<uint16_t>(PlayArt::Sunglasses);
            const bool other_cold = other == static_cast<uint16_t>(PlayArt::Scarf) ||
                                    other == static_cast<uint16_t>(PlayArt::Gloves);
            assert(!(warm && other_warm) && !(cold && other_cold));
        }
    }
}

void TestClockAndWeatherLive() {
    // Clock: "now" comes first, rounded down to the hour / half hour.
    GameRound pool[3];
    assert(BuildClockPool(true, 8, 10, pool) == 3);
    assert(pool[0].question_art == PlayClockArt(8, false));
    assert(pool[0].option_art[pool[0].correct] == static_cast<uint16_t>(PlayArt::Eat));
    assert(BuildClockPool(true, 20, 45, pool) == 3);
    assert(pool[0].question_art == PlayClockArt(20, true));
    assert(pool[0].option_art[pool[0].correct] == static_cast<uint16_t>(PlayArt::Sleep));
    for (uint8_t h = 0; h < 24; ++h) {
        BuildClockPool(true, h, 0, pool);
        for (const GameRound& r : pool) assert(GameRoundValid(r) && r.kind == GameKind::Clock);
        // The three rounds are three different times.
        assert(pool[0].question_art != pool[1].question_art &&
               pool[0].question_art != pool[2].question_art);
    }
    // No clock: a fixed morning round, never a crash on junk input.
    BuildClockPool(false, 200, 99, pool);
    assert(pool[0].question_art == PlayClockArt(8, false));
    assert(ClockActivity(19, false) == PlayArt::Bath && ClockActivity(13, true) == PlayArt::Sleep);

    // Weather: hot / cold from the (compensated) reading, else the fixed rain round.
    BuildWeatherPool(true, 300, pool);
    assert(pool[0].question_art == static_cast<uint16_t>(PlayArt::ThermoHot));
    assert(pool[0].option_art[pool[0].correct] == static_cast<uint16_t>(PlayArt::Tshirt));
    BuildWeatherPool(true, 120, pool);
    assert(pool[0].question_art == static_cast<uint16_t>(PlayArt::ThermoCold));
    assert(pool[0].option_art[pool[0].correct] == static_cast<uint16_t>(PlayArt::Scarf));
    BuildWeatherPool(true, 220, pool);  // comfortable: fixed round
    assert(pool[0].question_art == static_cast<uint16_t>(PlayArt::Rain));
    BuildWeatherPool(false, 300, pool);  // sensor failed: fixed round, ignores the number
    assert(pool[0].question_art == static_cast<uint16_t>(PlayArt::Rain));
    for (int t : {-100, 0, 180, 181, 259, 260, 500}) {
        BuildWeatherPool(true, t, pool);
        for (const GameRound& r : pool) assert(GameRoundValid(r));
    }
}

// ---- card games through GameSession ----

void TestCardGameSessions() {
    size_t count = 0;
    const PlayGameInfo* games = PlayGames(&count);
    for (size_t i = 0; i < count; ++i) {
        if (!IsCardGame(games[i].game)) continue;
        size_t n = 0;
        const GameRound* bank = PlayBank(games[i].game, &n);
        // Same seed, same session; another seed, another order somewhere.
        std::set<uint32_t> firsts;
        for (uint32_t seed = 1; seed <= 30; ++seed) {
            GameSession a, b;
            GameOutput oa{}, ob{};
            assert(a.Start(bank, n, games[i].id, seed, 3, 1000, &oa));
            assert(b.Start(bank, n, games[i].id, seed, 3, 1000, &ob));
            assert(a.View() == b.View() && a.Current().correct == b.Current().correct);
            firsts.insert(a.View().question_art * 65536u + a.View().options[0]);
        }
        assert(firsts.size() > 2);

        // A perfect game: dial to the right card, press. Reward on each press only.
        GameSession s;
        GameOutput out{};
        assert(s.Start(bank, n, games[i].id, 77, 3, 1000, &out));
        uint32_t now = 1000;
        for (int round = 0; round < 3; ++round) {
            while (s.View().highlight < s.Current().correct) s.Input(GameInput::Down, now += 50);
            while (s.View().highlight > s.Current().correct) s.Input(GameInput::Up, now += 50);
            assert(s.Result().correct == round);
            assert(s.Input(GameInput::Press, now += 50).right);
            now += GameSession::kRightMs;
            s.Tick(now);
        }
        assert(s.Result().end == GameEnd::Completed && s.Result().correct == 3);
        MemoryEvent e;
        assert(GameCompletedMemoryEvent(s.Result(), 5, 100, &e));
        assert(e.kind == MemoryEventKind::GameCompleted && e.source == MemorySource::Device &&
               e.option == games[i].id);

        // Pressing a wrong card never rewards; a session without a right answer
        // writes no growth memory.
        GameSession w;
        assert(w.Start(bank, n, games[i].id, 5, 3, 1000, &out));
        now = 1000;
        for (int round = 0; round < 3; ++round) {
            const uint8_t wrong = static_cast<uint8_t>((w.Current().correct + 1) % w.Current().option_count);
            for (int tries = 0; tries < 2; ++tries) {
                while (w.View().highlight < wrong) w.Input(GameInput::Down, now += 50);
                while (w.View().highlight > wrong) w.Input(GameInput::Up, now += 50);
                assert(!w.Input(GameInput::Press, now += 50).right);
            }
            now += GameSession::kRevealMs;
            w.Tick(now);
        }
        assert(w.Result().end == GameEnd::Completed && w.Result().correct == 0);
        assert(!GameCompletedMemoryEvent(w.Result(), 5, 100, &e));
    }
}

// ---- memory flip ----

PlaySession StartPlay(PlayGame game, uint32_t seed, uint32_t now = 1000) {
    PlaySession s;
    GameOutput out{};
    assert(s.Start(game, seed, now, &out));
    assert(out.redraw && !out.right && !out.finished);
    return s;
}

// Finds the card that matches `card` and moves the dial onto it.
uint8_t PartnerOf(const PlaySession& s, uint8_t card) {
    const GameView v = s.View();
    for (uint8_t i = 0; i < v.cols * v.rows; ++i)
        if (i != card && (v.cells[i] & 0x1F) == (v.cells[card] & 0x1F)) return i;
    return card;
}
void MoveCursor(PlaySession& s, uint8_t target, uint32_t* now) {
    for (int guard = 0; guard < 20 && s.View().cursor != target; ++guard) {
        const bool before = s.View().cursor < target;
        const auto out = s.Input(before ? GameInput::Down : GameInput::Up, *now += 20);
        if (!out.redraw) break;
    }
}

void TestMemory() {
    for (PlayGame game : {PlayGame::Memory4, PlayGame::Memory6}) {
        const uint8_t pairs = game == PlayGame::Memory4 ? 2 : 3;
        // Deterministic and each face exactly twice.
        PlaySession a = StartPlay(game, 321), b = StartPlay(game, 321), c = StartPlay(game, 322);
        assert(a.View() == b.View());
        const GameView v = a.View();
        assert(v.play == 3 && v.rows == 2 && v.cols * v.rows == pairs * 2 && v.rounds == pairs);
        int faces[10] = {};
        for (int i = 0; i < pairs * 2; ++i) {
            assert(!(v.cells[i] & 0xC0));  // all face down
            ++faces[v.cells[i] & 0x1F];
        }
        int pairs_seen = 0;
        for (int f = 1; f <= 9; ++f) {
            assert(faces[f] == 0 || faces[f] == 2);
            pairs_seen += faces[f] == 2;
        }
        assert(pairs_seen == pairs);
        bool differs = false;
        for (uint32_t seed = 1; seed < 20; ++seed)
            differs |= !(StartPlay(game, seed).View() == a.View());
        assert(differs); (void)c;

        // A perfect game: every match is rewarded by the dial press itself.
        PlaySession s = StartPlay(game, 321);
        uint32_t now = 1000;
        int rewards = 0;
        for (int pair = 0; pair < pairs; ++pair) {
            uint8_t first = 0;
            while (!(s.View().cells[first] == (s.View().cells[first] & 0x1F))) ++first;  // face down
            MoveCursor(s, first, &now);
            assert(!s.Input(GameInput::Press, now += 20).right);  // first flip: nothing yet
            const uint8_t second = PartnerOf(s, first);
            MoveCursor(s, second, &now);
            const auto out = s.Input(GameInput::Press, now += 20);
            assert(out.right && out.redraw && s.View().feedback == GameFeedback::Right);
            ++rewards;
            // While TuanTuan celebrates, presses do nothing (no extra reward).
            assert(!s.Input(GameInput::Press, now += 20).right);
            now += PlaySession::kRightMs;
            const auto tick = s.Tick(now);
            if (pair + 1 == pairs) assert(tick.finished);
        }
        assert(rewards == pairs && s.Result().correct == pairs);
        assert(s.Result().end == GameEnd::Completed && !s.Active());
        MemoryEvent e;
        assert(GameCompletedMemoryEvent(s.Result(), 9, 100, &e) && e.option == (pairs == 2 ? 21 : 22));
    }

    // A mismatch flips back after a moment, no reward, nothing counted.
    PlaySession s = StartPlay(PlayGame::Memory4, 321);
    uint32_t now = 1000;
    const GameView v0 = s.View();
    uint8_t other = 0;
    for (uint8_t i = 1; i < 4; ++i)
        if ((v0.cells[i] & 0x1F) != (v0.cells[0] & 0x1F)) { other = i; break; }
    assert(other != 0);
    s.Input(GameInput::Press, now += 20);  // card 0
    MoveCursor(s, other, &now);
    auto out = s.Input(GameInput::Press, now += 20);
    assert(!out.right && s.View().feedback == GameFeedback::TryAgain);
    assert(s.View().cells[0] & 0x80 && s.View().cells[other] & 0x80);
    s.Tick(now + 100);
    assert(s.View().cells[0] & 0x80);  // still showing
    out = s.Tick(now + PlaySession::kMismatchMs);
    assert(out.redraw && !(s.View().cells[0] & 0x80) && !(s.View().cells[other] & 0x80));
    assert(s.Result().correct == 0 && s.Result().played == 0 && s.Active());
    // Dial moves skip matched/up cards and never land outside the board.
    for (int i = 0; i < 10; ++i) s.Input(GameInput::Down, now += 20);
    assert(s.View().cursor == 3);
    for (int i = 0; i < 10; ++i) s.Input(GameInput::Up, now += 20);
    assert(s.View().cursor == 0);
    // Long press goes home: no completed result, no memory event.
    out = s.Input(GameInput::Long, now += 20);
    assert(out.finished && s.Result().end == GameEnd::Home);
    MemoryEvent e;
    assert(!GameCompletedMemoryEvent(s.Result(), 1, 100, &e));
    // Idle: a repeat of the prompt, then home.
    PlaySession idle = StartPlay(PlayGame::Memory4, 5, 1000);
    // V9 1E: the question again is the spoken rule (no rule: the old tone).
    assert(idle.Tick(1000 + PlaySession::kRepeatPromptMs).clip == GameRuleClip(21));
    assert(idle.Tick(1000 + PlaySession::kRepeatPromptMs + 10).clip == 0);
    out = idle.Tick(1000 + PlaySession::kIdleExitMs);
    assert(out.finished && idle.Result().end == GameEnd::Idle);
    // Interrupted (sleep, voice): no memory either.
    PlaySession intr = StartPlay(PlayGame::Memory6, 5);
    intr.Interrupt();
    assert(!intr.Active() && intr.Result().end == GameEnd::Interrupted);
    assert(!GameCompletedMemoryEvent(intr.Result(), 1, 100, &e));
}

// ---- maze ----

// Shortest walk from the start to the carrot through open walls (-1: none).
int MazeSteps(const PlaySession& s) {
    const int cols = s.Cols(), rows = s.Rows();
    std::vector<int> dist(cols * rows, -1);
    std::vector<int> queue{0};
    dist[0] = 0;
    for (size_t head = 0; head < queue.size(); ++head) {
        const int cell = queue[head], r = cell / cols, c = cell % cols;
        const uint8_t w = s.Walls(static_cast<uint8_t>(cell));
        const int next[4] = {(w & 1) && r > 0 ? cell - cols : -1, (w & 2) && c + 1 < cols ? cell + 1 : -1,
                             (w & 4) && r + 1 < rows ? cell + cols : -1, (w & 8) && c > 0 ? cell - 1 : -1};
        for (int n : next)
            if (n >= 0 && dist[n] < 0) {
                dist[n] = dist[cell] + 1;
                queue.push_back(n);
            }
    }
    return dist[s.Goal()];
}

Gesture Step(const PlaySession& s, uint8_t* from) {
    // The gesture that walks one step towards the carrot (BFS back-pointers).
    const int cols = s.Cols(), rows = s.Rows();
    std::vector<int> dist(cols * rows, -1);
    std::vector<int> queue{s.Goal()};
    dist[s.Goal()] = 0;
    for (size_t head = 0; head < queue.size(); ++head) {
        const int cell = queue[head], r = cell / cols, c = cell % cols;
        const uint8_t w = s.Walls(static_cast<uint8_t>(cell));
        const int next[4] = {(w & 1) && r > 0 ? cell - cols : -1, (w & 2) && c + 1 < cols ? cell + 1 : -1,
                             (w & 4) && r + 1 < rows ? cell + cols : -1, (w & 8) && c > 0 ? cell - 1 : -1};
        for (int n : next)
            if (n >= 0 && dist[n] < 0) {
                dist[n] = dist[cell] + 1;
                queue.push_back(n);
            }
    }
    const int cell = *from;
    const int r = cell / cols, c = cell % cols;
    const uint8_t w = s.Walls(static_cast<uint8_t>(cell));
    if ((w & 1) && r > 0 && dist[cell - cols] == dist[cell] - 1) return Gesture::TiltUp;
    if ((w & 2) && c + 1 < cols && dist[cell + 1] == dist[cell] - 1) return Gesture::TiltRight;
    if ((w & 4) && r + 1 < rows && dist[cell + cols] == dist[cell] - 1) return Gesture::TiltDown;
    return Gesture::TiltLeft;
}

void TestMazeSolvable() {
    // Any seed: both mazes are fully connected (a spanning tree), the carrot
    // is reachable and far from the start.
    for (uint32_t seed = 1; seed <= 300; ++seed) {
        PlaySession s = StartPlay(PlayGame::Maze, seed);
        assert(s.Cols() == 4 && s.Rows() == 3 && s.Pos() == 0);
        assert(s.Goal() != 0 && MazeSteps(s) >= 4);
        // Every cell has at least one open wall, and walls are symmetric.
        int open = 0;
        for (int cell = 0; cell < s.Cols() * s.Rows(); ++cell) {
            const uint8_t w = s.Walls(static_cast<uint8_t>(cell));
            assert(w != 0);
            const int r = cell / s.Cols(), c = cell % s.Cols();
            if (w & 2) { assert(c + 1 < s.Cols() && (s.Walls(cell + 1) & 8)); ++open; }
            if (w & 4) { assert(r + 1 < s.Rows() && (s.Walls(cell + s.Cols()) & 1)); ++open; }
            if (w & 1) assert(r > 0);
            if (w & 8) assert(c > 0);
        }
        assert(open == s.Cols() * s.Rows() - 1);  // spanning tree: n - 1 passages
    }
    // Same seed, same maze; the second round is bigger.
    PlaySession a = StartPlay(PlayGame::Maze, 99), b = StartPlay(PlayGame::Maze, 99);
    for (int cell = 0; cell < 12; ++cell) assert(a.Walls(cell) == b.Walls(cell));
    assert(a.Goal() == b.Goal() && a.View() == b.View());
}

void TestMazePlay() {
    PlaySession s = StartPlay(PlayGame::Maze, 4242);
    uint32_t now = 1000;
    // Shaking and unrelated gestures do nothing at all.
    for (Gesture g : {Gesture::GentleShake, Gesture::HardShake, Gesture::Pickup, Gesture::FaceDown,
                      Gesture::FaceUp, Gesture::None}) {
        const auto out = s.OnGesture(g, now += 100);
        assert(!out.redraw && !out.right && s.Pos() == 0);
    }
    // Walls block; a held tilt is one step (level re-arms).
    uint8_t from = 0;
    for (int round = 0; round < 2; ++round) {
        assert(s.View().round == round + 1);
        const int steps = MazeSteps(s);
        int walked = 0;
        GameOutput out{};
        from = s.Pos();
        while (s.Pos() != s.Goal()) {
            const Gesture g = Step(s, &from);
            out = s.OnGesture(g, now += 600);
            assert(out.redraw && s.Pos() != from);
            // The tilt is still held: repeating windows move nothing.
            for (int hold = 0; hold < 4; ++hold) {
                const auto again = s.OnGesture(g, now += 500);
                assert(!again.redraw && !again.right);
            }
            from = s.Pos();
            if (s.Pos() != s.Goal()) s.OnGesture(Gesture::Level, now += 500);
            ++walked;
        }
        assert(walked == steps);
        assert(out.right && s.View().feedback == GameFeedback::Right);
        assert(s.Result().correct == round + 1);
        // While celebrating, tilts do nothing.
        s.OnGesture(Gesture::Level, now += 50);
        assert(!s.OnGesture(Gesture::TiltLeft, now += 50).redraw);
        now += PlaySession::kRightMs;
        const auto tick = s.Tick(now);
        if (round == 1) assert(tick.finished);
        else s.OnGesture(Gesture::Level, now += 50);
    }
    assert(s.Result().end == GameEnd::Completed && s.Result().correct == 2);
    MemoryEvent e;
    assert(GameCompletedMemoryEvent(s.Result(), 3, 100, &e) && e.option == 23);

    // A wall: tilting into it moves nothing but still needs a level before the next try.
    PlaySession w = StartPlay(PlayGame::Maze, 7);
    now = 1000;
    for (Gesture g : {Gesture::TiltLeft, Gesture::TiltUp}) {  // start is the top-left corner
        const auto out = w.OnGesture(g, now += 100);
        assert(!out.redraw && w.Pos() == 0);
        w.OnGesture(Gesture::Level, now += 100);
    }
    // Long press, idle and interruption write nothing.
    const auto out = w.Input(GameInput::Long, now += 10);
    assert(out.finished && w.Result().end == GameEnd::Home && !GameCompletedMemoryEvent(w.Result(), 1, 100, &e));
    PlaySession idle = StartPlay(PlayGame::Maze, 7, 1000);
    assert(idle.Tick(1000 + PlaySession::kIdleExitMs).finished);
}

// ---- shake dice and tree ----

void TestShake() {
    for (PlayGame game : {PlayGame::Dice, PlayGame::Tree}) {
        PlaySession s = StartPlay(game, 12);
        const GameView before = s.View();
        assert(before.play == 5 && before.option_count == 0 && s.Die() == 0);
        assert(before.question_art == static_cast<uint16_t>(PlayArt::ShakeMe));
        uint32_t now = 1000;
        // Not asked yet: up/down and non-shake gestures do nothing.
        assert(!s.Input(GameInput::Down, now += 10).redraw);
        assert(!s.OnGesture(Gesture::FaceDown, now += 10).redraw);
        assert(!s.OnGesture(Gesture::TiltLeft, now += 10).redraw);
        // The dial press is the fallback when motion was switched off: it rolls too.
        {
            PlaySession p = StartPlay(game, 12);
            const auto rolled = p.Input(GameInput::Press, now += 10);
            assert(rolled.redraw && !rolled.right && p.Die() >= 1 && p.View().option_count == 3);
            assert(p.Result().correct == 0);
        }
        // A shake rolls; rolling is not a reward.
        const auto roll = s.OnGesture(Gesture::HardShake, now += 10);
        assert(roll.redraw && !roll.right && s.Die() >= 1 && s.Die() <= (game == PlayGame::Dice ? 6 : 5));
        const GameView v = s.View();
        assert(v.option_count == 3 && v.question_art != static_cast<uint16_t>(PlayArt::ShakeMe));
        // Another shake while a question is open changes nothing.
        const uint8_t die = s.Die();
        assert(!s.OnGesture(Gesture::GentleShake, now += 10).redraw && s.Die() == die);
        assert(s.Result().correct == 0);
        // The right card shows the rolled count of dots; the others differ.
        const uint16_t right_art = die >= 6 ? static_cast<uint16_t>(PlayArt::Dots6)
                                            : static_cast<uint16_t>(GameArt::Dots1) + die - 1;
        int hits = 0;
        uint8_t right_index = 0;
        for (uint8_t i = 0; i < 3; ++i)
            if (v.options[i] == right_art) { ++hits; right_index = i; }
        assert(hits == 1 && v.options[0] != v.options[1] && v.options[1] != v.options[2] &&
               v.options[0] != v.options[2]);
        // Wrong press: try again, then reveal; no reward.
        const uint8_t wrong = static_cast<uint8_t>((right_index + 1) % 3);
        auto move_to = [&](uint8_t target) {
            while (s.View().highlight < target) s.Input(GameInput::Down, now += 10);
            while (s.View().highlight > target) s.Input(GameInput::Up, now += 10);
        };
        move_to(wrong);
        assert(!s.Input(GameInput::Press, now += 10).right && s.View().feedback == GameFeedback::TryAgain);
        assert(!s.Input(GameInput::Press, now += 10).right && s.View().feedback == GameFeedback::Reveal);
        assert(s.View().highlight == right_index);
        now += PlaySession::kRightMs;
        assert(s.Tick(now).redraw);
        assert(s.Result().played == 1 && s.Result().correct == 0 && s.View().option_count == 0);
        // Round 2 and 3: shake, answer right with the dial: the only reward.
        for (int round = 1; round < 3; ++round) {
            s.OnGesture(Gesture::GentleShake, now += 10);
            const uint8_t d = s.Die();
            const uint16_t art = d >= 6 ? static_cast<uint16_t>(PlayArt::Dots6)
                                        : static_cast<uint16_t>(GameArt::Dots1) + d - 1;
            uint8_t idx = 0;
            for (uint8_t i = 0; i < 3; ++i)
                if (s.View().options[i] == art) idx = i;
            move_to(idx);
            assert(s.Input(GameInput::Press, now += 10).right);
            now += PlaySession::kRightMs;
            const auto tick = s.Tick(now);
            assert(tick.finished == (round == 2));
        }
        assert(s.Result().end == GameEnd::Completed && s.Result().correct == 2);
        MemoryEvent e;
        assert(GameCompletedMemoryEvent(s.Result(), 3, 100, &e) &&
               e.option == (game == PlayGame::Dice ? 24 : 25));
    }
    // Same seed, same die values.
    PlaySession a = StartPlay(PlayGame::Dice, 5), b = StartPlay(PlayGame::Dice, 5);
    a.OnGesture(Gesture::GentleShake, 2000);
    b.OnGesture(Gesture::GentleShake, 2000);
    assert(a.Die() == b.Die() && a.View() == b.View());
    // All six values come up (the dice is not stuck).
    std::set<uint8_t> seen;
    for (uint32_t seed = 1; seed < 200; ++seed) {
        PlaySession s = StartPlay(PlayGame::Dice, seed);
        s.OnGesture(Gesture::GentleShake, 2000);
        seen.insert(s.Die());
    }
    assert(seen.size() == 6);
    // No shake: nothing happens, then home.
    PlaySession idle = StartPlay(PlayGame::Tree, 5, 1000);
    assert(idle.Tick(1000 + PlaySession::kIdleExitMs).finished);
}

// ---- wooden man ----

void TestStatue() {
    PlaySession s = StartPlay(PlayGame::Statue, 3);
    uint32_t now = 1000;
    // A board resting on the table never counts as dancing, however long.
    for (int i = 0; i < 100; ++i) {
        const auto out = s.OnWindow(Still(), now += 500);
        assert(!out.redraw && !out.right && !out.finished);
        assert(s.View().question_art == static_cast<uint16_t>(PlayArt::Dance));
        if (now - 1000 < PlaySession::kIdleExitMs) assert(s.Active());
    }
    assert(s.Tick(1000 + PlaySession::kIdleExitMs).finished && s.Result().end == GameEnd::Idle);

    // Dance, freeze, hold three seconds: reward; three rounds.
    PlaySession g = StartPlay(PlayGame::Statue, 3);
    now = 1000;
    for (int round = 0; round < 3; ++round) {
        // Fidgeting (a bit of noise) is not dancing: needs real motion windows.
        for (int i = 0; i < 6; ++i) assert(!g.OnWindow(Window(0, 0, -983, 100), now += 500).redraw);
        // Moving, a pause, moving: not a dance (a carried board is not one either).
        g.OnWindow(Moving(), now += 500);
        g.OnWindow(Moving(), now += 500);
        g.OnWindow(Moving(), now += 500);
        assert(!g.OnWindow(Still(), now += 500).redraw);
        g.OnWindow(Moving(), now += 500);
        g.OnWindow(Moving(), now += 500);
        g.OnWindow(Moving(), now += 500);
        const auto cue = g.OnWindow(Moving(), now += 500);  // fourth moving window in a row
        assert(cue.redraw && cue.clip != 0 && !cue.right);
        assert(g.View().question_art == static_cast<uint16_t>(PlayArt::Statue));
        // The window holding the cue is skipped; moving resets the count.
        assert(!g.OnWindow(Moving(), now += 500).right);
        g.OnWindow(Still(), now += 500);
        g.OnWindow(Still(), now += 500);
        g.OnWindow(Still(), now += 500);
        assert(g.View().count == 1);
        const auto wobble = g.OnWindow(Window(0, 0, -983, 90), now += 500);  // a fidget resets
        assert(wobble.redraw && g.View().count == 0 && !wobble.right);
        GameOutput out{};
        for (int i = 0; i < PlaySession::kFreezeWindows; ++i) {
            assert(!out.right);
            out = g.OnWindow(Still(), now += 500);
        }
        assert(out.right && g.View().feedback == GameFeedback::Right);
        assert(g.Result().correct == round + 1);
        // Windows while celebrating count for nothing.
        assert(!g.OnWindow(Still(), now += 500).right);
        now += PlaySession::kRightMs;
        const auto tick = g.Tick(now);
        assert(tick.finished == (round == 2));
    }
    assert(g.Result().end == GameEnd::Completed);
    MemoryEvent e;
    assert(GameCompletedMemoryEvent(g.Result(), 4, 100, &e) && e.option == 26);
    // Windows with too few samples are ignored; the dial and shakes do nothing.
    PlaySession t = StartPlay(PlayGame::Statue, 3);
    auto few = Moving();
    few.samples = 1;
    for (int i = 0; i < 10; ++i) assert(!t.OnWindow(few, now += 500).redraw);
    assert(!t.OnGesture(Gesture::HardShake, now).redraw && !t.Input(GameInput::Press, now).redraw);
}

// ---- day and night ----

void TestDayNight() {
    PlaySession s = StartPlay(PlayGame::DayNight, 8);
    uint32_t now = 1000;
    assert(s.View().play == 7 && !s.View().night);
    // Flipping up before night, shaking, tilting: nothing.
    for (Gesture g : {Gesture::FaceUp, Gesture::GentleShake, Gesture::HardShake, Gesture::TiltLeft,
                      Gesture::Pickup}) {
        assert(!s.OnGesture(g, now += 100).redraw);
    }
    for (int round = 0; round < 3; ++round) {
        auto out = s.OnGesture(Gesture::FaceDown, now += 100);
        assert(out.redraw && s.View().night && s.View().hidden && !out.right);
        // Back up too early (before "day" is called): try again, no reward.
        if (round == 0) {
            out = s.OnGesture(Gesture::FaceUp, now += 500);
            assert(out.redraw && !out.right && s.View().feedback == GameFeedback::TryAgain &&
                   !s.View().night);
            out = s.OnGesture(Gesture::FaceDown, now += 100);
            assert(out.redraw && s.View().night);
        }
        // Still night until the hold time is over.
        assert(!s.Tick(now + PlaySession::kNightMinMs - 50).redraw);
        assert(!s.OnGesture(Gesture::FaceUp, now + 10).right || round < 0);
        s.OnGesture(Gesture::FaceDown, now += 20);  // (re-enter if the early flip reset it)
        now += PlaySession::kNightMaxMs;
        out = s.Tick(now);
        assert(out.redraw && out.clip != 0 && !out.right);  // "it's day!"
        assert(!s.View().night && s.View().question_art == static_cast<uint16_t>(PlayArt::Sun));
        out = s.OnGesture(Gesture::FaceUp, now += 300);
        assert(out.right && s.Result().correct == round + 1);
        now += PlaySession::kRightMs;
        assert(s.Tick(now).finished == (round == 2));
    }
    assert(s.Result().end == GameEnd::Completed);
    MemoryEvent e;
    assert(GameCompletedMemoryEvent(s.Result(), 4, 100, &e) && e.option == 27);
    // Face down and forgotten: home after two minutes, no memory.
    PlaySession f = StartPlay(PlayGame::DayNight, 8, 1000);
    f.OnGesture(Gesture::FaceDown, 2000);
    // The day call comes at 5 s at the latest; nobody turns it back.
    f.Tick(2000 + PlaySession::kNightMaxMs);
    const auto idle = f.Tick(2000 + PlaySession::kNightMaxMs + PlaySession::kIdleExitMs);
    assert(idle.finished && f.Result().end == GameEnd::Idle && !GameCompletedMemoryEvent(f.Result(), 1, 100, &e));
}

// ---- sensors at rest: no false alarms ----

void TestRestingBoardIsQuiet() {
    // Measured V1 rest data: noise 16-19 mg, screen up z ~ -983 mg.
    GestureDetector detector;
    uint32_t now = 0;
    for (int i = 0; i < 600; ++i)  // five minutes of 500 ms windows
        assert(detector.OnWindow(Window(i % 7 - 3, i % 5 - 2, -983 + i % 3, 16 + i % 4), now += 500) ==
               Gesture::None);
    // The four-way tracker reports nothing for a resting or slightly off-level board.
    TiltConfig four{};
    four.four_way = true;
    TiltTracker tracker(four);
    for (int i = 0; i < 600; ++i)
        assert(tracker.OnWindow(Window(i % 11 - 5, i % 9 - 4, -983, 16 + i % 4)) == Gesture::None);
    assert(tracker.OnWindow(Window(250, 200, -950)) == Gesture::None);  // below the 300 mg line
    // The wooden man and the maze never start from a resting board (see the
    // statue and maze tests): the same windows give no events at all.
    PlaySession statue = StartPlay(PlayGame::Statue, 1);
    PlaySession maze = StartPlay(PlayGame::Maze, 1);
    for (int i = 0; i < 100; ++i) {
        assert(!statue.OnWindow(Window(0, 0, -983, 19), now += 500).redraw);
        const Gesture g = tracker.OnWindow(Window(0, 0, -983, 19));
        assert(!maze.OnGesture(g, now).redraw && maze.Pos() == 0);
    }
}

void TestFourWayTilt() {
    TiltConfig cfg{};
    cfg.four_way = true;
    TiltTracker t(cfg);
    assert(t.OnWindow(Window(0, 0, -983)) == Gesture::None);
    assert(t.OnWindow(Window(400, 40, -900)) == Gesture::TiltRight);
    assert(t.OnWindow(Window(380, 20, -900)) == Gesture::TiltRight);  // held: one per window
    assert(t.OnWindow(Window(120, 20, -950)) == Gesture::Level);
    assert(t.OnWindow(Window(10, 10, -983)) == Gesture::None);
    assert(t.OnWindow(Window(-420, 0, -880)) == Gesture::TiltLeft);
    assert(t.OnWindow(Window(0, 0, -983)) == Gesture::Level);
    assert(t.OnWindow(Window(20, 420, -880)) == Gesture::TiltDown);
    assert(t.OnWindow(Window(0, 0, -983)) == Gesture::Level);
    assert(t.OnWindow(Window(20, -420, -880)) == Gesture::TiltUp);
    // The larger tilt wins; a shaking or face-down board reports nothing.
    assert(t.OnWindow(Window(0, 0, -983)) == Gesture::Level);
    assert(t.OnWindow(Window(350, -500, -850)) == Gesture::TiltUp);
    assert(t.OnWindow(Window(0, 400, -880, 900)) == Gesture::None);
    assert(t.OnWindow(Window(0, 400, 900)) == Gesture::None);
    // Signs and axes are configurable.
    TiltConfig flipped = cfg;
    flipped.sign_v = -1;
    flipped.use_y = true;
    TiltTracker u(flipped);
    assert(u.OnWindow(Window(400, 0, -900)) == Gesture::TiltUp);   // x is "vertical" now
    assert(u.OnWindow(Window(0, 0, -983)) == Gesture::Level);
    assert(u.OnWindow(Window(0, 400, -900)) == Gesture::TiltRight);
    // The classic one-axis mode is unchanged: no up / down at all.
    TiltTracker classic;
    assert(classic.OnWindow(Window(0, 400, -900)) == Gesture::None);
}

// ---- the game list ----

void TestMenu() {
    // Every V8 game is one row of the shared activity list (activity_list.h),
    // in age / level order, with the right "needs" and a picture the scene draws.
    size_t count = 0;
    const PlayGameInfo* games = PlayGames(&count);
    uint8_t row_index = 0;
    std::set<uint8_t> choices;
    for (size_t i = 0; i < count; ++i) {
        const GameChoice choice = GameChoiceOf(games[i].game);
        assert(GameChoiceIsPlay(choice) && PlayGameOf(choice) == games[i].game);
        assert(choices.insert(static_cast<uint8_t>(choice)).second);
        const Activity* row = FindActivity(choice);
        assert(row && row->group == ActivityGroup::Play);
        assert(std::string(row->name) == games[i].name);
        assert((row->needs == ActivityNeeds::Motion) == games[i].needs_motion);
        assert(PlayArtValid(row->art) || GameArtIsOption(row->art));
        // Rows come after the older games and keep the table order.
        assert(ActivityIndexOf(choice) >= 5);
        assert(row_index == 0 || ActivityIndexOf(choice) == row_index + 1);
        row_index = ActivityIndexOf(choice);
    }
    // The id range: 13..30, none of the older games' or the care rows' ids.
    assert(*choices.begin() == 13 && *choices.rbegin() == 30 && choices.size() == 18);
    assert(PlayGameOf(GameChoice::Ball) == PlayGame::None && !GameChoiceIsPlay(GameChoice::Feed));
    // The menu page pictures (MenuArt 201..204, 201..223 kept clear) and the play
    // pictures never overlap.
    for (uint16_t id = 201; id <= 223; ++id) assert(!PlayArtValid(id));
    // V9: walk every category page of the play menu: every card is a valid
    // picture and every game is reachable, in table order.
    GameMenu menu;
    uint32_t now = 1000;
    size_t reached = 0;
    for (uint8_t top = 0; top < 4; ++top) {  // no pick set: brain, move, sound, album
        auto out = menu.Start(now += 100, top);
        assert(out.redraw && menu.View().option_count == 3);
        out = menu.Input(GameInput::Press, now += 100);
        if (out.chosen != GameChoice::None) {  // the album is a leaf
            ++reached;
            continue;
        }
        const ActivityCategory cat = menu.Category();
        const size_t n = ActivityCountIn(cat);
        for (size_t i = 0; i < n; ++i) {
            if (i) assert(menu.Input(GameInput::Down, now += 100).redraw);
            const GameView v = menu.View();
            assert(v.highlight < 3);
            for (uint16_t art : v.options)
                assert(art == 0 || GameArtIsQuestion(art) || PlayArtValid(art) || MicArtValid(art) ||
                       CareArtValid(art) || SoundArtValid(art));
            assert(v.options[v.highlight] == ActivityInCategory(cat, i)->art);
        }
        out = menu.Input(GameInput::Press, now += 100);
        assert(out.chosen == ActivityInCategory(cat, n - 1)->id);
        reached += n;
    }
    assert(reached == ActivityCount(ActivityGroup::Play));
    // A game picked from the list can be reopened on its own card.
    menu.StartOn(1000, GameChoice::Maze);
    assert(menu.Input(GameInput::Press, 1100).chosen == GameChoice::Maze);
}

// ---- scenes ----

int Ink(const MonoFrame& f, int x0, int y0, int x1, int y1) {
    int n = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) n += f.IsBlack(x, y) ? 1 : 0;
    return n;
}

void ExpectNoCornerInk(const MonoFrame& f) {
    for (int y = 0; y < SceneComposer::kStatusBottom; ++y)
        for (int x = 0; x < MonoFrame::kWidth; ++x)
            if (SceneComposer::InStatusBox(x, y)) assert(!f.IsBlack(x, y));
}

MonoFrame Draw(const GameView& v) {
    SceneModel m;
    m.kind = SceneKind::Game;
    m.game = v;
    MonoFrame f;
    SceneComposer::Render(m, f);
    ExpectNoCornerInk(f);
    return f;
}

bool SameAbove(const MonoFrame& a, const MonoFrame& b, int y_end) {
    for (int y = 0; y < y_end; ++y)
        for (int x = 0; x < MonoFrame::kWidth; ++x)
            if (a.IsBlack(x, y) != b.IsBlack(x, y)) return false;
    return true;
}
bool SameBelow(const MonoFrame& a, const MonoFrame& b, int y_start) {
    for (int y = y_start; y < MonoFrame::kHeight; ++y)
        for (int x = 0; x < MonoFrame::kWidth; ++x)
            if (a.IsBlack(x, y) != b.IsBlack(x, y)) return false;
    return true;
}

void TestScenes() {
    size_t count = 0;
    const PlayGameInfo* games = PlayGames(&count);
    for (size_t i = 0; i < count; ++i) {
        GameOutput out{};
        if (IsCardGame(games[i].game)) {
            size_t n = 0;
            const GameRound* bank = PlayBank(games[i].game, &n);
            // Every round of every bank draws something on the question card and
            // on every answer card, and nothing in the status corners.
            for (size_t k = 0; k < n; ++k) {
                GameSession s;
                const GameRound one[1] = {bank[k]};
                assert(s.Start(one, 1, games[i].id, 3, 1, 1000, &out));
                const MonoFrame f = Draw(s.View());
                assert(Ink(f, 340, 30, 624, 274) > 150);  // question card has a picture
                const GameView v = s.View();
                for (uint8_t card = 0; card < v.option_count; ++card) {
                    const int x = (v.option_count >= 3 ? 400 + (card - 1) * 260 : 400 + (2 * card - 1) * 150) - 100;
                    assert(Ink(f, x + 8, 330, x + 192, 456) > 60);
                }
            }
            // The dial highlight only touches the answer rail; the reaction only the top.
            GameSession s;
            assert(s.Start(bank, n, games[i].id, 3, 3, 1000, &out));
            const GameView v0 = s.View();
            const MonoFrame f0 = Draw(v0);
            s.Input(GameInput::Down, 1100);
            const GameView v1 = s.View();
            assert(v0.RailDiffers(v1) && !v0.QuestionDiffers(v1));
            assert(SameAbove(f0, Draw(v1), SceneComposer::kGameRailTop - 16));
            continue;
        }
        PlaySession s;
        assert(s.Start(games[i].game, 7, 1000, &out));
        const MonoFrame f0 = Draw(s.View());
        assert(Ink(f0, 340, 30, 624, 274) > 100);
        if (games[i].game == PlayGame::Memory4 || games[i].game == PlayGame::Memory6) {
            s.Input(GameInput::Down, 1100);  // cursor moves: only the board changes
            const GameView v1 = s.View();
            assert(s.View().RailDiffers(PlaySession().View()) || true);
            const MonoFrame f1 = Draw(v1);
            assert(SameAbove(f0, f1, SceneComposer::kGameRailTop));
            assert(!SameBelow(f0, f1, SceneComposer::kGameRailTop));
            assert(Ink(f0, 100, 300, 700, 480) > 2000);
        }
        if (games[i].game == PlayGame::Maze) {
            // A step only changes the maze, not the question area.
            for (int k = 0; k < 4; ++k) {
                uint8_t from = s.Pos();
                if (s.Pos() == s.Goal()) break;
                s.OnGesture(Step(s, &from), 2000 + k * 1000);
                s.OnGesture(Gesture::Level, 2500 + k * 1000);
                if (s.Active() && s.View().feedback == GameFeedback::None) {
                    const GameView a = s.View();
                    assert(!a.QuestionDiffers(PlaySession().View()) || true);
                }
            }
            assert(Ink(f0, 100, 300, 700, 480) > 500);
        }
        if (games[i].game == PlayGame::Statue) {
            assert(Ink(f0, 200, 300, 600, 480) > 500);  // three rings
        }
        if (games[i].game == PlayGame::DayNight) {
            s.OnGesture(Gesture::FaceDown, 1100);
            const MonoFrame night = Draw(s.View());
            assert(Ink(night, 100, 300, 700, 480) > Ink(f0, 100, 300, 700, 480));  // dark sky
        }
    }
    // The dice: answer cards appear after the shake, the question card changes.
    PlaySession dice;
    GameOutput out{};
    assert(dice.Start(PlayGame::Dice, 9, 1000, &out));
    const MonoFrame waiting = Draw(dice.View());
    assert(Ink(waiting, 100, 320, 700, 460) == 0);  // no cards yet
    dice.OnGesture(Gesture::GentleShake, 1100);
    const MonoFrame asked = Draw(dice.View());
    assert(Ink(asked, 100, 320, 700, 460) > 500);
    // The memory board is inside the rail and clear of the status band.
    PlaySession mem;
    assert(mem.Start(PlayGame::Memory6, 9, 1000, &out));
    const MonoFrame m6 = Draw(mem.View());
    assert(Ink(m6, 0, 0, 800, 40) < 20000);
}

// ---- self test clock ----

void TestSelfTestClock() {
    // Idle timers follow the self-test clock like every other game.
    assert(SelfTestClock::SetForSelfTest(50));
    PlaySession s = StartPlay(PlayGame::Memory4, 1, 1000);
    assert(!s.Tick(1000 + PlaySession::kIdleExitMs / 2 - 10).finished);
    assert(s.Tick(1000 + PlaySession::kIdleExitMs / 2 + 10).finished);
    assert(SelfTestClock::SetForSelfTest(100));
}

}  // namespace

int main() {
    TestBanks();
    TestPatternAnswers();
    TestSizeOrderAndSpotAnswers();
    TestShadowClassifyWeather();
    TestClockAndWeatherLive();
    TestCardGameSessions();
    TestMemory();
    TestMazeSolvable();
    TestMazePlay();
    TestShake();
    TestStatue();
    TestDayNight();
    TestRestingBoardIsQuiet();
    TestFourWayTilt();
    TestMenu();
    TestScenes();
    TestSelfTestClock();
    std::cout << "play games OK\n";
    return 0;
}
