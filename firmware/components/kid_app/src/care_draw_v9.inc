// V9 care pictures (docs/V9_MENU_DEEPENING_PROPOSAL.md slice 1C), included
// by scene_model.cc next to the V8 care scenes. Everything is drawn from the
// primitives there (no bitmaps); the care picture keeps to the area above
// SceneComposer::kGameRailTop and away from the status corner boxes.

// ---- C1: what changes from day to day ----

// Eight places on the care pet's head (CatHead at scale 80), as offsets
// from its centre: mud spots sit on the face, knots on the fur edge.
constexpr int kMudSpots[kCareSpotSlots][2] = {
    {-62, -40}, {62, -40}, {-78, 8}, {78, 8}, {-46, 50}, {46, 50}, {-40, -64}, {40, -64},
};
constexpr int kKnots[kCareSpotSlots][2] = {
    {-92, -30}, {92, -30}, {-100, 22}, {100, 22}, {-72, 64}, {72, 64}, {-56, -76}, {56, -76},
};

// The places of `mask` in bit order, skipping the first `skip` of them
// (already scrubbed / combed out).
template <typename Draw>
void EachSpot(uint16_t mask, uint8_t skip, Draw draw) {
    uint8_t seen = 0;
    for (uint8_t i = 0; i < kCareSpotSlots; ++i) {
        if (!(mask & (1u << i))) continue;
        if (seen++ < skip) continue;
        draw(i);
    }
}

// Bath: a splash of mud per spot still on the face.
void MudSpots(MonoFrame& f, const CareView& c, int hx, int hy) {
    if (!c.variant) return;
    EachSpot(c.variant, c.progress, [&](uint8_t i) {
        const int x = hx + kMudSpots[i][0], y = hy + kMudSpots[i][1];
        Disk(f, x, y, 11);
        Disk(f, x + 11, y - 7, 5);
        Disk(f, x - 9, y + 9, 4);
    });
}

// Brush: a tangle per knot not combed out yet (one back-and-forth each).
void FurKnots(MonoFrame& f, const CareView& c, int hx, int hy) {
    if (!c.variant) return;
    EachSpot(c.variant, static_cast<uint8_t>(c.progress / 2), [&](uint8_t i) {
        const int x = hx + kKnots[i][0], y = hy + kKnots[i][1];
        Disk(f, x, y, 14, false);
        Ring(f, x, y, 12, 9, 3);
        Stroke(f, x - 12, y - 8, x + 12, y + 8, 3);
        Stroke(f, x - 10, y + 9, x + 10, y - 9, 3);
    });
}

// Tickle (dated): a little TuanTuan on the right with head, belly and feet;
// wavy itch marks beside today's spot and the feather where the dial put
// it. A hit on the most ticklish spot shakes the big head with laughter.
constexpr int kTickleX = 470, kTickleY[3] = {82, 168, 246};

void TickleBody(MonoFrame& f, const CareView& c, int hx, int hy) {
    const uint8_t target = TickleTargetOf(c.variant), feather = TickleFeatherOf(c.variant);
    if (TickleLaughOf(c.variant)) {  // the big laugh: wobble lines around the head
        for (int side : {-1, 1})
            for (int i = 0; i < 2; ++i)
                Arc(f, hx + side * (112 + i * 14), hy - 8, 22, 36, side < 0 ? 150 : -30,
                    side < 0 ? 210 : 30, 5);
    }
    if (c.hearts) return;  // done: the hearts take the right side
    // The little body, top to bottom.
    CatHead(f, kTickleX, kTickleY[0], 30, Emotion::Happy, Eyes::ByEmotion);
    Blob(f, kTickleX, kTickleY[1], 46, 40, 5);
    for (int side : {-1, 1}) Blob(f, kTickleX + side * 26, kTickleY[2], 20, 13, 4);
    // Itch: wavy marks on both sides of the spot (not after the last one).
    if (c.progress < c.goal) {
        const int y = kTickleY[target];
        for (int side : {-1, 1})
            for (int k = 0; k < 2; ++k)
                Arc(f, kTickleX + side * (64 + 12 * k), y, 10, 18, side < 0 ? 120 : -60,
                    side < 0 ? 240 : 60, 3);
    }
    // The feather, pointing at its spot.
    CareArtIcon(f, static_cast<uint16_t>(CareArt::Feather), 620, kTickleY[feather], 40);
    ProgressDots(f, c.progress, c.goal, 640, 272);
}

