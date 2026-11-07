// V4-06 mini games: built-in rounds, deterministic sessions, gentle
// feedback, rewards only for correct dial answers, growth memory, tones,
// the game scene and its refresh regions.
#include "kid_app/memory_store.h"
#include "kid_app/mini_game.h"
#include "kid_app/pet_refresh_gate.h"
#include "kid_app/refresh_planner.h"
#include "kid_app/scene_model.h"
#include "kid_app/selftest_clock.h"

#include <cassert>
#include <iostream>
#include <set>
#include <vector>

using namespace kid_app;

namespace {

std::vector<GameRound> Pool() {
    std::vector<GameRound> pool;
    for (size_t i = 0; i < BuiltinGameRoundCount(); ++i) pool.push_back(BuiltinGameRound(i));
    return pool;
}

GameSession Started(const std::vector<GameRound>& pool, uint32_t seed, uint8_t rounds = 3,
                    uint32_t now = 1000) {
    GameSession s;
    GameOutput out{};
    assert(s.Start(pool.data(), pool.size(), kBuiltinGameId, seed, rounds, now, &out));
    assert(out.redraw && out.clip == s.Current().prompt_clip && !out.finished && !out.right);
    return s;
}

// Moves the highlight onto option `target` (dial down from the left end).
void MoveTo(GameSession& s, uint8_t target, uint32_t now) {
    while (s.View().highlight > target) s.Input(GameInput::Up, now);
    while (s.View().highlight < target) s.Input(GameInput::Down, now);
}

uint8_t Wrong(const GameSession& s) { return s.Current().correct == 0 ? 1 : 0; }

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

SceneModel GameScene(const GameView& view) {
    SceneModel scene{};
    scene.kind = SceneKind::Game;
    scene.game = view;
    return scene;
}

}  // namespace

