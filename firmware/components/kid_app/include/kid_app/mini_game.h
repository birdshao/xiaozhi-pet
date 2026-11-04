#pragma once

#include "kid_app/game_view.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace kid_app {

struct MemoryEvent;

// V4-06 rule-based offline mini games (PRD v0.2 §7, docs/V4_MINI_GAMES.md).
// Every round is reviewed content: from a KPG1 game pack on the TF card or
// from the small built-in set below. Nothing is improvised.
enum class GameKind : uint8_t {
    MatchShape = 1,   // find the same shape
    Count = 2,        // how many? match the number of things
    AnimalSound = 3,  // which animal makes this sound?
    BigSmall = 4,     // which one is big / small?
    EnglishWord = 5,  // English word card: which picture is "cat"?
};
constexpr bool GameKindValid(uint8_t kind) { return kind >= 1 && kind <= 5; }
const char* GameKindName(GameKind kind);

// Clip ids, shared with tools/game_pack.py. 1..15: session lines a pack may
// record (missing ones fall back to a built-in tone); 16..999: round clips
// in a pack; kGameToneBase + art: built-in tones (no recording needed).
constexpr uint16_t kGameClipRight = 2;     // "you got it!"
constexpr uint16_t kGameClipTryAgain = 3;  // "hmm, try another one"
constexpr uint16_t kGameClipReveal = 4;    // "it's this one"
constexpr uint16_t kGameClipBye = 5;       // "that was fun"
constexpr uint16_t kGameSessionClipMax = 15;
constexpr uint16_t kGameRoundClipMin = 16, kGameRoundClipMax = 999;
constexpr uint16_t kGameToneBase = 0x4000;
constexpr uint16_t kGameTonePrompt = kGameToneBase;  // "look and listen"
constexpr uint16_t GameArtTone(uint16_t art) {
    return static_cast<uint16_t>(kGameToneBase + art);
}

struct GameRound {
    GameKind kind = GameKind::MatchShape;
    uint8_t option_count = 0;  // 2..3
    uint8_t correct = 0;       // 0-based right answer
    uint16_t question_art = 0;  // GameArt; 0 = Listen
    std::array<uint16_t, 3> option_art{};
    uint16_t prompt_clip = 0;
    std::array<uint16_t, 3> option_clip{};
};

// Same rules as the KPG1 reader and tools/game_pack.py.
bool GameRoundValid(const GameRound& round);

// Built-in set: works without a TF card. Wordless picture matching (same
// shape, same number) with generated tones, so it needs no recordings.
constexpr uint16_t kBuiltinGameId = 1;
size_t BuiltinGameRoundCount();
const GameRound& BuiltinGameRound(size_t index);
const GameRound* BuiltinGameRounds();  // BuiltinGameRoundCount() in a row

// Built-in tones for the clip ids above (deterministic, nothing in flash).
size_t GameToneSamples(uint16_t clip);
size_t RenderGameTone(uint16_t clip, size_t offset, int16_t* out, size_t max);

enum class GameInput : uint8_t { Up, Down, Press, Long };
enum class GameEnd : uint8_t { None, Completed, Home, Idle, Interrupted };
const char* GameEndName(GameEnd end);

// What the board does after Start / Input / Tick.
struct GameOutput {
    bool redraw = false;   // publish View()
    uint16_t clip = 0;     // play this clip now (0 = nothing new)
    bool right = false;    // a correct dial answer was just given
    bool finished = false;  // session over: back to the home screen
};

struct GameResult {
    uint16_t game_id = 0;
    uint8_t rounds = 0;   // rounds in the session
    uint8_t played = 0;   // rounds finished (answered or shown)
    uint8_t correct = 0;  // rounds answered right with the dial
    GameEnd end = GameEnd::None;
};

// One short game: a few rounds drawn from the pool, then home. Pure logic,
// single owner (the board's pet_input task), deterministic for a seed: the
// same pool, seed and inputs always give the same rounds, option order and
// outputs. Up/down move the highlight and say the option's name; a press
// answers. Right: TuanTuan is happy (the only reward), then the next round.
// Wrong: a gentle "try another"; after kMaxTries wrong presses TuanTuan
// shows the answer and the game moves on without a reward. A long press
// goes home at once; kIdleExitMs without input ends the game too.
class GameSession {
public:
    static constexpr uint8_t kDefaultRounds = 3;
    static constexpr uint8_t kMaxRounds = 5;
    static constexpr size_t kMaxPool = 64;
    static constexpr uint8_t kMaxTries = 2;
    static constexpr uint32_t kRightMs = 2500;      // happy TuanTuan, then next round
    static constexpr uint32_t kRevealMs = 3500;     // the answer stays this long
    static constexpr uint32_t kRepeatPromptMs = 12000;  // once per round, no input
    static constexpr uint32_t kIdleExitMs = 60000;

    // `pool` must outlive the session. False (and nothing changes) for an
    // empty pool or a round that is not GameRoundValid.
    bool Start(const GameRound* pool, size_t pool_size, uint16_t game_id, uint32_t seed,
               uint8_t rounds, uint32_t now_ms, GameOutput* out);
    GameOutput Input(GameInput input, uint32_t now_ms);
    GameOutput Tick(uint32_t now_ms);
    // The board ends the game (sleep, voice, parent menu, Pet mode off).
    void Interrupt();

    bool Active() const { return phase_ != Phase::Idle && phase_ != Phase::Done; }
    GameView View() const;
    // The round on screen, options in the order shown.
    const GameRound& Current() const { return current_; }
    const GameResult& Result() const { return result_; }

private:
    enum class Phase : uint8_t { Idle, Asking, Right, Reveal, Done };
    static bool Reached(uint32_t now_ms, uint32_t at_ms) {
        return static_cast<int32_t>(now_ms - at_ms) >= 0;
    }
    uint32_t Next();
    void BeginRound(uint32_t now_ms, GameOutput* out);
    void Finish(GameEnd end, GameOutput* out);

    const GameRound* pool_ = nullptr;
    std::array<uint8_t, kMaxRounds> order_{};
    GameRound current_{};
    GameResult result_{};
    Phase phase_ = Phase::Idle;
    GameFeedback feedback_ = GameFeedback::None;
    uint8_t highlight_ = 0;
    uint8_t tries_ = 0;
    bool repeated_ = false;
    uint32_t rng_ = 1;
    uint32_t phase_until_ms_ = 0;
    uint32_t last_input_ms_ = 0;
};

// GAME_COMPLETED for the growth memory (ADR-012): only a finished session
// with at least one correct dial answer counts; going home early, an idle
// timeout or an interruption never does. option = game id (pack content id,
// kBuiltinGameId for the built-in set), source Device (trusted rules).
bool GameCompletedMemoryEvent(const GameResult& result, uint64_t event_id, uint32_t day,
                              MemoryEvent* out);

}  // namespace kid_app
