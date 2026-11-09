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

// Sized shapes (shape 0..4 = circle square triangle star heart, size 0..2 =
// small medium large) are catalogue pictures too: KPG1 "big_small" rounds
// name them (tools/game_pack.py ART circle_small ... heart_large).
// A shape (0..4) drawn small / medium / large (size 0..2).
constexpr uint16_t PlaySizedArt(uint8_t shape, uint8_t size) {
    return static_cast<uint16_t>(0x3000 | (shape << 2) | size);
}
constexpr bool PlaySizedArtValid(uint16_t id) {
    return id >= 0x3000 && id < 0x3100 && (id & 3) < 3 && ((id >> 2) & 63) < 5;
}

constexpr std::array<uint16_t, 21> kGameArtIds = {
    1, 2, 3, 4, 5, 11, 12, 13, 14, 15, 21, 22, 23, 24, 25, 31, 32, 41, 42, 43, 44};
// kGameArtIds lists the answer pictures; Listen may only be a question.
constexpr bool GameArtIsOption(uint16_t id) {
    for (uint16_t known : kGameArtIds)
        if (known == id) return true;
    return PlaySizedArtValid(id);
}
constexpr bool GameArtIsQuestion(uint16_t id) {
    return id == static_cast<uint16_t>(GameArt::Listen) || GameArtIsOption(id);
}

