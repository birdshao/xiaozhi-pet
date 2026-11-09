#include "kid_app/sound_view.h"

namespace kid_app {

GameView ParrotGameView(const ParrotView& view) {
    GameView g;
    g.sound = kSoundParrot;
    g.sa = static_cast<uint8_t>(view.phase);
    g.sb = static_cast<uint8_t>(view.hint);
    g.sc = view.choice;
    return g;
}

GameView SongGameView(const SongView& view) {
    GameView g;
    g.sound = kSoundSongs;
    g.sa = static_cast<uint8_t>(view.phase);
    g.sb = static_cast<uint8_t>(static_cast<uint8_t>(view.kind) |
                                (static_cast<uint8_t>(view.missing) << 4));
    g.sc = static_cast<uint8_t>(view.sway & 3);
    g.sd = view.timer_minutes == 10 ? 1 : view.timer_minutes == 20 ? 2 : view.timer_minutes == 30 ? 3 : 0;
    g.highlight = view.selected;
    g.option_count = view.count;
    return g;
}

GameView RoutineGameView(const RoutineView& view) {
    GameView g;
    g.sound = view.kind == RoutineKind::Brush ? kSoundBrush : kSoundWash;
    g.sa = static_cast<uint8_t>(view.phase);
    g.sb = view.step;
    g.sc = view.steps;
    return g;
}

}  // namespace kid_app