int main() {
    const auto pool = Pool();

    // Built-in set: valid, wordless (picture matching), playable by tones.
    {
        assert(BuiltinGameRoundCount() >= 6);
        std::set<GameKind> kinds;
        for (const auto& r : pool) {
            assert(GameRoundValid(r));
            assert(r.question_art != 0);  // the question card shows what to find
            kinds.insert(r.kind);
            assert(GameToneSamples(r.prompt_clip) > 0);
            for (uint8_t i = 0; i < r.option_count; ++i) {
                assert(GameToneSamples(r.option_clip[i]) > 0);
                assert(GameArtIsOption(r.option_art[i]));
            }
        }
        assert(kinds.count(GameKind::MatchShape) && kinds.count(GameKind::Count));
        for (uint16_t id : {kGameClipRight, kGameClipTryAgain, kGameClipReveal, kGameClipBye})
            assert(GameToneSamples(id) > 0);
        assert(GameToneSamples(0) == 0 && GameToneSamples(1) == 0 && GameToneSamples(16) == 0);
        // Counting cards beep their number.
        assert(GameToneSamples(GameArtTone(static_cast<uint16_t>(GameArt::Dots3))) >
               GameToneSamples(GameArtTone(static_cast<uint16_t>(GameArt::Dots2))));
        int16_t a[480] = {}, b[480] = {};
        const uint16_t right = kGameClipRight;
        assert(RenderGameTone(right, 0, a, 480) == 480 && RenderGameTone(right, 0, b, 480) == 480);
        bool loud = false;
        for (int i = 0; i < 480; ++i) {
            assert(a[i] == b[i]);
            loud = loud || a[i] > 2000 || a[i] < -2000;
        }
        assert(loud);
        const size_t total = GameToneSamples(right);
        assert(RenderGameTone(right, total - 10, a, 480) == 10);
        assert(RenderGameTone(right, total, a, 480) == 0);
    }

    // Round rules (shared with the KPG1 reader and tools/game_pack.py).
    {
        GameRound r = BuiltinGameRound(0);
        assert(GameRoundValid(r));
        GameRound bad = r;
        bad.option_art[1] = bad.option_art[0];
        assert(!GameRoundValid(bad));  // the same picture twice
        bad = r;
        bad.correct = r.option_count;
        assert(!GameRoundValid(bad));
        bad = r;
        bad.option_art[0] = static_cast<uint16_t>(GameArt::Listen);
        assert(!GameRoundValid(bad));  // a question picture, not an answer
        bad = r;
        bad.kind = static_cast<GameKind>(15);
        assert(!GameRoundValid(bad));
        bad = r;
        bad.option_count = 2;  // third slot still filled
        assert(!GameRoundValid(bad));
        bad = r;
        bad.prompt_clip = 0;
        assert(!GameRoundValid(bad));
        bad = r;
        bad.question_art = 7;
        assert(!GameRoundValid(bad));
    }

    // Start: bad input changes nothing; round count is clamped.
    {
        GameSession s;
        GameOutput out{};
        assert(!s.Start(nullptr, 3, kBuiltinGameId, 1, 3, 0, &out));
        assert(!s.Start(pool.data(), 0, kBuiltinGameId, 1, 3, 0, &out));
        assert(!s.Start(pool.data(), pool.size(), 0, 1, 3, 0, &out));
        std::vector<GameRound> broken = pool;
        broken[2].correct = 7;
        assert(!s.Start(broken.data(), broken.size(), kBuiltinGameId, 1, 3, 0, &out));
        assert(!s.Active() && s.View().option_count == 0);
        assert(Started(pool, 5, 0).Result().rounds == 1);
        assert(Started(pool, 5, 9).Result().rounds == GameSession::kMaxRounds);
        assert(Started(std::vector<GameRound>(pool.begin(), pool.begin() + 2), 5, 3)
                   .Result()
                   .rounds == 2);
    }

    // Deterministic: same seed and inputs -> same rounds, order and outputs.
    {
        for (uint32_t seed : {1u, 7u, 0u, 0xDEADBEEFu}) {
            GameSession a = Started(pool, seed), b = Started(pool, seed);
            uint32_t now = 1000;
            for (int step = 0; step < 40 && a.Active(); ++step) {
                const GameInput in = step % 3 == 0 ? GameInput::Down
                                     : step % 3 == 1 ? GameInput::Press
                                                     : GameInput::Up;
                const auto oa = a.Input(in, now), ob = b.Input(in, now);
                assert(oa.redraw == ob.redraw && oa.clip == ob.clip && oa.right == ob.right &&
                       oa.finished == ob.finished);
                assert(a.View() == b.View());
                now += 4000;
                const auto ta = a.Tick(now), tb = b.Tick(now);
                assert(ta.clip == tb.clip && ta.finished == tb.finished && a.View() == b.View());
            }
        }
        // Different seeds pick different rounds or orders, and the right
        // answer is not always in the same place.
        std::set<uint16_t> firsts;
        std::set<uint8_t> positions;
        for (uint32_t seed = 1; seed <= 40; ++seed) {
            const GameSession s = Started(pool, seed);
            firsts.insert(s.View().question_art);
            positions.insert(s.Current().correct);
        }
        assert(firsts.size() > 2 && positions.size() > 1);
    }

    // tools/selftest/v4_mini_game.pettest relies on seed 4242 playing:
    // heart (third card), circle (second card), one block (third card).
    {
        GameSession s = Started(pool, 4242);
        const uint16_t expected[3][2] = {{static_cast<uint16_t>(GameArt::Heart), 2},
                                         {static_cast<uint16_t>(GameArt::Circle), 1},
                                         {static_cast<uint16_t>(GameArt::Blocks1), 2}};
        uint32_t now = 1000;
        for (const auto& round : expected) {
            assert(s.View().question_art == round[0] && s.Current().correct == round[1]);
            assert(s.Current().option_count == 3);
            MoveTo(s, s.Current().correct, now);
            assert(s.Input(GameInput::Press, now).right);
            now += GameSession::kRightMs;
            s.Tick(now);
        }
        assert(s.Result().end == GameEnd::Completed);
    }

    // Dial: up/down move (clamped) and say the option; nothing else changes.
    {
        GameSession s = Started(pool, 3);
        assert(s.View().highlight == 0 && s.View().round == 1 && s.View().rounds == 3);
        auto out = s.Input(GameInput::Up, 1100);
        assert(!out.redraw && out.clip == 0);  // already at the left end
        out = s.Input(GameInput::Down, 1200);
        assert(out.redraw && out.clip == s.Current().option_clip[1] && !out.right);
        assert(s.View().highlight == 1);
        for (int i = 0; i < 4; ++i) s.Input(GameInput::Down, 1300);
        assert(s.View().highlight == s.Current().option_count - 1);
        assert(s.Result().correct == 0);
    }

    // Right answer: the only reward, then the next round after a pause.
    {
        GameSession s = Started(pool, 11);
        const GameRound first = s.Current();
        MoveTo(s, first.correct, 1500);
        auto out = s.Input(GameInput::Press, 2000);
        assert(out.right && out.redraw && out.clip == kGameClipRight);
        assert(s.View().feedback == GameFeedback::Right && s.Result().correct == 1);
        // Pressing again while TuanTuan cheers wins nothing more.
        for (int i = 0; i < 3; ++i) {
            out = s.Input(GameInput::Press, 2100);
            assert(!out.right && !out.redraw && out.clip == 0);
            out = s.Input(GameInput::Down, 2100);
            assert(!out.redraw);
        }
        assert(s.Result().correct == 1);
        out = s.Tick(2000 + GameSession::kRightMs - 1);
        assert(!out.redraw && s.View().round == 1);
        out = s.Tick(2000 + GameSession::kRightMs);
        assert(out.redraw && out.clip == s.Current().prompt_clip && !out.finished);
        assert(s.View().round == 2 && s.View().highlight == 0 &&
               s.View().feedback == GameFeedback::None && s.Result().played == 1);
    }

    // Wrong: a gentle "try another", then TuanTuan shows the answer. No reward.
    {
        GameSession s = Started(pool, 21);
        const uint8_t wrong = Wrong(s);
        MoveTo(s, wrong, 1100);
        auto out = s.Input(GameInput::Press, 1200);
        assert(!out.right && out.redraw && out.clip == kGameClipTryAgain);
        assert(s.View().feedback == GameFeedback::TryAgain && s.Active());
        // Moving clears the question mark and says the option.
        out = s.Input(GameInput::Down, 1300);
        out = s.Input(GameInput::Up, 1400);
        assert(s.View().feedback == GameFeedback::None);
        MoveTo(s, wrong, 1500);
        out = s.Input(GameInput::Press, 1600);
        assert(!out.right && out.clip == kGameClipReveal);
        assert(s.View().feedback == GameFeedback::Reveal &&
               s.View().highlight == s.Current().correct && s.Result().correct == 0);
        assert(!s.Input(GameInput::Press, 1700).right);  // the shown answer is no reward
        assert(!s.Tick(1600 + GameSession::kRevealMs - 1).redraw);
        out = s.Tick(1600 + GameSession::kRevealMs);
        assert(out.redraw && s.View().round == 2 && s.Result().correct == 0 &&
               s.Result().played == 1);
    }

    // A whole game: three rounds, then home with a goodbye and the growth
    // memory event; going home early or idling never counts.
    {
        GameSession s = Started(pool, 99);
        uint32_t now = 1000;
        for (int round = 0; round < 3; ++round) {
            if (round == 1) {  // one round answered wrong twice
                MoveTo(s, Wrong(s), now);
                s.Input(GameInput::Press, now);
                s.Input(GameInput::Press, now);
                now += GameSession::kRevealMs;
            } else {
                MoveTo(s, s.Current().correct, now);
                assert(s.Input(GameInput::Press, now).right);
                now += GameSession::kRightMs;
            }
            const auto out = s.Tick(now);
            assert(out.finished == (round == 2));
            if (out.finished) assert(out.clip == kGameClipBye && !out.redraw);
        }
        assert(!s.Active());
        const GameResult& r = s.Result();
        assert(r.end == GameEnd::Completed && r.played == 3 && r.correct == 2 &&
               r.game_id == kBuiltinGameId && r.rounds == 3);
        assert(!s.Input(GameInput::Press, now).redraw && !s.Tick(now + 99999).finished);

        MemoryEvent event{};
        assert(!GameCompletedMemoryEvent(r, 0, 20000, &event));  // needs an id
        assert(GameCompletedMemoryEvent(r, 77, 20000, &event));
        assert(event.kind == MemoryEventKind::GameCompleted &&
               event.source == MemorySource::Device && event.option == kBuiltinGameId &&
               event.day == 20000 && event.id == 77);
        MemoryStore memory;
        assert(memory.Apply(event) == MemoryResult::Applied);
        assert(memory.GetGrowth().games_completed == 1 && memory.Today().games == 1 &&
               memory.Today().last_game == kBuiltinGameId);
        assert(memory.GetGrowth().milestones & MemoryStore::kFirstGame);
        // Never from the voice path, never a like (ADR-012).
        event.source = MemorySource::Voice;
        assert(memory.Apply(event) == MemoryResult::Rejected);
        assert(memory.PreferenceCount() == 0);

        // Completed without a single right answer: no growth entry.
        GameResult none = r;
        none.correct = 0;
        assert(!GameCompletedMemoryEvent(none, 78, 20000, &event));
    }
    {
        GameSession s = Started(pool, 5);
        MoveTo(s, s.Current().correct, 1000);
        assert(s.Input(GameInput::Press, 1000).right);
        const auto out = s.Input(GameInput::Long, 1100);  // long press: home now
        assert(out.finished && out.clip == kGameClipBye && !s.Active());
        assert(s.Result().end == GameEnd::Home);
        MemoryEvent event{};
        assert(!GameCompletedMemoryEvent(s.Result(), 9, 0, &event));
    }
    {
        GameSession s = Started(pool, 6, 3, 1000);
        // No input: the question once more after 12 s, home after 60 s.
        assert(s.Tick(1000 + GameSession::kRepeatPromptMs - 1).clip == 0);
        auto out = s.Tick(1000 + GameSession::kRepeatPromptMs);
        assert(out.clip == s.Current().prompt_clip && !out.redraw);
        assert(s.Tick(1000 + GameSession::kRepeatPromptMs + 10).clip == 0);  // once
        out = s.Tick(1000 + GameSession::kIdleExitMs);
        assert(out.finished && s.Result().end == GameEnd::Idle);
        MemoryEvent event{};
        assert(!GameCompletedMemoryEvent(s.Result(), 9, 0, &event));
        // Wrap-safe timestamps.
        GameSession w = Started(pool, 6, 3, 0xFFFFF000u);
        assert(!w.Tick(0x00000100u).finished);
        assert(w.Tick(0xFFFFF000u + GameSession::kIdleExitMs).finished);
    }
    {
        // Test-only self-test time scale: both "nobody plays" timers follow
        // it (50 %: question again after 6 s, home after 30 s), then real again.
        assert(SelfTestClock::SetForSelfTest(50));
        GameSession s = Started(pool, 6, 3, 1000);
        assert(s.Tick(1000 + GameSession::kRepeatPromptMs / 2 - 1).clip == 0);
        assert(s.Tick(1000 + GameSession::kRepeatPromptMs / 2).clip == s.Current().prompt_clip);
        assert(!s.Tick(1000 + GameSession::kIdleExitMs / 2 - 1).finished);
        assert(s.Tick(1000 + GameSession::kIdleExitMs / 2).finished);
        assert(SelfTestClock::SetForSelfTest(SelfTestClock::kRealPct));
        GameSession r = Started(pool, 6, 3, 1000);
        assert(r.Tick(1000 + GameSession::kRepeatPromptMs - 1).clip == 0);
        assert(!r.Tick(1000 + GameSession::kIdleExitMs - 1).finished);
    }
    {
        GameSession s = Started(pool, 8);
        s.Interrupt();  // sleep, voice, parent menu, Pet mode off
        assert(!s.Active() && s.Result().end == GameEnd::Interrupted);
        assert(!s.Input(GameInput::Press, 2000).redraw);
        s.Interrupt();
        assert(s.Result().end == GameEnd::Interrupted);
    }

    // Scene: highlight moves only touch the answer rail, TuanTuan's
    // reactions only the question area; nothing in the status corners.
    {
        GameSession s = Started(pool, 31);
        const SceneModel base = GameScene(s.View());
        SceneModel moved = base;
        moved.game.highlight = 1;
        int top = 0, bottom = 0;
        assert(DiffRows(base, moved, &top, &bottom) > 0);
        assert(top >= SceneComposer::kGameRailTop);
        for (GameFeedback fb : {GameFeedback::Right, GameFeedback::TryAgain, GameFeedback::Reveal}) {
            SceneModel reacted = base;
            reacted.game.feedback = fb;
            assert(DiffRows(base, reacted, &top, &bottom) > 0);
            assert(bottom < SceneComposer::kGameRailTop);
            MonoFrame f;
            SceneComposer::Render(reacted, f);
            for (int y = 0; y < SceneComposer::kStatusBottom; ++y)
                for (int x = 0; x < MonoFrame::kWidth; ++x)
                    assert(!SceneComposer::InStatusBox(x, y) || !f.IsBlack(x, y));
        }
        SceneModel next = base;
        next.game.round = 2;
        assert(DiffRows(base, next, &top, &bottom) > 0 && bottom < SceneComposer::kGameRailTop);

        // Every catalogue picture draws inside its card, for 2 and 3 cards.
        for (uint16_t art : kGameArtIds) {
            for (uint8_t n : {uint8_t{2}, uint8_t{3}}) {
                SceneModel card = GameScene(GameView{});
                card.game.option_count = n;
                card.game.highlight = 0;
                card.game.options = {art, 0, 0};
                if (n == 3) card.game.options[2] = 0;
                SceneModel empty = card;
                empty.game.options = {0, 0, 0};
                MonoFrame fc, fe;
                SceneComposer::Render(card, fc);
                SceneComposer::Render(empty, fe);
                const int card_x = n == 3 ? 40 : 150;  // card 0 of n
                int ink = 0;
                for (int y = 0; y < MonoFrame::kHeight; ++y)
                    for (int x = 0; x < MonoFrame::kWidth; ++x)
                        if (fc.IsBlack(x, y) != fe.IsBlack(x, y)) {
                            ++ink;
                            assert(x > card_x + 5 && x < card_x + 195 && y > 327 && y < 457);
                        }
                assert(ink > 200);
            }
            // As the question picture too.
            SceneModel q = GameScene(GameView{});
            q.game.question_art = art;
            int top_q = 0, bottom_q = 0;
            SceneModel listen = q;
            listen.game.question_art = 0;
            assert(DiffRows(q, listen, &top_q, &bottom_q) > 0 &&
                   bottom_q < SceneComposer::kGameRailTop);
        }
    }

    // Refresh: entering the game is a full refresh; highlight moves count
    // against the rail budget, reactions against the pet budget; the gate
    // sees every game change.
    {
        GameSession s = Started(pool, 41);
        RefreshPlanner planner;
        SceneModel home{};
        planner.Completed(home, planner.Plan(home), true);
        const SceneModel game = GameScene(s.View());
        assert(planner.Plan(game) == RefreshMode::Full);
        planner.Completed(game, RefreshMode::Full, true);
        SceneModel moved = game;
        moved.game.highlight = 1;
        assert(planner.Plan(moved) == RefreshMode::Partial);
        planner.Completed(moved, RefreshMode::Partial, true);
        assert(planner.RailPartials() == 1 && planner.PetPartials() == 0);
        SceneModel happy = moved;
        happy.game.feedback = GameFeedback::Right;
        planner.Completed(happy, planner.Plan(happy), true);
        assert(planner.RailPartials() == 1 && planner.PetPartials() == 1);
        assert(planner.Plan(home) == RefreshMode::Full);  // back home: clean picture

        PetRefreshGate gate(0);
        assert(gate.Request(game));
        SceneModel out{};
        assert(gate.Acquire(0, &out));
        gate.Complete(true, 0);
        assert(!gate.Request(game));
        assert(gate.Request(moved));
    }

    std::cout << "mini game tests passed\n";
    return 0;
}