// V4-08 microphone games: pictures only they use (candles, the clapping
// hand, the blowing face). Not in kGameArtIds and not for KPG1 packs, which
// stay limited to the catalogue above; same append-only rule.
enum class MicArt : uint16_t {
    CandleLit = 101, CandleOut = 102, Hand = 103, HandHit = 104, Blow = 105,
};
constexpr bool MicArtValid(uint16_t id) { return id >= 101 && id <= 105; }
// V8 mini games (docs/V8_MINI_GAMES.md): pictures for the new learning
// rounds and motion games. Not in kGameArtIds and not for KPG1 packs (a
// pack may only name the catalogue above); append only, never renumber.
// Simple pictures are 401..599 (601.. are the care pictures, CareArt; 201..204 the
// menu pages, MenuArt). Composite pictures encode their parts in
// the id (see the PlayArt* helpers) so a round stays four small numbers:
//   0x2000 pattern strip   0x2800 spot-the-difference pair
//   0x3000 sized shape     0x3100 size-order strip
//   0x3200 clock face      0x3300 shadow of a catalogue picture
//   0x3500 tree with fruit  0x3600 arrow + shape  0x3700 shape in a box
//   0x3800 die face (1..6)
enum class PlayArt : uint16_t {
    Apple = 401, Carrot = 402, Cake = 403, Bread = 404, Shoe = 405, Hat = 406, Cup = 407,
    Rabbit = 408, Duck = 409,
    BabyCat = 411, BabyDog = 412, BabyBird = 413, BabyFish = 414, BabyRabbit = 415,
    BabyDuck = 416,
    EatYes = 421, EatNo = 422,
    Eat = 461, Sleep = 462, Play = 463, Bath = 464,
    Tshirt = 471, Scarf = 472, Umbrella = 473, Gloves = 474, Sunglasses = 475,
    Sun = 481, Snow = 482, Rain = 483, ThermoHot = 484, ThermoCold = 485,
    Dots6 = 491, ShakeMe = 495, Tree = 497,
    Pos1 = 501, Pos2 = 502, Pos3 = 503,
    Statue = 511, Dance = 512, Moon = 513,
};
constexpr bool PlayArtSimple(uint16_t id) {
    return (id >= 401 && id <= 409) || (id >= 411 && id <= 416) || id == 421 || id == 422 ||
           (id >= 461 && id <= 464) || (id >= 471 && id <= 475) || (id >= 481 && id <= 485) || id == 491 || id == 495 ||
           id == 497 || (id >= 501 && id <= 503) || (id >= 511 && id <= 513);
}
// Shapes used inside composites: 0 circle 1 square 2 triangle 3 star 4 heart.
constexpr uint16_t kPlayShapeArt[5] = {1, 2, 3, 4, 5};
// Pattern strip: type 0 = ABAB?, 1 = AABAAB?, 2 = ABCABC?, a/b/c shape 0..4.
constexpr uint16_t PlayPatternArt(uint8_t type, uint8_t a, uint8_t b, uint8_t c) {
    return static_cast<uint16_t>(0x2000 | (type << 9) | (a << 6) | (b << 3) | c);
}
constexpr bool PlayPatternArtValid(uint16_t id) { return id >= 0x2000 && id < 0x2800; }
// Spot-the-difference: three shapes (0..3) on top; the copy below differs in
// cell `idx` (0..2), where shape (top + 1 + delta) % 4 (delta 0..2) is drawn.
constexpr uint16_t PlaySpotArt(uint8_t s0, uint8_t s1, uint8_t s2, uint8_t idx, uint8_t delta) {
    return static_cast<uint16_t>(0x2800 | (s0 << 8) | (s1 << 6) | (s2 << 4) | (idx << 2) | delta);
}
constexpr bool PlaySpotArtValid(uint16_t id) { return id >= 0x2800 && id < 0x3000; }
// Three sizes in a row (ascending or descending) with one cell hidden by "?".
constexpr uint16_t PlayRankArt(uint8_t shape, bool descending, uint8_t hidden) {
    return static_cast<uint16_t>(0x3100 | (shape << 3) | (descending ? 4 : 0) | hidden);
}
constexpr bool PlayRankArtValid(uint16_t id) {
    return id >= 0x3100 && id < 0x3200 && (id & 3) < 3 && ((id >> 3) & 31) < 5;
}
// Clock face showing hour24 (0..23) and half past (0/1); the sun or the moon
// beside it tells morning from evening.
constexpr uint16_t PlayClockArt(uint8_t hour24, bool half) {
    return static_cast<uint16_t>(0x3200 | (hour24 << 1) | (half ? 1 : 0));
}
constexpr bool PlayClockArtValid(uint16_t id) { return id >= 0x3200 && id < 0x3230; }
// Silhouette (grey checkerboard) of a solid picture: circle..heart (1..5) or
// Apple..Cup (401..407, stored in the low byte as 101..107).
constexpr uint16_t PlayShadowArt(uint16_t base) {
    return static_cast<uint16_t>(0x3300 | (base >= 400 ? base - 300 : base));
}
constexpr uint16_t PlayShadowBase(uint16_t id) {
    return static_cast<uint16_t>((id & 0xFF) >= 100 ? (id & 0xFF) + 300 : (id & 0xFF));
}
constexpr bool PlayShadowArtValid(uint16_t id) {
    return id >= 0x3300 && id < 0x3400 &&
           (((id & 0xFF) >= 1 && (id & 0xFF) <= 5) || ((id & 0xFF) >= 101 && (id & 0xFF) <= 107));
}
// Tree with 1..5 apples on the ground.
constexpr uint16_t PlayTreeArt(uint8_t fruit) { return static_cast<uint16_t>(0x3500 | fruit); }
constexpr bool PlayTreeArtValid(uint16_t id) { return id >= 0x3500 && id <= 0x3505; }

// Memory flip card faces: face index 1..9 -> catalogue picture.
constexpr uint16_t kMemoryFaceArt[9] = {1, 2, 3, 4, 5, 41, 42, 43, 44};

