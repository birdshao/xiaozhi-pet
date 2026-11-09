// V9 1E (G4): the games ask for their spoken rule when they open, and say it
// once more after 12 s without input. Nothing else about the games changes:
// later rounds, option names and the V4 quiz keep their tones.
#include "kid_app/game_rules.h"
#include "kid_app/gesture_detector.h"
#include "kid_app/mini_game.h"
#include "kid_app/play_content.h"
#include "kid_app/play_game.h"

#include <cassert>
#include <iostream>
#include <set>
#include <string>

using namespace kid_app;

namespace {

void TestEveryPlayGameHasARule() {
    size_t count = 0;
    const PlayGameInfo* games = PlayGames(&count);
    std::set<std::string> texts;
    for (size_t i = 0; i < count; ++i) {
        assert(GameHasRule(games[i].id));
        assert(PlayGamePromptText(games[i].game)[0] != 0);
        texts.insert(PlayGamePromptText(games[i].game));
    }
    // 18 games, memory 4 and 6 share their words: 17 recordings.
    assert(count == 18 && texts.size() == 17);
}

void TestPlaySessionOpensWithTheRule() {
    for (PlayGame game : {PlayGame::Memory4, PlayGame::Memory6, PlayGame::Maze, PlayGame::Dice,
                          PlayGame::Tree, PlayGame::Statue, PlayGame::DayNight}) {
        PlaySession s;
        GameOutput out{};
        assert(s.Start(game, 77, 1000, &out));
        const uint16_t rule = GameRuleClip(FindPlayGame(game)->id);
        assert(rule != 0 && out.clip == rule);
        // Idle: the rule once more, only once.
        out = s.Tick(1000 + PlaySession::kRepeatPromptMs);
        assert(out.clip == rule);
        assert(s.Tick(1000 + PlaySession::kRepeatPromptMs + 10).clip == 0);
    }
}

void TestPlaySessionLaterRoundsKeepTheTone() {
    PlaySession s;
    GameOutput out{};
    assert(s.Start(PlayGame::Dice, 12, 1000, &out));
    uint32_t now = 1000;
    s.OnGesture(Gesture::HardShake, now += 10);
    const uint8_t die = s.Die();
    const uint16_t right_art = die >= 6 ? static_cast<uint16_t>(PlayArt::Dots6)
                                        : static_cast<uint16_t>(GameArt::Dots1) + die - 1;
    // Press the cards that are not the count twice: the answer is shown and
    // round 2 opens with the plain prompt tone, not the rule again.
    int presses = 0;
    for (uint8_t i = 0; i < 3 && presses < 2; ++i) {
        if (s.View().options[i] == right_art) continue;
        while (s.View().highlight < i) s.Input(GameInput::Down, now += 10);
        while (s.View().highlight > i) s.Input(GameInput::Up, now += 10);
        s.Input(GameInput::Press, now += 10);
        ++presses;
    }
    assert(presses == 2 && s.View().feedback == GameFeedback::Reveal);
    out = s.Tick(now + PlaySession::kRightMs);
    assert(s.Result().played == 1 && out.redraw);
    assert(out.clip == kGameTonePrompt);
}

void TestCardGameOpensWithTheRule() {
    size_t count = 0;
    const PlayGameInfo* games = PlayGames(&count);
    for (size_t i = 0; i < count; ++i) {
        const PlayGameInfo& info = games[i];
        if (!IsCardGame(info.game)) continue;
        size_t n = 0;
        const GameRound* pool = PlayBank(info.game, &n);
        GameRound clock[3];
        if (info.game == PlayGame::Clock) {
            n = BuildClockPool(true, 9, 0, clock);
            pool = clock;
        } else if (info.game == PlayGame::Weather) {
            n = BuildWeatherPool(true, 220, clock);
            pool = clock;
        }
        GameSession s;
        GameOutput out{};
        assert(s.Start(pool, n, info.id, 4242, GameSession::kDefaultRounds, 1000, &out));
        const uint16_t rule = GameRuleClip(info.id);
        assert(out.clip == rule);
        // 12 s without input: the rule again, once.
        out = s.Tick(1000 + GameSession::kRepeatPromptMs);
        assert(out.clip == rule);
        assert(s.Tick(1000 + GameSession::kRepeatPromptMs + 5).clip == 0);
        // Answer round 1 (any press), wait for round 2: tone prompt again.
        GameSession t;
        assert(t.Start(pool, n, info.id, 4242, GameSession::kDefaultRounds, 1000, &out));
        uint32_t now = 1000;
        for (int k = 0; k < 4 && t.Result().played == 0; ++k) {
            uint8_t guard = 0;
            while (t.Current().correct != 0 && guard++ < 3) t.Input(GameInput::Down, now += 50);
            t.Input(GameInput::Press, now += 50);
        }
        out = t.Tick(now + GameSession::kRevealMs + GameSession::kRightMs);
        assert(t.Result().played == 1 && out.clip == t.Current().prompt_clip);
        assert(out.clip == kGameTonePrompt);
    }
}

void TestOtherGamesKeepTheirTones() {
    // The V4 built-in quiz (id 1) and a pack game (content id >= 100).
    for (uint16_t id : {kBuiltinGameId, uint16_t{201}}) {
        GameSession s;
        GameOutput out{};
        assert(s.Start(BuiltinGameRounds(), BuiltinGameRoundCount(), id, 4242,
                       GameSession::kDefaultRounds, 1000, &out));
        assert(out.clip == kGameTonePrompt);
        assert(s.Tick(1000 + GameSession::kRepeatPromptMs).clip == kGameTonePrompt);
    }
}

}  // namespace

int main() {
    TestEveryPlayGameHasARule();
    TestPlaySessionOpensWithTheRule();
    TestPlaySessionLaterRoundsKeepTheTone();
    TestCardGameOpensWithTheRule();
    TestOtherGamesKeepTheirTones();
    std::cout << "game rule clips ok\n";
    return 0;
}
