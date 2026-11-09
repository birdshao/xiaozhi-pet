// V9 S1 parrot voices: the five voice cards, the home slot, TuanTuan's hats
// and the parrot scene. Included by scene_model.cc inside its anonymous
// namespace (it uses the file's drawing primitives); not a translation unit
// of its own. Everything is drawn from lines, discs and polygons: no text, no
// bitmaps. Pure drawing: it reads the GameView only, never any audio.
//
// GameView for the parrot: sa = ParrotPhase, sb = ParrotHint, sc = choice
// (0 parrot, 1 chipmunk, 2 bear, 3 robot, 4 echo, 5 home).

void GameArtIcon(MonoFrame& f, uint16_t art, int cx, int cy, int r);  // scene_model.cc

// A shared pair of eyes: white disc with a black pupil.
void ParrotEyes(MonoFrame& f, int cx, int cy, int gap, int r) {
    for (int side : {-1, 1}) {
        Disk(f, cx + side * gap, cy, r, false);
        Ring(f, cx + side * gap, cy, r, r, std::max(2, r / 4));
        Disk(f, cx + side * gap, cy + r / 4, std::max(2, r / 2));
    }
}

// Chipmunk: round face, big ears, two stripes, cheeks and a buck tooth.
void ChipmunkIcon(MonoFrame& f, int cx, int cy, int r) {
    auto p = [r](int percent) { return r * percent / 100; };
    const int w = std::max(4, r / 12);
    for (int side : {-1, 1}) {
        Disk(f, cx + side * p(46), cy - p(54), p(22));
        Disk(f, cx + side * p(46), cy - p(54), p(11), false);
    }
    Blob(f, cx, cy, p(66), p(58), w);
    for (int k : {-1, 0, 1}) Stroke(f, cx + k * p(16), cy - p(56), cx + k * p(12), cy - p(30), w);
    ParrotEyes(f, cx, cy - p(10), p(26), p(10));
    for (int side : {-1, 1}) Shade(f, cx + side * p(46), cy + p(22), p(18), p(14), 5);
    FillEllipse(f, cx, cy + p(10), p(8), p(6));
    FillRect(f, cx - p(8), cy + p(20), p(16), p(18), false);
    FillRect(f, cx - p(8), cy + p(20), p(16), p(2));
    Stroke(f, cx - p(8), cy + p(20), cx - p(8), cy + p(38), w / 2 + 1);
    Stroke(f, cx + p(8), cy + p(20), cx + p(8), cy + p(38), w / 2 + 1);
    Stroke(f, cx - p(8), cy + p(38), cx + p(8), cy + p(38), w / 2 + 1);
}

// Bear: big round ears, a pale muzzle and a nose.
void BearIcon(MonoFrame& f, int cx, int cy, int r) {
    auto p = [r](int percent) { return r * percent / 100; };
    const int w = std::max(4, r / 12);
    for (int side : {-1, 1}) {
        Disk(f, cx + side * p(54), cy - p(56), p(26));
        Disk(f, cx + side * p(54), cy - p(56), p(13), false);
    }
    Blob(f, cx, cy, p(72), p(64), w);
    ParrotEyes(f, cx, cy - p(18), p(30), p(9));
    Blob(f, cx, cy + p(22), p(30), p(22), std::max(3, w - 1));
    FillEllipse(f, cx, cy + p(10), p(11), p(8));
    Stroke(f, cx, cy + p(18), cx, cy + p(30), w / 2 + 1);
    Arc(f, cx - p(10), cy + p(30), p(10), p(8), 0, 180, w / 2 + 1);
    Arc(f, cx + p(10), cy + p(30), p(10), p(8), 0, 180, w / 2 + 1);
}

// Robot: boxy head, antenna, square eyes, a speaker grille mouth.
void RobotIcon(MonoFrame& f, int cx, int cy, int r) {
    auto p = [r](int percent) { return r * percent / 100; };
    const int w = std::max(4, r / 12);
    Stroke(f, cx, cy - p(54), cx, cy - p(86), w);
    Disk(f, cx, cy - p(90), p(9));
    FillRoundRect(f, cx - p(70), cy - p(56), p(140), p(112), p(16));
    FillRoundRect(f, cx - p(70) + w, cy - p(56) + w, p(140) - 2 * w, p(112) - 2 * w, p(12), false);
    for (int side : {-1, 1}) {
        FillRect(f, cx + side * p(30) - p(15), cy - p(34), p(30), p(26));
        FillRect(f, cx + side * p(30) - p(5), cy - p(26), p(10), p(10), false);
        FillRoundRect(f, cx + side * p(86), cy - p(16), p(10), p(32), p(4));  // ear bolts
    }
    for (int i = 0; i < 4; ++i) {
        const int x = cx - p(36) + i * p(24);
        Stroke(f, x, cy + p(16), x, cy + p(40), std::max(3, w - 1));
    }
    Stroke(f, cx - p(42), cy + p(28), cx + p(42), cy + p(28), std::max(2, w / 2));
}

// Echo: a speaker and sound arcs that fade out to the right.
void EchoIcon(MonoFrame& f, int cx, int cy, int r) {
    auto p = [r](int percent) { return r * percent / 100; };
    const int w = std::max(4, r / 11);
    const int x0 = cx - p(66);
    FillRect(f, x0, cy - p(22), p(30), p(44));
    FillTriangle(f, x0 + p(28), cy - p(22), x0 + p(28), cy + p(22), x0 + p(78), cy + p(62));
    FillTriangle(f, x0 + p(28), cy - p(22), x0 + p(78), cy - p(62), x0 + p(78), cy + p(62));
    for (int k = 1; k <= 3; ++k) {
        const int rr = p(34) + k * p(26);
        Arc(f, x0 + p(52), cy, rr, rr, -48, 48, std::max(2, w - k + 1));
    }
}

