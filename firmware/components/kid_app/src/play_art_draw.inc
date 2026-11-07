// V8 mini games: the PlayArt pictures and the boards of the non-card games.
// Included by scene_model.cc inside its anonymous namespace (it uses the
// file's drawing primitives); not a translation unit of its own. Pictures
// fit the same (cx, cy, r) square as the V4-06 GameArt ones: no text, no
// bitmaps, everything is drawn from lines, discs and polygons.

void GameArtIcon(MonoFrame& f, uint16_t art, int cx, int cy, int r);  // scene_model.cc

// Grey "shadow" of whatever the callback just drew in the square: every
// second black pixel is cleared (checkerboard).
void DitherSquare(MonoFrame& f, int cx, int cy, int r) {
    for (int y = Clamp(cy - r, 0, kH - 1); y <= Clamp(cy + r, 0, kH - 1); ++y)
        for (int x = Clamp(cx - r, 0, kW - 1); x <= Clamp(cx + r, 0, kW - 1); ++x)
            if (((x + y) & 1) && f.IsBlack(x, y)) f.SetPixel(x, y, false);
}

// The six classic dice layouts (GameDots stops at five).
void DiceDots(MonoFrame& f, int n, int cx, int cy, int r) {
    if (n != 6) {
        GameDots(f, n, cx, cy, r);
        return;
    }
    const int dot = std::max(4, r * 20 / 100);
    for (int col : {-1, 1})
        for (int row : {-1, 0, 1}) Disk(f, cx + col * r * 42 / 100, cy + row * r * 48 / 100, dot);
}

// The six animals that have a mama and a baby: 0 cat 1 dog 2 bird 3 fish
// 4 rabbit 5 duck.
void RabbitIcon(MonoFrame& f, int cx, int cy, int r) {
    const int w = std::max(3, r / 10);
    auto p = [r](int percent) { return r * percent / 100; };
    for (int side : {-1, 1}) FillEllipse(f, cx + side * p(24), cy - p(40), p(14), p(46));
    Blob(f, cx, cy + p(20), p(52), p(48), w);
    for (int side : {-1, 1}) Disk(f, cx + side * p(20), cy + p(8), p(7));
    FillEllipse(f, cx, cy + p(26), p(9), p(7));
    Arc(f, cx, cy + p(34), p(12), p(9), 0, 180, std::max(2, w * 2 / 3));
}

void DuckIcon(MonoFrame& f, int cx, int cy, int r) {
    const int w = std::max(3, r / 10);
    auto p = [r](int percent) { return r * percent / 100; };
    Blob(f, cx - p(14), cy + p(34), p(62), p(40), w);
    Arc(f, cx - p(24), cy + p(30), p(30), p(20), 0, 180, w);
    Blob(f, cx + p(30), cy - p(26), p(32), p(32), w);
    FillTriangle(f, cx + p(58), cy - p(30), cx + p(92), cy - p(22), cx + p(58), cy - p(12));
    Disk(f, cx + p(38), cy - p(36), p(6));
}

void AnimalIcon(MonoFrame& f, int species, int cx, int cy, int r) {
    switch (species) {
        case 0: GameArtIcon(f, 41, cx, cy, r); break;
        case 1: GameArtIcon(f, 42, cx, cy, r); break;
        case 2: GameArtIcon(f, 43, cx, cy, r); break;
        case 3: GameArtIcon(f, 44, cx, cy, r); break;
        case 4: RabbitIcon(f, cx, cy, r); break;
        default: DuckIcon(f, cx, cy, r); break;
    }
}

// A face for "can eat / cannot eat": open mouth + tick, or closed mouth + cross.
void EatFace(MonoFrame& f, int cx, int cy, int r, bool yes) {
    const int w = std::max(3, r / 10);
    auto p = [r](int percent) { return r * percent / 100; };
    Blob(f, cx - p(10), cy + p(6), p(58), p(58), w);
    for (int side : {-1, 1}) Disk(f, cx - p(10) + side * p(22), cy - p(10), p(7));
    if (yes) FillEllipse(f, cx - p(10), cy + p(26), p(20), p(14));
    else Stroke(f, cx - p(30), cy + p(28), cx + p(10), cy + p(28), w);
    if (yes) CheckMark(f, cx + p(66), cy - p(56), p(22), w, true);
    else Cross(f, cx + p(66), cy - p(56), p(18), w, true);
}