// "Find the side" question: an arrow pointing left (0), right (1) or to the
// middle (2) with the shape drawn under it; the answer cards show the shape
// at the left / right / middle of a box.
constexpr uint16_t PlayArrowArt(uint8_t side, uint8_t shape) {
    return static_cast<uint16_t>(0x3600 | (side << 3) | shape);
}
constexpr bool PlayArrowArtValid(uint16_t id) {
    return id >= 0x3600 && id < 0x3700 && ((id >> 3) & 31) < 3 && (id & 7) < 5;
}
constexpr uint16_t PlaySideArt(uint8_t side, uint8_t shape) {
    return static_cast<uint16_t>(0x3700 | (side << 3) | shape);
}
constexpr bool PlaySideArtValid(uint16_t id) {
    return id >= 0x3700 && id < 0x3800 && ((id >> 3) & 31) < 3 && (id & 7) < 5;
}

// A die showing 1..6 dots.
constexpr uint16_t PlayDiceArt(uint8_t n) { return static_cast<uint16_t>(0x3800 | n); }
constexpr bool PlayDiceArtValid(uint16_t id) { return id >= 0x3801 && id <= 0x3806; }

constexpr bool PlayArtValid(uint16_t id) {
    return PlayArtSimple(id) || PlayPatternArtValid(id) || PlaySpotArtValid(id) ||
           PlaySizedArtValid(id) || PlayRankArtValid(id) || PlayClockArtValid(id) ||
           PlayShadowArtValid(id) || PlayTreeArtValid(id) || PlayArrowArtValid(id) ||
           PlaySideArtValid(id) || PlayDiceArtValid(id);
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
    // V4-07 motion games (0 = the V4-06 answer cards above).
    // 1 = tilt ball: the track under the line, `ball` rolls towards `hole`
    // (cells 0..kBallCells-1). 2 = peekaboo: `hidden` while the board lies
    // face down.
    uint8_t motion = 0;
    uint8_t ball = 0;
    uint8_t hole = 0;
    bool hidden = false;
    // V8 sound activities (sound_view.h): 0 = none; 1 parrot, 2 songs,
    // 3 tooth brushing, 4 hand washing. sa..sd are their small fields.
    uint8_t sound = 0;
    uint8_t sa = 0, sb = 0, sc = 0, sd = 0;
    // V8 non-card games (0 = none): 3 memory flip, 4 maze, 5 shake (dice /
    // tree, uses the answer cards once something fell), 6 wooden man,
    // 7 day and night. `cells` holds per-cell state: memory = face index
    // (low 5 bits) | 0x80 face up | 0x40 matched; maze = open-wall bits
    // (1 up, 2 right, 4 down, 8 left) of each cell, row by row.
    uint8_t play = 0;
    uint8_t cols = 0, rows = 0;
    std::array<uint8_t, 24> cells{};
    uint8_t cursor = 0;  // memory: card under the dial
    uint8_t pos = 0;     // maze: where TuanTuan is
    uint8_t goal = 0;    // maze: where the carrot is
    uint8_t count = 0;   // wooden man: seconds held still so far
    bool night = false;  // day and night: the moon is up

    // Changes drawn below kGameRailTop.
    bool RailDiffers(const GameView& o) const {
        return option_count != o.option_count || highlight != o.highlight || options != o.options ||
               motion != o.motion || ball != o.ball || hole != o.hole || hidden != o.hidden ||
               sound != o.sound || sa != o.sa || sb != o.sb || sc != o.sc || sd != o.sd ||
               play != o.play || cols != o.cols || rows != o.rows || cells != o.cells ||
               cursor != o.cursor || pos != o.pos || goal != o.goal;
    }
    // Changes drawn above it.
    bool QuestionDiffers(const GameView& o) const {
        return round != o.round || rounds != o.rounds || feedback != o.feedback ||
               question_art != o.question_art || motion != o.motion || hidden != o.hidden ||
               sound != o.sound || sa != o.sa || sb != o.sb || sc != o.sc || sd != o.sd ||
               count != o.count || night != o.night;
    }
    bool operator==(const GameView& o) const { return !RailDiffers(o) && !QuestionDiffers(o); }
    bool operator!=(const GameView& o) const { return !(*this == o); }
};

}  // namespace kid_app
