#pragma once

#include "kid_app/activity_list.h"
#include "kid_app/mini_game.h"

#include <cstddef>
#include <cstdint>

namespace kid_app {

// V8 mini games (docs/V8_MINI_GAMES.md): the list of games under the home
// rail's Game item, the built-in question banks of the picture-card games,
// and the live rounds (clock from the RTC, clothes from the SHTC3).
//
// Every card game is a GameSession over a bank of GameRounds, so the rules
// are the V4-06 ones: the dial picks, only a correct press makes TuanTuan
// happy, a session that finishes with at least one correct answer writes
// GAME_COMPLETED (source Device). Banks are deterministic tables; the order
// and the card positions of a session come from the caller's seed.
enum class PlayGame : uint8_t {
    None = 0,
    Memory4 = 1, Memory6, Maze, Dice, Tree, Statue, DayNight,  // PlaySession
    Shapes, Counting,                                           // V4-06 banks, 20+ rounds
    SpotDiff, MamaBaby, SizeOrder, Pattern, Classify, Shadow, LeftRight, Clock, Weather,
};
constexpr uint8_t kPlayGameCount = 18;

// Menu card id of a game (activity_list.h GameChoice 13..30 = 12 + PlayGame).
constexpr GameChoice GameChoiceOf(PlayGame game) {
    return static_cast<GameChoice>(12 + static_cast<uint8_t>(game));
}
constexpr bool GameChoiceIsPlay(GameChoice c) {
    return c >= GameChoice::Memory4 && c <= GameChoice::Weather;
}
constexpr PlayGame PlayGameOf(GameChoice c) {
    return GameChoiceIsPlay(c) ? static_cast<PlayGame>(static_cast<uint8_t>(c) - 12)
                               : PlayGame::None;
}

struct PlayGameInfo {
    PlayGame game;
    uint16_t id;          // growth memory id (GAME_COMPLETED option); 1..5 are older games
    const char* name;     // log name
    uint8_t min_age;      // suggested from this age (3..5)
    uint8_t level;        // 1 easy .. 3 hard inside the age
    bool needs_motion;    // IMU game: the parent's motion switch applies
    GameKind kind;        // card games: the round kind of the bank (else MatchShape)
};

// All games in menu order: age, then level (ties keep the table order).
const PlayGameInfo* PlayGames(size_t* count);
const PlayGameInfo* FindPlayGame(PlayGame game);
bool IsCardGame(PlayGame game);  // Shapes .. Weather: GameSession over a bank

// Bank of a card game (always >= 20 rounds, <= GameSession::kMaxPool).
// nullptr for the PlaySession games.
const GameRound* PlayBank(PlayGame game, size_t* count);

// Clock: the face shows hour24:minute rounded down to the hour or half hour;
// when the clock is not set (`clock_ok` false) the first round is a fixed
// 8 o'clock one. Fills 3 rounds (the first is "now"), returns 3.
size_t BuildClockPool(bool clock_ok, uint8_t hour24, uint8_t minute, GameRound out[3]);
// What the clock round answers: PlayArt::Eat / Sleep / Play / Bath.
PlayArt ClockActivity(uint8_t hour24, bool half);

// Weather: temp_c10 is the room temperature (0.1 C, already minus the board
// heating, as EnvTipPolicy uses). Hot >= 26.0 C, cold <= 18.0 C; in between,
// or with `sensor_ok` false, the first round is a fixed one (rain).
constexpr int kWeatherHotC10 = 260;
constexpr int kWeatherColdC10 = 180;
size_t BuildWeatherPool(bool sensor_ok, int temp_c10, GameRound out[3]);

// Prompt wording for the reviewers (and the TTS tool): the text a grown-up
// would say for a game's question. The firmware plays tones until a
// reviewed recording exists (TODO in the doc); nothing here is shown.
const char* PlayGamePromptText(PlayGame game);

}  // namespace kid_app