void Thermometer(MonoFrame& f, int cx, int cy, int r, bool hot) {
    const int w = std::max(3, r / 10);
    auto p = [r](int percent) { return r * percent / 100; };
    RoundRectRing(f, cx - p(16), cy - p(80), 2 * p(16), p(120), p(16), w);
    Disk(f, cx, cy + p(56), p(30));
    const int top = hot ? cy - p(62) : cy + p(0);
    FillRect(f, cx - p(7), top, 2 * p(7), cy + p(56) - top);
    if (hot) {
        for (int deg : {-60, -20, 20, 60}) Ray(f, cx + p(70), cy - p(30), deg, p(20), p(40), true);
    } else {
        Sparkle(f, cx + p(66), cy - p(36), p(18));
        Sparkle(f, cx + p(66), cy + p(14), p(10));
    }
}

void Snowflake(MonoFrame& f, int cx, int cy, int r) {
    const int w = std::max(3, r / 12);
    for (int deg : {0, 60, 120}) {
        Ray(f, cx, cy, deg, 0, r * 80 / 100, true);
        Ray(f, cx, cy, deg + 180, 0, r * 80 / 100, true);
    }
    for (int deg : {0, 60, 120, 180, 240, 300}) {
        const double a = deg * kPi / 180.0;
        const int tx = cx + static_cast<int>(std::lround(r * 0.55 * std::cos(a)));
        const int ty = cy + static_cast<int>(std::lround(r * 0.55 * std::sin(a)));
        Disk(f, tx, ty, w + 1);
    }
}

// Stick figure: rigid (Statue) or dancing with raised arms and motion arcs.
void StickFigure(MonoFrame& f, int cx, int cy, int r, bool dance) {
    const int w = std::max(4, r / 9);
    auto p = [r](int percent) { return r * percent / 100; };
    Disk(f, cx, cy - p(62), p(22));
    Stroke(f, cx, cy - p(36), cx, cy + p(20), w);
    if (dance) {
        Stroke(f, cx, cy - p(26), cx - p(50), cy - p(60), w);
        Stroke(f, cx, cy - p(26), cx + p(54), cy - p(8), w);
        Stroke(f, cx, cy + p(20), cx - p(40), cy + p(70), w);
        Stroke(f, cx, cy + p(20), cx + p(48), cy + p(54), w);
        Arc(f, cx, cy, p(84), p(84), 200, 250, w);
        Arc(f, cx, cy, p(84), p(84), 290, 340, w);
    } else {
        Stroke(f, cx, cy - p(26), cx - p(30), cy + p(14), w);
        Stroke(f, cx, cy - p(26), cx + p(30), cy + p(14), w);
        Stroke(f, cx, cy + p(20), cx - p(20), cy + p(78), w);
        Stroke(f, cx, cy + p(20), cx + p(20), cy + p(78), w);
    }
}

void ClockFace(MonoFrame& f, int cx, int cy, int r, int hour24, bool half) {
    const int w = std::max(3, r / 12);
    auto p = [r](int percent) { return r * percent / 100; };
    Ring(f, cx, cy, p(66), p(66), w);
    for (int k = 0; k < 12; ++k) {
        const double a = k * 30 * kPi / 180.0;
        const bool major = k % 3 == 0;
        Stroke(f, cx + static_cast<int>(std::lround(p(major ? 46 : 54) * std::sin(a))),
               cy - static_cast<int>(std::lround(p(major ? 46 : 54) * std::cos(a))),
               cx + static_cast<int>(std::lround(p(60) * std::sin(a))),
               cy - static_cast<int>(std::lround(p(60) * std::cos(a))), major ? w : 2);
    }
    const double hour_deg = ((hour24 % 12) + (half ? 0.5 : 0.0)) * 30.0;
    const double ha = hour_deg * kPi / 180.0;
    Stroke(f, cx, cy, cx + static_cast<int>(std::lround(p(34) * std::sin(ha))),
           cy - static_cast<int>(std::lround(p(34) * std::cos(ha))), w + 2);
    // Minute hand: 12 for o'clock, 6 for half past.
    Stroke(f, cx, cy, cx, cy + (half ? p(50) : -p(50)), w);
    Disk(f, cx, cy, w + 1);
    // The sun or the moon tells morning from evening.
    const bool day = hour24 >= 6 && hour24 < 18;
    if (day) {
        Disk(f, cx + p(92), cy - p(70), p(16));
        for (int deg = 0; deg < 360; deg += 60) Ray(f, cx + p(92), cy - p(70), deg, p(22), p(32), true);
    } else {
        Moon(f, cx + p(92), cy - p(70), p(22), true);
    }
}

