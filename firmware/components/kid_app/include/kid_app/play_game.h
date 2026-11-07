#pragma once

#include "kid_app/gesture_detector.h"
#include "kid_app/icon_choice.h"
#include "kid_app/mini_game.h"
#include "kid_app/motion_stats.h"
#include "kid_app/play_content.h"

#include <array>
#include <cstdint>

namespace kid_app {

// V8 non-card mini games (docs/V8_MINI_GAMES.md): memory flip, tilt maze,
// shake the dice / the tree, wooden man, day and night. Pure logic, single
// owner (the board's pet_input task), deterministic for a seed: the same
// seed and inputs always give the same cards, maze, die values and outputs.
// Nothing is recorded: the IMU only gives 500 ms window summaries.
//
// Rewards (ADR-012): in the picture games (memory flip, dice, tree) only a
// correct dial press makes TuanTuan happy (`GameOutput::right`). The motion
// goals (reaching the carrot, holding still 3 s, flipping on the cue) use
// the same "only light, deliberate motion" rule as the V4-07 ball: shaking,
// jerking and everything the game did not ask for is ignored. Going home
// early, an idle timeout or an interruption never writes a growth memory
// (GameCompletedMemoryEvent: finished + at least one correct).
class PlaySession {
public:
    // Tuning, documented in docs/V8_MINI_GAMES.md.
    static constexpr uint32_t kRightMs = 2500;      // happy TuanTuan before the next step
    static constexpr uint32_t kMismatchMs = 1500;   // two different cards stay up this long
    static constexpr uint32_t kRepeatPromptMs = 12000;
    static constexpr uint32_t kIdleExitMs = 60000;
    // Maze: two mazes of growing size, one step per tilt (level in between).
    static constexpr uint8_t kMazeRounds = 2;
    // Shake games and the others: three rounds.
    static constexpr uint8_t kDefaultRounds = 3;
    static constexpr uint8_t kMaxTries = 2;
    // Wooden man (500 ms windows): "dance" until kMoveWindows moving windows
    // were seen, then freeze for kFreezeWindows still windows in a row (3 s).
    // A window is moving at max_dev >= kMoveDevMg (a lift measured 230-240),
    // still at max_dev <= kStillDevMg (a resting board measured 16-19).
    static constexpr uint16_t kMoveDevMg = 150;
    static constexpr uint16_t kStillDevMg = 60;
    static constexpr uint8_t kMoveWindows = 3;
    static constexpr uint8_t kFreezeWindows = 6;
    static constexpr uint16_t kMinWindowSamples = 5;
    // Day and night: stay face down this long before "day" is called.
    static constexpr uint32_t kNightMinMs = 3000, kNightMaxMs = 5000;
    static constexpr uint32_t kNightHiddenMaxMs = 120000;  // face down, never flipped

    bool Start(PlayGame game, uint32_t seed, uint32_t now_ms, GameOutput* out);
    GameOutput Input(GameInput input, uint32_t now_ms);
    // GentleShake / HardShake (dice, tree), FaceDown / FaceUp (day and night),
    // TiltLeft / TiltRight / TiltUp / TiltDown / Level (maze).
    GameOutput OnGesture(Gesture gesture, uint32_t now_ms);
    // One 500 ms IMU window (wooden man).
    GameOutput OnWindow(const MotionStats::Summary& window, uint32_t now_ms);
    GameOutput Tick(uint32_t now_ms);
    void Interrupt();

    bool Active() const { return phase_ != Phase::Idle && phase_ != Phase::Done; }
    PlayGame Game() const { return game_; }
    GameView View() const;
    const GameResult& Result() const { return result_; }
    // The board feeds IMU windows / gestures only while one of these is true.
    bool WantsWindows() const { return Active() && game_ == PlayGame::Statue; }
    bool WantsTilt() const { return Active() && game_ == PlayGame::Maze; }
    bool WantsShake() const {
        return Active() && (game_ == PlayGame::Dice || game_ == PlayGame::Tree);
    }
    bool WantsFace() const { return Active() && game_ == PlayGame::DayNight; }

    // Test and board helpers.
    uint8_t Pos() const { return pos_; }
    uint8_t Goal() const { return goal_; }
    uint8_t Cols() const { return cols_; }
    uint8_t Rows() const { return rows_; }
    // Open-wall bits (1 up, 2 right, 4 down, 8 left) of a cell.
    uint8_t Walls(uint8_t cell) const { return cell < cells_.size() ? cells_[cell] : 0; }
    uint8_t Die() const { return die_; }

private:
    enum class Phase : uint8_t {
        Idle, Playing, Mismatch, Right, Waiting, Asking, Reveal, Dance, Freeze, Night, DayCue, Done
    };
    static bool Reached(uint32_t now_ms, uint32_t at_ms) {
        return static_cast<int32_t>(now_ms - at_ms) >= 0;
    }
    uint32_t Next();
    void BeginRound(uint32_t now_ms, GameOutput* out);
    void Finish(GameEnd end, GameOutput* out);
    void Win(uint32_t now_ms, GameOutput* out);   // a correct step: reward, then Right
    void NextRound(uint32_t now_ms, GameOutput* out);
    void BuildMaze();
    void BeginAsk(uint8_t count, uint32_t now_ms, GameOutput* out);  // dice / tree options
    GameOutput FlipCard(uint32_t now_ms);
    bool Selectable(uint8_t card) const;

    PlayGame game_ = PlayGame::None;
    GameResult result_{};
    Phase phase_ = Phase::Idle;
    GameFeedback feedback_ = GameFeedback::None;
    uint32_t rng_ = 1;
    uint32_t phase_until_ms_ = 0;
    uint32_t last_input_ms_ = 0;
    bool repeated_ = false;

    // Memory flip
    uint8_t cols_ = 0, rows_ = 0;
    std::array<uint8_t, 24> cells_{};  // memory: face | 0x80 up | 0x40 matched; maze: walls
    uint8_t cursor_ = 0;
    int8_t first_ = -1, second_ = -1;
    // Maze
    uint8_t pos_ = 0, goal_ = 0;
    bool armed_ = true;  // a tilt only counts after the board was level again
    // Dice / tree
    IconChoice choice_;
    std::array<uint16_t, 3> options_{};
    uint8_t answer_ = 0;  // 0-based right card
    uint8_t tries_ = 0;
    uint8_t die_ = 0;     // 1..6 dots / 1..5 fruit currently shown, 0 = not shaken yet
    // Wooden man
    uint8_t move_windows_ = 0, still_windows_ = 0;
    bool window_seen_ = false;
    // Day and night
    uint32_t night_since_ms_ = 0;
    uint32_t night_for_ms_ = 0;
};

}  // namespace kid_app