// ---- C3: TuanTuan grows up (home picture, SittingCat at scale 100) ----
// Drawn around SittingCat at (cx, cy): GrowthBehind first (SittingCat
// covers the inner parts, so only the bigger edges show), GrowthFront
// after it. Stage 0 draws nothing: the V8 picture exactly.

void GrowthBehind(MonoFrame& f, uint8_t stage, int cx, int cy) {
    if (!stage || stage > 3) return;
    // Longer ears: the same ear triangles with the tips a little higher.
    const int longer = 6 + 4 * (stage - 1);
    for (int side : {-1, 1})
        FillTriangle(f, cx + side * 118, cy - 10, cx + side * 102, cy - 138 - longer, cx + side * 22,
                     cy - 84);
    // A fluffier tail: tufts along the outside of the curl.
    const int tufts = 3 + stage;
    for (int k = 0; k < tufts; ++k) {
        const double a = (-50 + 120.0 * k / (tufts - 1)) * kPi / 180.0;  // clear of the rail
        const int x = cx + 100 + static_cast<int>(std::lround((56 + 12) * std::cos(a)));
        const int y = cy + 150 + static_cast<int>(std::lround((46 + 12) * std::sin(a)));
        Disk(f, x, y, 7 + 2 * stage);
    }
    // Bigger (stage 2+): a soft fur fringe around the body.
    if (stage >= 2) {
        for (int deg = 160; deg < 380; deg += 22)
            Arc(f, cx, cy + 140, 104 + 10, 76 + 8, deg, deg + 12, 5);
    }
}

void GrowthFront(MonoFrame& f, uint8_t stage, int cx, int cy) {
    if (stage < 3) return;
    // Captain TuanTuan: a little star badge on the chest.
    Disk(f, cx - 58, cy + 132, 20, false);
    Ring(f, cx - 58, cy + 132, 20, 20, 4);
    FilledStar(f, cx - 58, cy + 133, 14);
}

// ---- C4: seasonal foods (food pictures 7..9, the FoodIcon size rules) ----

void SeasonalFoodIcon(MonoFrame& f, uint8_t picture, int cx, int cy, int r) {
    auto p = [r](int percent) { return r * percent / 100; };
    const int w = std::max(3, r / 10);
    switch (picture) {
        case 7: {  // dumpling: a half moon with pleats on top
            FillEllipse(f, cx, cy + p(14), p(86), p(52));
            FillEllipse(f, cx, cy + p(24), p(70), p(36), false);
            FillRect(f, cx - p(90), cy + p(18), p(180), p(54), false);
            Arc(f, cx, cy + p(18), p(86), p(52), 180, 360, w);
            Stroke(f, cx - p(86), cy + p(18), cx + p(86), cy + p(18), w);
            for (int k = -2; k <= 2; ++k)
                Stroke(f, cx + k * p(26), cy - p(28) + std::abs(k) * p(8), cx + k * p(22),
                       cy - p(8), w);
            break;
        }
        case 8:  // zongzi: a leaf triangle tied with a string
            FillTriangle(f, cx, cy - p(70), cx - p(78), cy + p(56), cx + p(78), cy + p(56));
            FillTriangle(f, cx, cy - p(46), cx - p(52), cy + p(40), cx + p(52), cy + p(40), false);
            FillTriangle(f, cx, cy - p(46), cx - p(52), cy + p(40), cx + p(52), cy + p(40), true, 6);
            Stroke(f, cx - p(60), cy + p(14), cx + p(60), cy + p(14), w + 2);
            break;
        case 9:  // mooncake: a round cake with a ring and a star
            Disk(f, cx, cy, p(76));
            Disk(f, cx, cy, p(62), false);
            Ring(f, cx, cy, p(48), p(48), w);
            FilledStar(f, cx, cy + p(2), p(26));
            for (int deg = 0; deg < 360; deg += 45)
                Disk(f, cx + static_cast<int>(std::lround(p(69) * std::cos(deg * kPi / 180))),
                     cy + static_cast<int>(std::lround(p(69) * std::sin(deg * kPi / 180))), p(8),
                     false);
            break;
        default:
            break;
    }
}