void SmallShape(MonoFrame& f, int shape, int cx, int cy, int r) {
    GameArtIcon(f, kPlayShapeArt[shape % 5], cx, cy, r);
}

void PlayArtIcon(MonoFrame& f, uint16_t art, int cx, int cy, int r) {
    const int w = std::max(3, r / 10);
    auto p = [r](int percent) { return r * percent / 100; };
    if (PlayPatternArtValid(art)) {
        const int type = (art >> 9) & 3, a = (art >> 6) & 7, b = (art >> 3) & 7, c = art & 7;
        int seq[6], n = 0;
        if (type == 0) { seq[n++] = a; seq[n++] = b; seq[n++] = a; seq[n++] = b; }
        else if (type == 1) { seq[n++] = a; seq[n++] = a; seq[n++] = b; seq[n++] = a; seq[n++] = a; }
        else { seq[n++] = a; seq[n++] = b; seq[n++] = c; seq[n++] = a; seq[n++] = b; }
        const int step = r * 46 / 96, total = n + 1;
        for (int i = 0; i < total; ++i) {
            const int x = cx + (2 * i - (total - 1)) * step / 2;
            if (i < n) SmallShape(f, seq[i], x, cy, p(20));
            else QuestionMark(f, x, cy - p(6), p(14), w);
        }
        return;
    }
    if (PlaySpotArtValid(art)) {
        const int s[3] = {(art >> 8) & 3, (art >> 6) & 3, (art >> 4) & 3};
        const int idx = (art >> 2) & 3, delta = art & 3;
        const int step = r * 62 / 96;
        for (int row = 0; row < 2; ++row) {
            for (int i = 0; i < 3; ++i) {
                int shape = s[i];
                if (row == 1 && i == idx) shape = (s[i] + 1 + delta) % 4;
                SmallShape(f, shape, cx + (i - 1) * step, cy + (row ? 1 : -1) * p(46), p(24));
            }
        }
        Stroke(f, cx - p(86), cy, cx + p(86), cy, 2);
        return;
    }
    if (PlaySizedArtValid(art)) {
        static const int kScale[3] = {32, 58, 90};
        SmallShape(f, (art >> 2) & 63, cx, cy, p(kScale[art & 3]));
        return;
    }
    if (PlayRankArtValid(art)) {
        static const int kScale[3] = {26, 44, 66};
        const int shape = (art >> 3) & 31, hidden = art & 3;
        const bool desc = (art & 4) != 0;
        const int step = r * 62 / 96;
        for (int i = 0; i < 3; ++i) {
            const int x = cx + (i - 1) * step;
            if (i == hidden) QuestionMark(f, x, cy - p(6), p(20), w);
            else SmallShape(f, shape, x, cy, p(kScale[desc ? 2 - i : i]));
        }
        return;
    }
    if (PlayClockArtValid(art)) {
        ClockFace(f, cx - p(10), cy + p(6), r, (art >> 1) & 31, (art & 1) != 0);
        return;
    }
    if (PlayShadowArtValid(art)) {
        GameArtIcon(f, PlayShadowBase(art), cx, cy, r);
        DitherSquare(f, cx, cy, r);
        return;
    }
    if (PlayTreeArtValid(art)) {
        const int fruit = art & 7;
        Blob(f, cx, cy - p(30), p(62), p(50), w);
        FillRect(f, cx - p(10), cy + p(14), 2 * p(10), p(50));
        for (int k = 0; k < fruit; ++k)
            Disk(f, cx + (2 * k - (fruit - 1)) * p(17), cy + p(82), p(11));
        Disk(f, cx - p(22), cy - p(36), p(7));
        Disk(f, cx + p(24), cy - p(24), p(7));
        return;
    }
    if (PlayArrowArtValid(art)) {
        const int side = (art >> 3) & 31, shape = art & 7;
        if (side == 2) {  // down: the middle
            Stroke(f, cx, cy - p(80), cx, cy - p(18), w + 2);
            FillTriangle(f, cx - p(24), cy - p(26), cx + p(24), cy - p(26), cx, cy + p(2));
        } else {
            const int d = side == 0 ? -1 : 1;
            Stroke(f, cx - d * p(52), cy - p(46), cx + d * p(30), cy - p(46), w + 2);
            FillTriangle(f, cx + d * p(62), cy - p(46), cx + d * p(26), cy - p(72),
                         cx + d * p(26), cy - p(20));
        }
        SmallShape(f, shape, cx, cy + p(42), p(34));
        return;
    }
    if (PlaySideArtValid(art)) {
        const int side = (art >> 3) & 31, shape = art & 7;
        RoundRectRing(f, cx - p(86), cy - p(46), 2 * p(86), p(92), p(18), w);
        SmallShape(f, shape, cx + (side - 1) * p(52), cy, p(28));
        return;
    }
    if (PlayDiceArtValid(art)) {
        RoundRectRing(f, cx - p(70), cy - p(70), 2 * p(70), 2 * p(70), p(22), w + 1);
        DiceDots(f, art & 7, cx, cy, p(64));
        return;
    }
    switch (static_cast<PlayArt>(art)) {
        case PlayArt::Apple:
            Disk(f, cx, cy + p(14), p(62));
            Stroke(f, cx, cy - p(46), cx + p(8), cy - p(78), w);
            FillEllipse(f, cx + p(28), cy - p(66), p(22), p(10));
            break;
        case PlayArt::Carrot:
            FillTriangle(f, cx - p(30), cy - p(46), cx + p(30), cy - p(46), cx, cy + p(86));
            for (int k = -1; k <= 1; ++k)
                Stroke(f, cx + k * p(6), cy - p(46), cx + k * p(26), cy - p(88), w);
            Stroke(f, cx - p(18), cy - p(14), cx + p(10), cy - p(14), 2, false);
            Stroke(f, cx - p(10), cy + p(20), cx + p(6), cy + p(20), 2, false);
            break;
        case PlayArt::Cake:
            FillRect(f, cx - p(74), cy + p(10), 2 * p(74), p(50));
            FillRect(f, cx - p(52), cy - p(28), 2 * p(52), p(38));
            Stroke(f, cx - p(74), cy + p(30), cx + p(74), cy + p(30), 3, false);
            Stroke(f, cx, cy - p(28), cx, cy - p(60), w);
            FillEllipse(f, cx, cy - p(70), p(8), p(12));
            break;
        case PlayArt::Bread:
            FillRoundRect(f, cx - p(84), cy - p(40), 2 * p(84), p(80), p(34));
            for (int k = -1; k <= 1; ++k)
                Stroke(f, cx + k * p(34) - p(8), cy - p(18), cx + k * p(34) + p(8), cy + p(6), 3,
                       false);
            break;
        case PlayArt::Shoe:
            FillRoundRect(f, cx - p(84), cy + p(10), 2 * p(84), p(36), p(14));
            FillRoundRect(f, cx - p(84), cy - p(46), p(76), p(66), p(18));
            Stroke(f, cx - p(60), cy - p(24), cx - p(26), cy - p(24), 3, false);
            Stroke(f, cx - p(60), cy - p(8), cx - p(26), cy - p(8), 3, false);
            break;
        case PlayArt::Hat:
            FillEllipse(f, cx, cy + p(36), p(88), p(16));
            FillRoundRect(f, cx - p(48), cy - p(54), 2 * p(48), p(92), p(22));
            Stroke(f, cx - p(48), cy + p(8), cx + p(48), cy + p(8), 4, false);
            break;
        case PlayArt::Cup:
            FillRoundRect(f, cx - p(52), cy - p(44), 2 * p(52), p(96), p(16));
            FillRoundRect(f, cx - p(38), cy - p(32), 2 * p(38), p(52), p(8), false);
            Ring(f, cx + p(62), cy, p(26), p(30), w);
            break;
        case PlayArt::Rabbit:
            RabbitIcon(f, cx, cy, r);
            break;
        case PlayArt::Duck:
            DuckIcon(f, cx, cy, r);
            break;
        case PlayArt::BabyCat: case PlayArt::BabyDog: case PlayArt::BabyBird:
        case PlayArt::BabyFish: case PlayArt::BabyRabbit: case PlayArt::BabyDuck:
            AnimalIcon(f, art - static_cast<int>(PlayArt::BabyCat), cx, cy + p(24), p(62));
            Sparkle(f, cx + p(58), cy - p(54), p(14));
            break;
        case PlayArt::EatYes:
            EatFace(f, cx, cy, r, true);
            break;
        case PlayArt::EatNo:
            EatFace(f, cx, cy, r, false);
            break;
        case PlayArt::Eat:
            FillEllipse(f, cx, cy + p(6), p(76), p(56));
            FillRect(f, cx - p(80), cy - p(60), 2 * p(80), p(66), false);
            Stroke(f, cx - p(30), cy - p(70), cx + p(40), cy - p(12), w);
            Stroke(f, cx - p(14), cy - p(74), cx + p(54), cy - p(20), w);
            break;
        case PlayArt::Sleep:
            Moon(f, cx - p(18), cy + p(6), p(58), true);
            LetterZ(f, cx + p(30), cy - p(60), p(26), w);
            LetterZ(f, cx + p(58), cy - p(86), p(16), std::max(2, w * 2 / 3));
            break;
        case PlayArt::Play:
            GameBall(f, cx - p(10), cy + p(8), p(60));
            Sparkle(f, cx + p(58), cy - p(54), p(18));
            Sparkle(f, cx + p(70), cy - p(10), p(10));
            break;
        case PlayArt::Bath:
            FillRoundRect(f, cx - p(84), cy - p(4), 2 * p(84), p(54), p(22));
            Stroke(f, cx - p(60), cy + p(50), cx - p(60), cy + p(70), w);
            Stroke(f, cx + p(60), cy + p(50), cx + p(60), cy + p(70), w);
            Ring(f, cx - p(40), cy - p(34), p(18), p(18), 3);
            Ring(f, cx + p(10), cy - p(52), p(12), p(12), 3);
            Ring(f, cx + p(44), cy - p(30), p(16), p(16), 3);
            break;
        case PlayArt::Tshirt:
            FillRect(f, cx - p(42), cy - p(40), 2 * p(42), p(104));
            FillTriangle(f, cx - p(42), cy - p(40), cx - p(88), cy - p(6), cx - p(42), cy + p(14));
            FillTriangle(f, cx + p(42), cy - p(40), cx + p(88), cy - p(6), cx + p(42), cy + p(14));
            Disk(f, cx, cy - p(44), p(17), false);
            break;
        case PlayArt::Scarf:
            FillRoundRect(f, cx - p(82), cy - p(46), 2 * p(82), p(44), p(22));
            FillRect(f, cx + p(18), cy - p(6), p(36), p(84));
            for (int k = 0; k < 3; ++k)
                Stroke(f, cx + p(18), cy + p(18) + k * p(22), cx + p(54), cy + p(18) + k * p(22), 3,
                       false);
            break;
        case PlayArt::Umbrella:
            Disk(f, cx, cy - p(8), p(82));
            FillRect(f, cx - p(86), cy - p(8), 2 * p(86), p(90), false);
            Stroke(f, cx, cy - p(8), cx, cy + p(66), w);
            Arc(f, cx - p(15), cy + p(66), p(15), p(15), 0, 180, w);
            break;
        case PlayArt::Gloves:
            FillRoundRect(f, cx - p(44), cy - p(12), 2 * p(44), p(60), p(14));
            for (int k = 0; k < 4; ++k)
                FillRoundRect(f, cx - p(44) + k * p(23), cy - p(66) + (k == 1 || k == 2 ? 0 : p(10)),
                              p(19), p(70), p(9));
            FillEllipse(f, cx + p(54), cy + p(14), p(20), p(14));
            FillRect(f, cx - p(46), cy + p(44), 2 * p(46), p(26));
            Stroke(f, cx - p(46), cy + p(52), cx + p(46), cy + p(52), 3, false);
            break;
        case PlayArt::Sunglasses:
            for (int side : {-1, 1}) FillEllipse(f, cx + side * p(42), cy, p(36), p(28));
            Stroke(f, cx - p(10), cy - p(4), cx + p(10), cy - p(4), w);
            Stroke(f, cx - p(78), cy - p(8), cx - p(92), cy - p(26), w);
            Stroke(f, cx + p(78), cy - p(8), cx + p(92), cy - p(26), w);
            break;
        case PlayArt::Sun:
            Disk(f, cx, cy, p(44));
            for (int deg = 0; deg < 360; deg += 45) Ray(f, cx, cy, deg, p(58), p(86), true);
            break;
        case PlayArt::Snow:
            Snowflake(f, cx, cy, r);
            break;
        case PlayArt::Rain:
            Cloud(f, cx, cy - p(30), std::max(1, r / 14), true);
            for (int k = -2; k <= 2; ++k)
                Stroke(f, cx + k * p(30), cy + p(30), cx + k * p(30) - p(10), cy + p(62), w);
            break;
        case PlayArt::ThermoHot:
            Thermometer(f, cx - p(10), cy, r, true);
            break;
        case PlayArt::ThermoCold:
            Thermometer(f, cx - p(10), cy, r, false);
            break;
        case PlayArt::Dots6:
            DiceDots(f, 6, cx, cy, r);
            break;
        case PlayArt::ShakeMe:
            RoundRectRing(f, cx - p(36), cy - p(70), 2 * p(36), 2 * p(70), p(14), w);
            Disk(f, cx, cy + p(52), p(7));
            for (int side : {-1, 1}) {
                Arc(f, cx + side * p(64), cy, p(20), p(46), side < 0 ? 100 : -80,
                    side < 0 ? 260 : 80, w);
                Arc(f, cx + side * p(88), cy, p(20), p(30), side < 0 ? 110 : -70,
                    side < 0 ? 250 : 70, w);
            }
            break;
        case PlayArt::Tree:
            Blob(f, cx, cy - p(30), p(62), p(50), w);
            FillRect(f, cx - p(10), cy + p(14), 2 * p(10), p(60));
            break;
        case PlayArt::Pos1: case PlayArt::Pos2: case PlayArt::Pos3: {
            const int mark = art - static_cast<int>(PlayArt::Pos1);
            for (int i = 0; i < 3; ++i) {
                const int x = cx + (i - 1) * p(56);
                if (i == mark) Disk(f, x, cy, p(22));
                else Ring(f, x, cy, p(22), p(22), std::max(3, w - 1));
            }
            break;
        }
        case PlayArt::Statue:
            StickFigure(f, cx, cy, r, false);
            break;
        case PlayArt::Dance:
            StickFigure(f, cx, cy, r, true);
            break;
        case PlayArt::Moon:
            Moon(f, cx - p(10), cy, p(70), true);
            Sparkle(f, cx + p(56), cy - p(50), p(18));
            Sparkle(f, cx + p(70), cy + p(20), p(10));
            break;
    }
}

