#pragma once

#include "kid_app/activity_list.h"
#include "kid_app/gesture_detector.h"
#include "kid_app/mini_game.h"
#include "kid_app/motion_stats.h"

#include <cstdint>

namespace kid_app {

// V4-07 motion games (PRD v0.2 §3/§7, ADR-011, docs/V4_MINI_GAMES.md):
// tilt the board to roll a ball into the hole, or hide TuanTuan face down
// and "find" her again. Only light, deliberate motion is rewarded; shaking
// is never a reward. Pure logic, single owner (the board's pet_input task),
// deterministic for a seed. Nothing is recorded: the IMU only gives window
// means that are thrown away.

// Game ids written into the growth memory (1 = built-in quiz set, V4-06).
// 2 and 3 are the V4-08 microphone games.
constexpr uint16_t kBallGameId = 4;
constexpr uint16_t kPeekabooGameId = 5;

// ---- TiltTracker: IMU window -> tilt events for the ball game ----
struct TiltConfig {
    int16_t tilt_mg = 300;       // |axis| beyond this rolls the ball (about 17 degrees)
    int16_t level_mg = 150;      // |axis| below this counts as level again
    int16_t min_up_z_mg = -500;  // screen must face roughly up (z ~ -983 mg)
    uint16_t max_dev_mg = 500;   // a shaking board is not a tilt
    // Which board axis is "left/right" for a child holding the board in
    // landscape, and its sign. To be calibrated on hardware (V4-07 trial).
    bool use_y = false;
    int8_t sign = 1;
    // V8 maze: also report TiltUp / TiltDown from the other axis (the larger
    // of the two tilts wins; Level needs both inside level_mg). `sign_v`
    // is +1 when a positive reading means "towards the child's bottom edge"
    // (TiltDown). Calibrate with `sign` on hardware.
    bool four_way = false;
    int8_t sign_v = 1;
};

// TiltRight/TiltLeft for every 500 ms window held past tilt_mg (the ball
// rolls one cell per window), Level once when it comes back. Single owner.
class TiltTracker {
public:
    explicit TiltTracker(TiltConfig config = {}) : config_(config) {}
    Gesture OnWindow(const MotionStats::Summary& window);
    void Reset() { level_ = true; }

private:
    TiltConfig config_;
    bool level_ = true;
};

// ---- Ball and peekaboo ----
enum class MotionKind : uint8_t { Ball = 1, Peekaboo = 2 };
const char* MotionKindName(MotionKind kind);

constexpr uint8_t kBallCells = 7;
constexpr uint8_t kBallStart = 3;

// One short motion game: kDefaultRounds rounds, then home.
//  Ball: each round a hole is at one end of the track (cells 0, 1, 5 or 6);
//   tilt rolls the ball one cell per tilt event (it never falls off the
//   ends); reaching the hole is the reward and ends the round.
//  Peekaboo: lay the board face down (TuanTuan hides), keep it there at
//   least kMinHideMs, turn it back up: "found you!" is the reward. Turning
//   back too early only says "hmm, again" (no reward, no penalty).
// A long press goes home; kIdleExitMs without any input or motion (or
// kHiddenMaxMs face down) ends the game. Shaking and everything else is
// ignored. Reuses GameOutput/GameResult/GameView from V4-06.
class MotionGameSession {
public:
    static constexpr uint8_t kDefaultRounds = 3;
    static constexpr uint32_t kRightMs = 2500;
    static constexpr uint32_t kMinHideMs = 2000;  // a real hold, not a flip
    static constexpr uint32_t kRepeatPromptMs = 12000;
    static constexpr uint32_t kIdleExitMs = 60000;
    static constexpr uint32_t kHiddenMaxMs = 120000;

    bool Start(MotionKind kind, uint32_t seed, uint8_t rounds, uint32_t now_ms, GameOutput* out);
    GameOutput Input(GameInput input, uint32_t now_ms);
    // Gesture::FaceDown/FaceUp (peekaboo), TiltLeft/TiltRight/Level (ball).
    GameOutput OnGesture(Gesture gesture, uint32_t now_ms);
    GameOutput Tick(uint32_t now_ms);
    void Interrupt();

    bool Active() const { return phase_ != Phase::Idle && phase_ != Phase::Done; }
    MotionKind Kind() const { return kind_; }
    GameView View() const;
    const GameResult& Result() const { return result_; }
    uint8_t Ball() const { return ball_; }
    uint8_t Hole() const { return hole_; }

private:
    enum class Phase : uint8_t { Idle, Playing, Hidden, Right, Done };
    static bool Reached(uint32_t now_ms, uint32_t at_ms) {
        return static_cast<int32_t>(now_ms - at_ms) >= 0;
    }
    uint32_t Next();
    void BeginRound(uint32_t now_ms, GameOutput* out);
    void Finish(GameEnd end, GameOutput* out);
    void Win(uint32_t now_ms, GameOutput* out);

    MotionKind kind_ = MotionKind::Ball;
    GameResult result_{};
    Phase phase_ = Phase::Idle;
    GameFeedback feedback_ = GameFeedback::None;
    uint8_t ball_ = kBallStart;
    uint8_t hole_ = 0;
    bool repeated_ = false;
    uint32_t rng_ = 1;
    uint32_t phase_until_ms_ = 0;
    uint32_t last_input_ms_ = 0;
    uint32_t hidden_since_ms_ = 0;
};

// ---- Game menu: which game? / which care? ----
// The home rail's Game item opens this on the Play group of the activity
// table (activity_list.h): the quiz (star), the tilt ball, peekaboo (cat),
// the V4-08 candles and the clap rhythm, then whatever later rows append.
// Three picture cards at a time (a window that slides with the highlight).
// Dial up/down to pick, press to start, long press home. Drawn by the V4-06
// answer-card scene. The V8 care menu is the same menu on the Care group.
// GameChoice now lives in activity_list.h.
constexpr uint8_t kGameChoiceCount = 5;  // the V4 built-in games (ids 1..5)
const char* GameChoiceName(GameChoice choice);

struct GameMenuOutput {
    bool redraw = false;
    uint16_t clip = 0;
    GameChoice chosen = GameChoice::None;  // set when a card was pressed
    bool finished = false;                 // home or idle: back to the home screen
};

class GameMenu {
public:
    static constexpr uint32_t kIdleExitMs = 60000;

    // start: the card highlighted first (0-based index in the group). A
    // group without rows does not open: finished (end Home), not Active().
    GameMenuOutput Start(uint32_t now_ms, uint8_t start = 0,
                         ActivityGroup group = ActivityGroup::Play);
    GameMenuOutput Input(GameInput input, uint32_t now_ms);
    GameMenuOutput Tick(uint32_t now_ms);
    void Interrupt();

    bool Active() const { return active_; }
    ActivityGroup Group() const { return group_; }
    GameEnd End() const { return end_; }
    GameView View() const;

private:
    uint8_t Count() const;
    uint8_t index_ = 0;  // 0-based card in group_
    ActivityGroup group_ = ActivityGroup::Play;
    bool active_ = false;
    GameEnd end_ = GameEnd::None;
    uint32_t last_input_ms_ = 0;
};

}  // namespace kid_app
