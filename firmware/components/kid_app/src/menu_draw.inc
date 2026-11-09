// V9 1A: the four category cards of the play menu (MenuArt, activity_list.h).
// Included by scene_model.cc inside its anonymous namespace (it uses the
// file's drawing primitives); not a translation unit of its own. Same
// (cx, cy, r) square as the other game pictures: no text, no bitmaps.
//   Today: TuanTuan with a happy face and sparkles   Brain: a card with a star
//   Move:  a ball with speed lines                   Sound: a note and waves

void MenuArtIcon(MonoFrame& f, uint16_t art, int cx, int cy, int r) {
    auto p = [r](int percent) { return r * percent / 100; };
    const int w = std::max(3, r / 10);
    switch (static_cast<MenuArt>(art)) {
        case MenuArt::Today:
            CatHead(f, cx - p(8), cy + p(20), r * 100 / 190, Emotion::Happy);
            Sparkle(f, cx + p(66), cy - p(58), p(26));
            Sparkle(f, cx - p(72), cy - p(46), p(14));
            break;
        case MenuArt::Brain: {
            const int cw = p(130), ch = p(165);
            FillRect(f, cx - cw / 2, cy - ch / 2, cw, ch);
            FillRect(f, cx - cw / 2 + w, cy - ch / 2 + w, cw - 2 * w, ch - 2 * w, false);
            Sparkle(f, cx, cy - p(8), p(40));  // the thing to think about
            Disk(f, cx, cy + p(52), std::max(3, p(7)));
            break;
        }
        case MenuArt::Move:
            Disk(f, cx + p(26), cy + p(4), p(52));
            Disk(f, cx + p(26) - p(14), cy - p(10), p(10), false);  // a shine
            for (int k = -1; k <= 1; ++k)  // speed lines behind the ball
                Stroke(f, cx - p(92), cy + p(4) + k * p(30), cx - p(46) + (k == 0 ? 0 : p(14)),
                       cy + p(4) + k * p(30), w);
            Stroke(f, cx - p(70), cy + p(70), cx + p(88), cy + p(70), w);  // the floor
            break;
        case MenuArt::Sound:
            FillEllipse(f, cx - p(36), cy + p(52), p(30), p(22));          // note head
            FillRect(f, cx - p(10), cy - p(64), std::max(4, p(10)), p(118));  // stem
            FillTriangle(f, cx - p(10), cy - p(64), cx + p(40), cy - p(38), cx - p(10), cy - p(8));
            for (int k = 1; k <= 2; ++k)  // sound waves
                Arc(f, cx + p(22), cy - p(8), p(26 + 24 * k), p(26 + 24 * k), -45, 45, w);
            break;
    }
}
