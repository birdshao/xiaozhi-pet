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
// the motion tracks. Picture ids 701..710 are this session's range
// (PlayArt owns 401..513, CareArt 601..660, activity_list.h; the test
// "Art ranges never overlap" checks it); append only, never renumber.
enum class SoundArt : uint16_t {
    Parrot = 701, Song = 702, ToothBrush = 703, HandWash = 704, Lullaby = 705,
};
// V9 S1: the parrot's other voice cards (706..710; the parrot itself keeps
// SoundArt::Parrot). Drawn by sound_draw_v9.inc; the voice number travels in
// GameView::sc (0..4 = ParrotVoice, 5 = the home slot).
enum class ParrotArt : uint16_t { Chipmunk = 706, Bear = 707, Robot = 708, Echo = 709, Home = 710 };
constexpr bool SoundArtValid(uint16_t id) { return id >= 701 && id <= 710; }

// GameView::sound values.
constexpr uint8_t kSoundParrot = 1, kSoundSongs = 2, kSoundBrush = 3, kSoundWash = 4;

// What each field means (the scene code and these builders share this table):
//   parrot  sa = ParrotPhase, sb = ParrotHint, sc = choice (voice 0..4, 5 = home)
//   songs   sa = SongPhase, sb = SongKind | SongMissing << 4, sc = sway 0..3,
//           sd = timer choice 1..3 (10/20/30 min) or 0; highlight = list
//           cursor, option_count = songs in the list
//   brush / wash  sa = RoutinePhase, sb = sand step, sc = steps
GameView ParrotGameView(const ParrotView& view);
GameView SongGameView(const SongView& view);
GameView RoutineGameView(const RoutineView& view);

}  // namespace kid_app
