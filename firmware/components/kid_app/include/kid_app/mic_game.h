#pragma once

#include "kid_app/game_view.h"
#include "kid_app/mic_envelope.h"
#include "kid_app/mini_game.h"

#include <array>
#include <cstdint>
#include <cstdlib>

namespace kid_app {

// V4-08 microphone games (blow out the candles, clap the rhythm back),
// built on the V4-06 mini-game frame: the same GameView scene, the same
// GameOutput / GameResult / GameEnd, the same reward rules (ADR-012: one
// reward, only for a right answer; growth memory only for a finished game
// with at least one success).
//
// PRIVACY: the session sees only MicDecision (blowing / clap) from
// MicClassifier, which itself only sees an RMS number per 10 ms. No audio
// is recorded, stored or sent (see mic_envelope.h).
//
// The microphone is only wanted while a game really listens (MicWanted()):
// not while a tone or the demo beats play (the speaker would be heard), and
// never in a voice session (the board ends the game and disarms the tap).
enum class MicGameKind : uint8_t { Candle = 1, ClapRhythm = 2 };
const char* MicGameKindName(MicGameKind kind);

// GAME_COMPLETED option ids (1 = built-in quiz, packs use 100+).
constexpr uint16_t kMicCandleGameId = 2;
constexpr uint16_t kMicClapGameId = 3;

// Existing built-in tones (mini_game.cc), so no new sound data.
constexpr uint16_t kMicClipStart = kGameTonePrompt;
constexpr uint16_t kMicClipOut = GameArtTone(static_cast<uint16_t>(GameArt::Triangle));
constexpr uint16_t kMicClipBeat = GameArtTone(static_cast<uint16_t>(GameArt::Circle));
constexpr uint16_t kMicClipYourTurn = GameArtTone(static_cast<uint16_t>(GameArt::Listen));

// Clap rhythm patterns: three claps, the two gaps between them in ms.
constexpr uint8_t kMicClapRounds = 2;
constexpr uint8_t kMicClapsPerRound = 3;
constexpr std::array<std::array<uint16_t, 2>, kMicClapRounds> kMicClapGaps = {{{700, 700}, {500, 1000}}};

class MicGameSession {
public:
    static constexpr uint8_t kCandles = 3;
    static constexpr uint32_t kBlowMs = 700;          // steady blowing per candle
    static constexpr uint32_t kEchoGuardMs = 700;     // deaf after a tone (speaker)
    static constexpr uint32_t kRightMs = 2500;
    static constexpr uint32_t kRetryMs = 2500;
    static constexpr uint32_t kIdleExitMs = 30000;    // candles: nobody blows
    static constexpr uint32_t kIntroMs = 1500;        // clap: before the first demo beat
    static constexpr uint32_t kTurnGapMs = 900;       // last beat -> "your turn"
    static constexpr uint32_t kTurnToneMs = 400;      // the "your turn" tone plays: deaf
    static constexpr uint32_t kWarmMs = 400;          // microphone open, learning the room
    static constexpr uint32_t kListenMs = 7000;       // then this long to clap it back
    static constexpr uint32_t kStallMs = 2500;        // no clap for this long after one: wrong
    static constexpr uint32_t kClapToleranceMs = 250;
    static constexpr uint8_t kMaxTries = 2;

    // False (nothing changes) for an unknown kind.
    bool Start(MicGameKind kind, uint32_t now_ms, GameOutput* out);
    GameOutput Input(GameInput input, uint32_t now_ms);  // only Long (home) does anything
    // Call often (about every 20 ms) with what the classifier decided since the
    // last call; ignored unless MicWanted().
    GameOutput Tick(uint32_t now_ms, const MicDecision& mic);
    void Interrupt();

    bool Active() const { return phase_ != Phase::Idle && phase_ != Phase::Done; }
    // Arm the microphone tap only while this is true (clap rhythm: the classifier
    // gets kWarmMs to learn the room before listening starts).
    bool MicWanted() const {
        return kind_ == MicGameKind::Candle ? phase_ == Phase::Asking
                                            : phase_ == Phase::Arm || phase_ == Phase::Listen;
    }
    MicGameKind Kind() const { return kind_; }
    GameView View() const;
    const GameResult& Result() const { return result_; }
    uint8_t LitCandles() const { return lit_; }
    uint8_t ClapsHeard() const { return claps_; }
    uint8_t Round() const { return round_; }

private:
    enum class Phase : uint8_t { Idle, Asking, Intro, Demo, Turn, Quiet, Arm, Listen, Right, Retry, Done };
    static bool Reached(uint32_t now_ms, uint32_t at_ms) {
        return static_cast<int32_t>(now_ms - at_ms) >= 0;
    }
    void Finish(GameEnd end, GameOutput* out);
    void BeginClapRound(uint32_t now_ms, GameOutput* out);
    GameOutput TickCandle(uint32_t now_ms, const MicDecision& mic);
    GameOutput TickClap(uint32_t now_ms, const MicDecision& mic);
    void ClapFailed(uint32_t now_ms, GameOutput* out);

    MicGameKind kind_ = MicGameKind::Candle;
    Phase phase_ = Phase::Idle;
    GameFeedback feedback_ = GameFeedback::None;
    GameResult result_{};
    uint8_t lit_ = 0;
    uint8_t round_ = 0;  // clap: 0-based round
    uint8_t beat_ = 0;
    uint8_t claps_ = 0;
    uint8_t tries_ = 0;
    std::array<uint32_t, kMicClapsPerRound> clap_at_{};
    uint32_t blow_ms_ = 0;
    uint32_t last_tick_ms_ = 0;
    uint32_t guard_until_ms_ = 0;
    uint32_t next_ms_ = 0;      // next scheduled step (demo beat, listen start, ...)
    uint32_t listen_ms_ = 0;    // when listening opened
    uint32_t last_activity_ms_ = 0;
};

}  // namespace kid_app
