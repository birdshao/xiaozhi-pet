#pragma once

#include "kid_app/game_view.h"
#include "kid_app/parrot_session.h"
#include "kid_app/routine_timer.h"
#include "kid_app/song_player.h"

#include <cstdint>

namespace kid_app {

// V8 sound activities as a SceneKind::Game scene (no new SceneKind): the
// parrot, the song player and the brushing / hand-washing companion fill the
// GameView `sound` subtype, drawn by SceneComposer next to the quiz cards and
// the motion tracks. Picture ids 401..410 are this session's range
// (activity_list.h); append only, never renumber.
enum class SoundArt : uint16_t {
    Parrot = 401, Song = 402, ToothBrush = 403, HandWash = 404, Lullaby = 405,
};
constexpr bool SoundArtValid(uint16_t id) { return id >= 401 && id <= 410; }

// GameView::sound values.
constexpr uint8_t kSoundParrot = 1, kSoundSongs = 2, kSoundBrush = 3, kSoundWash = 4;

// What each field means (the scene code and these builders share this table):
//   parrot  sa = ParrotPhase, sb = ParrotHint
//   songs   sa = SongPhase, sb = SongKind | SongMissing << 4, sc = sway 0..3,
//           sd = timer choice 1..3 (10/20/30 min) or 0; highlight = list
//           cursor, option_count = songs in the list
//   brush / wash  sa = RoutinePhase, sb = sand step, sc = steps
GameView ParrotGameView(const ParrotView& view);
GameView SongGameView(const SongView& view);
GameView RoutineGameView(const RoutineView& view);

}  // namespace kid_app