// ---- Boards of the non-card games (below GameScene's question area) ----

constexpr int kPlayRailTop = 300;  // a few px under SceneComposer::kGameRailTop

// Memory flip: cards face down (solid with a dot), face up (the picture),
// matched (the picture with a ring); the dial cursor is a thick frame.
void MemoryBoard(MonoFrame& f, const GameView& g) {
    const int cw = 150, ch = 76, gx = 24, gy = 12;
    const int total_w = g.cols * cw + (g.cols - 1) * gx;
    const int x0 = 400 - total_w / 2, y0 = kPlayRailTop + 6;
    for (int i = 0; i < g.cols * g.rows; ++i) {
        const int x = x0 + (i % g.cols) * (cw + gx), y = y0 + (i / g.cols) * (ch + gy);
        const uint8_t cell = g.cells[i];
        const bool up = (cell & 0x80) || (cell & 0x40);
        if (up) {
            RoundRectRing(f, x, y, cw, ch, 18, (cell & 0x40) ? 8 : 4);
            GameArtIcon(f, kMemoryFaceArt[(cell & 0x1F) - 1], x + cw / 2, y + ch / 2, 27);
        } else {
            FillRoundRect(f, x, y, cw, ch, 18);
            Disk(f, x + cw / 2, y + ch / 2, 8, false);
        }
        if (i == g.cursor && !(cell & 0x40))
            RoundRectRing(f, x - 8, y - 6, cw + 16, ch + 12, 22, 6);
    }
}

