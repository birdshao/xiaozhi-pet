#pragma once

#include "kid_app/activity_list.h"

#include <array>
#include <cstdint>

namespace kid_app {

// V9 1A (docs/V9_MENU_DEEPENING_PROPOSAL.md G1): "today's pick", the first
// card of the play menu. A pure, deterministic function of the inputs below:
// the same day and state always give the same three games, no clock, no
// random source, nothing stored, no reward. Never decides anything for the
// child: it only orders three cards.
//
// Rules, in order:
//  - Candidates are the thinking, moving and sound games that the parents'
//    switches allow (motion off: no game that needs the board to move; mic
//    off: none that needs the microphone), that suit the age band, minus
//    the song player (not a game) and the sticker album.
//  - After dusk (`evening`): quiet games only, the thinking cards (cards,
//    memory flips). Quiet wins over boredom.
//  - Bored (level >= 2) in the daytime: moving games.
//  - Otherwise one game from each of thinking / moving / sound, the order
//    of the three rotating with the day.
//  - Inside a category the day shuffles the games; the game played last is
//    only offered again when nothing else is left.
//  - A category that cannot fill its slots is topped up from the next one.

// Age band of the child. All = not known yet (the default until the parent
// age setting exists, V9 2D): no age filter. Otherwise the child's age in
// years (3..5): a game suggested from a later age (play_content.h min_age)
// is left out. Older children keep all games.
enum class AgeBand : uint8_t { All = 0, Three = 3, Four = 4, Five = 5 };

struct RecommendInput {
    uint32_t day = 0;       // local day number (any stable counter, one value per day)
    bool evening = false;   // dusk or later; false when the clock is unknown
    uint8_t bored = 0;      // care need level 0..3 (care_store.h NeedLevels::bored)
    GameChoice last_game = GameChoice::None;  // the game played last (memory today.last_game)
    AgeBand age = AgeBand::All;
    bool motion_ok = true;  // parent motion switch
    bool mic_ok = true;
};

constexpr uint8_t kRecommendCount = 3;
using Recommendation = std::array<GameChoice, kRecommendCount>;

// Slots that stay None only when fewer than three games qualify at all.
Recommendation RecommendToday(const RecommendInput& in);

// The menu card of a growth memory game id (GAME_COMPLETED option, as stored
// in memory today.last_game): 1 quiz, 2 candles, 3 clap, 4 ball, 5 peekaboo,
// then the V8 games by their own ids. None for 0 and unknown ids.
GameChoice GameChoiceOfMemoryId(uint16_t id);

}  // namespace kid_app