// A little house: the "go home" slot.
void ParrotHomeIcon(MonoFrame& f, int cx, int cy, int r) {
    auto p = [r](int percent) { return r * percent / 100; };
    const int w = std::max(4, r / 11);
    FillTriangle(f, cx - p(82), cy - p(6), cx + p(82), cy - p(6), cx, cy - p(78));
    FillRect(f, cx - p(58), cy - p(6), p(116), p(72), false);
    Stroke(f, cx - p(58), cy - p(6), cx - p(58), cy + p(66), w);
    Stroke(f, cx + p(58), cy - p(6), cx + p(58), cy + p(66), w);
    Stroke(f, cx - p(58), cy + p(66), cx + p(58), cy + p(66), w);
    FillRoundRect(f, cx - p(16), cy + p(18), p(32), p(48), p(6));
}

void ParrotVoiceIcon(MonoFrame& f, uint8_t choice, int cx, int cy, int r) {
    switch (choice) {
        case 0: GameArtIcon(f, static_cast<uint16_t>(GameArt::Bird), cx, cy, r); break;
        case 1: ChipmunkIcon(f, cx, cy, r); break;
        case 2: BearIcon(f, cx, cy, r); break;
        case 3: RobotIcon(f, cx, cy, r); break;
        case 4: EchoIcon(f, cx, cy, r); break;
        default: ParrotHomeIcon(f, cx, cy, r); break;
    }
}

// TuanTuan's hat for the chosen voice, between the ears of CatHead(cx, 172, 62).
void ParrotHat(MonoFrame& f, uint8_t choice, int cx) {
    const int top = 106;  // the crown of the head
    switch (choice) {
        case 0:  // a feather plume
            for (int k : {-1, 0, 1}) Stroke(f, cx, top, cx + k * 12, top - 34 + (k ? 8 : 0), 5);
            Sparkle(f, cx + 26, top - 30, 8);
            break;
        case 1:  // an acorn cap
            FillEllipse(f, cx, top - 8, 20, 14);
            FillRect(f, cx - 22, top - 4, 44, 6, false);
            Stroke(f, cx, top - 20, cx + 4, top - 32, 5);
            break;
        case 2:  // bear ears
            for (int side : {-1, 1}) {
                Disk(f, cx + side * 16, top - 8, 13);
                Disk(f, cx + side * 16, top - 8, 6, false);
            }
            break;
        case 3:  // an antenna
            Stroke(f, cx, top, cx, top - 30, 5);
            Disk(f, cx, top - 36, 8);
            break;
        case 4:  // sound rings
            Disk(f, cx, top - 6, 6);
            for (int k = 1; k <= 2; ++k) {
                Arc(f, cx, top - 6, 10 + 11 * k, 10 + 11 * k, 215, 325, 4);
            }
            break;
        default: break;
    }
}

void ParrotScene(MonoFrame& f, const GameView& g) {
    constexpr int kCardX = 336, kCardY = 26, kCardW = 292, kCardH = 252, kCx = 482, kCy = 152;
    const auto phase = static_cast<ParrotPhase>(g.sa);
    const auto hint = static_cast<ParrotHint>(g.sb);
    const uint8_t choice = g.sc > 5 ? 0 : g.sc;
    const Emotion emotion = phase == ParrotPhase::Recording ? Emotion::Listening
                            : phase == ParrotPhase::Playing ? Emotion::Speaking
                                                            : Emotion::Curious;
    constexpr int kHeadX = 150;
    CatHead(f, kHeadX, 172, 62, emotion, Eyes::ByEmotion);
    ParrotHat(f, choice, kHeadX);
    RoundRectRing(f, kCardX, kCardY, kCardW, kCardH, 28, 6);
    ParrotVoiceIcon(f, choice, kCx, kCy, phase == ParrotPhase::Ready ? 100 : 88);

    if (phase == ParrotPhase::Playing) {
        Heart(f, 52, 64, 20);
        Sparkle(f, 270, 120, 14);
        Sparkle(f, 360, 60, 10);
    } else if (phase == ParrotPhase::Recording) {
        for (int k = 1; k <= 2; ++k) {  // sound arcs both sides of the card's picture
            Arc(f, kCx, kCy, 104 + 14 * k, 104 + 14 * k, -32, 32, 6);
            Arc(f, kCx, kCy, 104 + 14 * k, 104 + 14 * k, 148, 212, 6);
        }
    } else {
        // The choice row: one mark per voice and the little house; the chosen
        // one is big and ringed. The dial moves it, the press confirms.
        constexpr int kRowY = 322, kPitch = 52, kRowX0 = kCx - 5 * kPitch / 2;
        for (int i = 0; i < 6; ++i) {
            const int x = kRowX0 + i * kPitch;
            if (i == choice) {
                Ring(f, x, kRowY, 26, 26, 6);
                if (i == 5) ParrotHomeIcon(f, x, kRowY, 14);
                else Disk(f, x, kRowY, 12);
            } else if (i == 5) {
                ParrotHomeIcon(f, x, kRowY, 11);
            } else {
                Disk(f, x, kRowY, 8);
            }
        }
    }
    if (hint == ParrotHint::TooShort) QuestionMark(f, 274, 84, 22, 8);
    PressCue(f, 400, 392, phase == ParrotPhase::Recording);
}