// Tilt maze: the walls as lines, TuanTuan (a small cat) and the carrot.
void MazeBoard(MonoFrame& f, const GameView& g) {
    const int cell = 176 / g.rows;  // 58 px for 3 rows, 44 px for 4 rows
    const int x0 = 400 - g.cols * cell / 2, y0 = kPlayRailTop + 2;
    for (int i = 0; i < g.cols * g.rows; ++i) {
        const int x = x0 + (i % g.cols) * cell, y = y0 + (i / g.cols) * cell;
        const uint8_t open = g.cells[i];
        if (!(open & 1)) Stroke(f, x, y, x + cell, y, 4);
        if (!(open & 2)) Stroke(f, x + cell, y, x + cell, y + cell, 4);
        if (!(open & 4)) Stroke(f, x, y + cell, x + cell, y + cell, 4);
        if (!(open & 8)) Stroke(f, x, y, x, y + cell, 4);
    }
    auto centre = [&](int idx, int* cx, int* cy) {
        *cx = x0 + (idx % g.cols) * cell + cell / 2;
        *cy = y0 + (idx / g.cols) * cell + cell / 2;
    };
    int cx, cy;
    centre(g.goal, &cx, &cy);
    GameArtIcon(f, static_cast<uint16_t>(PlayArt::Carrot), cx, cy, cell * 36 / 100);
    centre(g.pos, &cx, &cy);
    const int k = cell / 4;
    Disk(f, cx, cy + 2, k + 4);
    FillTriangle(f, cx - k - 3, cy - 2, cx - k / 2, cy - k - 10, cx - 1, cy - 6);
    FillTriangle(f, cx + k + 3, cy - 2, cx + k / 2, cy - k - 10, cx + 1, cy - 6);
    Disk(f, cx - k / 2 - 1, cy, 3, false);
    Disk(f, cx + k / 2 + 1, cy, 3, false);
}

