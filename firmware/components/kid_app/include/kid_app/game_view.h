#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace kid_app {

// V4-06 mini games: the built-in 1-bit picture catalogue. Game packs
// (KPG1, tools/game_pack.py ART) name pictures by these ids; the firmware
// draws them with the scene primitives, so every answer card has the same
// size and position and a highlight change is a small partial refresh.
// Ids are stable: append only, never renumber or reuse.
enum class GameArt : uint16_t {
    None = 0,
    Circle = 1, Square = 2, Triangle = 3, Star = 4, Heart = 5,
    Dots1 = 11, Dots2 = 12, Dots3 = 13, Dots4 = 14, Dots5 = 15,
    Blocks1 = 21, Blocks2 = 22, Blocks3 = 23, Blocks4 = 24, Blocks5 = 25,
    BallBig = 31, BallSmall = 32,
    Cat = 41, Dog = 42, Bird = 43, Fish = 44,
    Listen = 90,  // question card only: "listen to the sound"
};

constexpr std::array<uint16_t, 21> kGameArtIds = {
    1, 2, 3, 4, 5, 11, 12, 13, 14, 15, 21, 22, 23, 24, 25, 31, 32, 41, 42, 43, 44};
// kGameArtIds lists the answer pictures; Listen may only be a question.
constexpr bool GameArtIsOption(uint16_t id) {
    for (uint16_t known : kGameArtIds)
        if (known == id) return true;
    return false;
}
constexpr bool GameArtIsQuestion(uint16_t id) {
    return id == static_cast<uint16_t>(GameArt::Listen) || GameArtIsOption(id);
}

// How TuanTuan reacts in the question area. Right is the only reward
// (hearts, happy face) and only follows a correct dial answer.
enum class GameFeedback : uint8_t { None, Right, TryAgain, Reveal };

// SceneKind::Game contents (not packed into the scene word, like the
// parent menu). The question area (pet, question card, progress dots) is
// above SceneComposer::kGameRailTop, the answer cards and the highlight
// below it.
struct GameView {
    uint8_t round = 0;         // 1-based round of this session
    uint8_t rounds = 0;        // rounds in the session (progress dots)
    uint8_t option_count = 0;  // 2..3 answer cards
    uint8_t highlight = 0;     // 0-based card under the dial
    GameFeedback feedback = GameFeedback::None;
    uint16_t question_art = 0;  // GameArt id; 0 = Listen
    std::array<uint16_t, 3> options{};

    // Changes drawn below kGameRailTop.
    bool RailDiffers(const GameView& o) const {
        return option_count != o.option_count || highlight != o.highlight || options != o.options;
    }
    // Changes drawn above it.
    bool QuestionDiffers(const GameView& o) const {
        return round != o.round || rounds != o.rounds || feedback != o.feedback ||
               question_art != o.question_art;
    }
    bool operator==(const GameView& o) const { return !RailDiffers(o) && !QuestionDiffers(o); }
    bool operator!=(const GameView& o) const { return !(*this == o); }
};

}  // namespace kid_app