// Wooden man: three seconds as three discs that fill while holding still.
void StatueBoard(MonoFrame& f, const GameView& g) {
    for (int i = 0; i < 3; ++i) {
        const int x = 300 + i * 100, y = 392;
        if (i < g.count) Disk(f, x, y, 30);
        else Ring(f, x, y, 30, 30, 6);
    }
}

// Day and night: a night sky strip (stars) or a day strip (sun, cloud, ground).
void SkyBoard(MonoFrame& f, const GameView& g) {
    if (g.night) {
        FillRoundRect(f, 120, kPlayRailTop + 14, 560, 150, 28);
        Sparkle(f, 220, 360, 14, false);
        Sparkle(f, 330, 400, 9, false);
        Sparkle(f, 470, 350, 12, false);
        Sparkle(f, 580, 410, 9, false);
        Moon(f, 400, 384, 36, false);
    } else {
        RoundRectRing(f, 120, kPlayRailTop + 14, 560, 150, 28, 6);
        Disk(f, 250, 372, 28);
        for (int deg = 0; deg < 360; deg += 45) Ray(f, 250, 372, deg, 38, 54, true);
        Cloud(f, 470, 366, 4, true);
        Stroke(f, 150, 440, 650, 440, 5);
    }
}

void PlayBoard(MonoFrame& f, const GameView& g) {
    switch (g.play) {
        case 3: MemoryBoard(f, g); break;
        case 4: MazeBoard(f, g); break;
        case 6: StatueBoard(f, g); break;
        case 7: SkyBoard(f, g); break;
        default: break;
    }
}
