#include "kid_app/scene_model.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace kid_app {

uint32_t PackScene(const SceneModel& scene) {
    return (static_cast<uint32_t>(scene.book_page) << 16) |
           (static_cast<uint32_t>(scene.emotion) << 8) |
           ((static_cast<uint32_t>(scene.home_selection) & 0x0F) << 4) |
           (static_cast<uint32_t>(scene.kind) & 0x0F);
}

SceneModel UnpackScene(uint32_t bits) {
    SceneModel scene;
    scene.kind = static_cast<SceneKind>(bits & 0x0F);
    scene.home_selection = static_cast<uint8_t>((bits >> 4) & 0x0F);
    scene.emotion = static_cast<Emotion>((bits >> 8) & 0xFF);
    scene.book_page = static_cast<uint16_t>(bits >> 16);
    return scene;
}

void MonoFrame::Clear() { pixels_.fill(0xFF); }

void MonoFrame::SetPixel(int x, int y, bool black) {
    if (x < 0 || x >= kWidth || y < 0 || y >= kHeight) return;
    const auto index = static_cast<size_t>(y * kStride + x / 8);
    const uint8_t mask = static_cast<uint8_t>(0x80u >> (x % 8));
    if (black) pixels_[index] &= static_cast<uint8_t>(~mask);
    else pixels_[index] |= mask;
}

void MonoFrame::PaintSpan(int x0, int x1, int y, uint8_t mask, bool black) {
    if (y < 0 || y >= kHeight) return;
    x0 = std::max(x0, 0);
    x1 = std::min(x1, kWidth - 1);
    if (x0 > x1) return;
    uint8_t* row = pixels_.data() + y * kStride;
    const int first = x0 / 8, last = x1 / 8;
    for (int b = first; b <= last; ++b) {
        uint8_t m = mask;
        if (b == first) m = static_cast<uint8_t>(m & (0xFFu >> (x0 % 8)));
        if (b == last) m = static_cast<uint8_t>(m & (0xFFu << (7 - x1 % 8)));
        if (black) row[b] = static_cast<uint8_t>(row[b] & ~m);
        else row[b] = static_cast<uint8_t>(row[b] | m);
    }
}

bool MonoFrame::IsBlack(int x, int y) const {
    if (x < 0 || x >= kWidth || y < 0 || y >= kHeight) return false;
    const auto index = static_cast<size_t>(y * kStride + x / 8);
    return (pixels_[index] & (0x80u >> (x % 8))) == 0;
}

namespace {

// ---- 1-bit drawing kit ------------------------------------------------------
// Every primitive takes an ink colour (true = black) so the same shapes can be
// drawn white-on-black, e.g. the selected rail card. Grey does not exist on
// the panel; shading fakes it with an ordered 4x4 Bayer dither. Shapes are
// filled as horizontal spans (MonoFrame::PaintSpan), never pixel by pixel:
// the frame lives in PSRAM and is re-rendered for every partial refresh.

constexpr int kW = MonoFrame::kWidth, kH = MonoFrame::kHeight;
constexpr double kPi = 3.14159265358979323846;
constexpr uint8_t kSolid = 0xFF;

int Clamp(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }

// 8-pixel dither mask for row y; level 0 (white) .. 16 (black).
uint8_t BayerMask(int y, int level) {
    static const uint8_t kM[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    uint8_t mask = 0;
    for (int i = 0; i < 8; ++i)
        if (kM[y & 3][i & 3] < level) mask = static_cast<uint8_t>(mask | (0x80u >> i));
    return mask;
}

// Largest dx with (dx/rx)^2 + (dy/ry)^2 <= 1, or -1 when row dy misses.
int HalfWidth(int dy, int rx, int ry) {
    if (rx <= 0 || ry <= 0 || dy < -ry || dy > ry) return -1;
    const int64_t rx2 = int64_t{rx} * rx, ry2 = int64_t{ry} * ry;
    const int64_t limit = rx2 * ry2 - int64_t{dy} * dy * rx2;
    int dx = static_cast<int>(std::sqrt(static_cast<float>(limit) / static_cast<float>(ry2)));
    while (dx > 0 && int64_t{dx} * dx * ry2 > limit) --dx;
    while (int64_t{dx + 1} * (dx + 1) * ry2 <= limit) ++dx;
    return dx;
}

void Span(MonoFrame& f, int x0, int x1, int y, bool ink, uint8_t mask = kSolid) {
    f.PaintSpan(x0, x1, y, mask, ink);
}

void FillEllipse(MonoFrame& f, int cx, int cy, int rx, int ry, bool ink = true) {
    for (int y = Clamp(cy - ry, 0, kH - 1); y <= Clamp(cy + ry, 0, kH - 1); ++y) {
        const int hw = HalfWidth(y - cy, rx, ry);
        if (hw >= 0) Span(f, cx - hw, cx + hw, y, ink);
    }
}

// Elliptical band of width w (inner edge = outer shrunk by w).
void Ring(MonoFrame& f, int cx, int cy, int rx, int ry, int w, bool ink = true) {
    for (int y = Clamp(cy - ry, 0, kH - 1); y <= Clamp(cy + ry, 0, kH - 1); ++y) {
        const int outer = HalfWidth(y - cy, rx, ry);
        if (outer < 0) continue;
        const int inner = HalfWidth(y - cy, rx - w, ry - w);
        if (inner < 0) {
            Span(f, cx - outer, cx + outer, y, ink);
        } else {
            Span(f, cx - outer, cx - inner - 1, y, ink);
            Span(f, cx + inner + 1, cx + outer, y, ink);
        }
    }
}

void Disk(MonoFrame& f, int cx, int cy, int r, bool ink = true) {
    FillEllipse(f, cx, cy, r, r, ink);
}

// White body with a black outline: the basic "sticker" shape.
void Blob(MonoFrame& f, int cx, int cy, int rx, int ry, int w) {
    FillEllipse(f, cx, cy, rx, ry, false);
    Ring(f, cx, cy, rx, ry, w);
}

// Dithered ellipse; only adds ink, white stays white under it.
void Shade(MonoFrame& f, int cx, int cy, int rx, int ry, int level) {
    for (int y = Clamp(cy - ry, 0, kH - 1); y <= Clamp(cy + ry, 0, kH - 1); ++y) {
        const int hw = HalfWidth(y - cy, rx, ry);
        if (hw >= 0) Span(f, cx - hw, cx + hw, y, true, BayerMask(y, level));
    }
}

void ShadeRect(MonoFrame& f, int x0, int y0, int w, int h, int level) {
    for (int y = Clamp(y0, 0, kH - 1); y < Clamp(y0 + h, 0, kH); ++y)
        Span(f, x0, x0 + w - 1, y, true, BayerMask(y, level));
}

// Round-capped stroke: disks stamped along a Bresenham line. Stamping every
// r/2 steps instead of every pixel leaves < 0.3 px of scalloping.
void Stroke(MonoFrame& f, int x0, int y0, int x1, int y1, int w, bool ink = true) {
    const int r = w / 2, every = std::max(1, r / 2);
    const int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (int i = 0;; ++i) {
        const bool last = x0 == x1 && y0 == y1;
        if (r <= 0) f.SetPixel(x0, y0, ink);
        else if (last || i % every == 0) Disk(f, x0, y0, r, ink);
        if (last) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// Elliptical arc, degrees clockwise from +x (screen y grows downwards):
// 0..180 is the lower half, 180..360 the upper half.
void Arc(MonoFrame& f, int cx, int cy, int rx, int ry, int a0, int a1, int w, bool ink = true) {
    // Polyline with segments about a quarter of the stroke width long: short
    // enough to look round, long enough for Stroke() to skip disk stamps.
    const int length = std::abs(a1 - a0) * std::max(rx, ry) * 314 / 18000;
    const int steps = std::max(8, length / std::max(2, w / 4));
    int px = 0, py = 0;
    for (int i = 0; i <= steps; ++i) {
        const double a = (a0 + (a1 - a0) * static_cast<double>(i) / steps) * kPi / 180.0;
        const int x = cx + static_cast<int>(std::lround(rx * std::cos(a)));
        const int y = cy + static_cast<int>(std::lround(ry * std::sin(a)));
        if (i > 0) Stroke(f, px, py, x, y, w, ink);
        px = x;
        py = y;
    }
}

// level < 16 dithers the fill (see BayerMask) instead of painting it solid.
void FillTriangle(MonoFrame& f, int x0, int y0, int x1, int y1, int x2, int y2, bool ink = true,
                  int level = 16) {
    const int xs[3] = {x0, x1, x2}, ys[3] = {y0, y1, y2};
    const int miny = Clamp(std::min({y0, y1, y2}), 0, kH - 1);
    const int maxy = Clamp(std::max({y0, y1, y2}), 0, kH - 1);
    for (int y = miny; y <= maxy; ++y) {
        float lo = 1e9f, hi = -1e9f;
        for (int e = 0; e < 3; ++e) {
            const int ax = xs[e], ay = ys[e], bx = xs[(e + 1) % 3], by = ys[(e + 1) % 3];
            if (y < std::min(ay, by) || y > std::max(ay, by)) continue;
            if (ay == by) {
                lo = std::min(lo, static_cast<float>(std::min(ax, bx)));
                hi = std::max(hi, static_cast<float>(std::max(ax, bx)));
                continue;
            }
            const float x = ax + static_cast<float>(y - ay) * (bx - ax) / static_cast<float>(by - ay);
            lo = std::min(lo, x);
            hi = std::max(hi, x);
        }
        if (lo > hi) continue;
        Span(f, static_cast<int>(std::ceil(lo)), static_cast<int>(std::floor(hi)), y, ink,
             level >= 16 ? kSolid : BayerMask(y, level));
    }
}

// Horizontal extent of a rounded rectangle on row py: [*l, *r], or false.
bool RoundRectRow(int py, int x, int y, int w, int h, int r, int* l, int* rt) {
    if (w <= 0 || h <= 0 || py < y || py >= y + h) return false;
    const int qy = Clamp(py, y + r, y + h - 1 - r), d = py - qy;
    const int hw = d == 0 ? r : HalfWidth(d, r, r);
    if (hw < 0) return false;
    *l = x + r - hw;
    *rt = x + w - 1 - r + hw;
    return true;
}

void FillRoundRect(MonoFrame& f, int x, int y, int w, int h, int r, bool ink = true) {
    for (int py = Clamp(y, 0, kH - 1); py < Clamp(y + h, 0, kH); ++py) {
        int l = 0, rt = 0;
        if (RoundRectRow(py, x, y, w, h, r, &l, &rt)) Span(f, l, rt, py, ink);
    }
}

void RoundRectRing(MonoFrame& f, int x, int y, int w, int h, int r, int t, bool ink = true) {
    for (int py = Clamp(y, 0, kH - 1); py < Clamp(y + h, 0, kH); ++py) {
        int l = 0, rt = 0, il = 0, irt = 0;
        if (!RoundRectRow(py, x, y, w, h, r, &l, &rt)) continue;
        if (!RoundRectRow(py, x + t, y + t, w - 2 * t, h - 2 * t, std::max(0, r - t), &il, &irt)) {
            Span(f, l, rt, py, ink);
        } else {
            Span(f, l, il - 1, py, ink);
            Span(f, irt + 1, rt, py, ink);
        }
    }
}

// Four-point twinkle star.
void Sparkle(MonoFrame& f, int cx, int cy, int r, bool ink = true) {
    const int k = std::max(1, r / 4);
    FillTriangle(f, cx, cy - r, cx - k, cy, cx + k, cy, ink);
    FillTriangle(f, cx, cy + r, cx - k, cy, cx + k, cy, ink);
    FillTriangle(f, cx - r, cy, cx, cy - k, cx, cy + k, ink);
    FillTriangle(f, cx + r, cy, cx, cy - k, cx, cy + k, ink);
}

// Bold "Z" for sleep bubbles.
void LetterZ(MonoFrame& f, int x, int y, int size, int w, bool ink = true) {
    Stroke(f, x, y, x + size, y, w, ink);
    Stroke(f, x + size, y, x, y + size, w, ink);
    Stroke(f, x, y + size, x + size, y + size, w, ink);
}

// Thick-stroked "?" that floats beside a curious pet.
void QuestionMark(MonoFrame& f, int cx, int cy, int r, int w) {
    Arc(f, cx, cy - r, r, r, 180, 400, w);
    const int bx = cx + static_cast<int>(std::lround(r * std::cos(40 * kPi / 180)));
    const int by = cy - r + static_cast<int>(std::lround(r * std::sin(40 * kPi / 180)));
    Stroke(f, bx, by, cx, cy + r / 2, w);
    Disk(f, cx, cy + r + w + 2, w / 2 + 1);
}

// ---- TuanTuan, the pet ---------------------------------------------------------
// A round tabby kitten. (cx, cy) is the head centre, scale is percent of the
// home size. Shapes are drawn back to front: ears, head, markings, face.

// asleep: closed eyes whatever the emotion. halo: a white rim around head
// and ears so the outline stays visible on a black background.
void CatHead(MonoFrame& f, int cx, int cy, int scale, Emotion emotion, bool asleep = false,
             bool halo = false) {
    auto s = [scale](int d) { return d * scale / 100; };
    const int line = std::max(2, s(7));
    if (halo) {
        for (int side : {-1, 1})
            FillTriangle(f, cx + side * s(130), cy - s(4), cx + side * s(106), cy - s(154),
                         cx + side * s(12), cy - s(88), false);
        FillEllipse(f, cx, cy, s(124) + 8, s(100) + 8, false);
    }

    // Ears: solid black triangles with a dithered inner ear.
    for (int side : {-1, 1}) {
        FillTriangle(f, cx + side * s(118), cy - s(10), cx + side * s(100), cy - s(138),
                     cx + side * s(22), cy - s(84));
        const int ix0 = cx + side * s(96), iy0 = cy - s(40), ix1 = cx + side * s(92),
                  iy1 = cy - s(108), ix2 = cx + side * s(46), iy2 = cy - s(78);
        FillTriangle(f, ix0, iy0, ix1, iy1, ix2, iy2, false);
        FillTriangle(f, ix0, iy0, ix1, iy1, ix2, iy2, true, 5);
    }
    Blob(f, cx, cy, s(124), s(100), line);

    // Tabby stripes on the forehead.
    const int stripe = std::max(2, s(9));
    Stroke(f, cx - s(22), cy - s(90), cx - s(16), cy - s(66), stripe);
    Stroke(f, cx, cy - s(94), cx, cy - s(64), stripe);
    Stroke(f, cx + s(22), cy - s(90), cx + s(16), cy - s(66), stripe);

    // Blush cheeks.
    Shade(f, cx - s(80), cy + s(36), s(22), s(13), 6);
    Shade(f, cx + s(80), cy + s(36), s(22), s(13), 6);

    // Whiskers reach past the cheeks.
    const int whisker = std::max(1, s(4));
    for (int side : {-1, 1}) {
        Stroke(f, cx + side * s(100), cy + s(22), cx + side * s(150), cy + s(12), whisker);
        Stroke(f, cx + side * s(100), cy + s(40), cx + side * s(152), cy + s(46), whisker);
    }

    // Eyes.
    const int ex = s(50), ey = cy + s(2);
    if (asleep) {  // peaceful closed eyes
        Arc(f, cx - ex, ey - s(6), s(20), s(13), 15, 165, std::max(2, s(8)));
        Arc(f, cx + ex, ey - s(6), s(20), s(13), 15, 165, std::max(2, s(8)));
    } else switch (emotion) {
        case Emotion::Happy:  // ^ ^
            Arc(f, cx - ex, ey + s(8), s(20), s(17), 195, 345, std::max(2, s(9)));
            Arc(f, cx + ex, ey + s(8), s(20), s(17), 195, 345, std::max(2, s(9)));
            break;
        case Emotion::Sleepy:  // drowsy: half-lidded lines with lashes
            for (int side : {-1, 1}) {
                Arc(f, cx + side * ex, ey - s(4), s(20), s(14), 15, 165, std::max(2, s(8)));
                Stroke(f, cx + side * (ex + s(18)), ey + s(4), cx + side * (ex + s(26)), ey + s(12),
                       std::max(1, s(4)));
            }
            break;
        case Emotion::Surprised:  // wide rings, small pupils
            for (int side : {-1, 1}) {
                Ring(f, cx + side * ex, ey, s(22), s(26), std::max(2, s(6)));
                Disk(f, cx + side * ex, ey, std::max(2, s(9)));
            }
            break;
        case Emotion::Curious:  // big shiny eyes glancing up
        case Emotion::Neutral:
        default: {
            const bool curious = emotion == Emotion::Curious;
            const int rx = s(curious ? 21 : 18), ry = s(curious ? 25 : 22);
            for (int side : {-1, 1}) {
                const int x = cx + side * ex;
                FillEllipse(f, x, ey, rx, ry);
                Disk(f, x + s(curious ? 6 : -5), ey - s(curious ? 10 : 8), std::max(1, s(7)), false);
                Disk(f, x + s(curious ? -6 : 6), ey + s(8), std::max(1, s(3)), false);
            }
            break;
        }
    }

    // Nose: small rounded triangle.
    FillTriangle(f, cx - s(11), cy + s(24), cx + s(11), cy + s(24), cx, cy + s(35));
    Disk(f, cx, cy + s(27), std::max(1, s(5)));

    // Mouth.
    const int mw = std::max(2, s(5));
    if (emotion == Emotion::Surprised) {
        Ring(f, cx, cy + s(58), s(12), s(15), mw);
    } else if (emotion == Emotion::Happy) {
        // Open laughing mouth with a dithered tongue.
        for (int dy = 0; dy <= s(26); ++dy) {
            const int hw = HalfWidth(dy, s(22), s(26));
            Span(f, cx - hw, cx + hw, cy + s(42) + dy, true);
        }
        FillEllipse(f, cx, cy + s(60), s(12), s(7), false);
        Shade(f, cx, cy + s(60), s(12), s(7), 6);
        Arc(f, cx - s(11), cy + s(36), s(11), s(9), 0, 180, mw);
        Arc(f, cx + s(11), cy + s(36), s(11), s(9), 0, 180, mw);
    } else if (emotion == Emotion::Curious) {
        Ring(f, cx + s(6), cy + s(54), s(8), s(8), mw);
    } else {
        // Classic cat "w" mouth.
        const int r = emotion == Emotion::Sleepy ? s(8) : s(11);
        Arc(f, cx - r, cy + s(35), r, s(9), 0, 180, mw);
        Arc(f, cx + r, cy + s(35), r, s(9), 0, 180, mw);
    }
}

// Sitting pose for the home screen: tail, body, paws, collar bell, head.
void SittingCat(MonoFrame& f, int cx, int cy, Emotion emotion) {
    // Tail curling up on the right.
    Arc(f, cx + 100, cy + 150, 56, 46, -60, 100, 22);
    Arc(f, cx + 100, cy + 150, 56, 46, -60, 100, 10, false);
    Blob(f, cx, cy + 140, 104, 76, 7);
    Shade(f, cx, cy + 156, 46, 36, 2);  // lighter belly fur
    for (int side : {-1, 1}) {
        const int px = cx + side * 44, py = cy + 196;
        Blob(f, px, py, 30, 18, 6);
        Stroke(f, px - 8, py + 4, px - 8, py + 14, 4);
        Stroke(f, px + 8, py + 4, px + 8, py + 14, 4);
    }
    CatHead(f, cx, cy, 100, emotion);
    // Bell on the collar, just under the chin.
    Blob(f, cx, cy + 110, 14, 13, 4);
    Shade(f, cx, cy + 110, 10, 9, 8);
    Stroke(f, cx, cy + 112, cx, cy + 122, 3);
    if (emotion == Emotion::Curious) QuestionMark(f, cx + 168, cy - 96, 20, 9);
    if (emotion == Emotion::Surprised) {
        Stroke(f, cx - 150, cy - 110, cx - 136, cy - 82, 8);
        Stroke(f, cx - 176, cy - 82, cx - 152, cy - 66, 8);
        Stroke(f, cx + 150, cy - 110, cx + 136, cy - 82, 8);
        Stroke(f, cx + 176, cy - 82, cx + 152, cy - 66, 8);
    }
    if (emotion == Emotion::Sleepy) {
        LetterZ(f, cx + 160, cy - 80, 22, 6);
        LetterZ(f, cx + 196, cy - 120, 30, 7);
    }
}

// ---- Home action rail -----------------------------------------------------------

// Rail icons: bold strokes centred on (cx, cy); `ink` false draws them
// white for the selected (black) card.
void RailIcon(MonoFrame& f, int slot, int cx, int cy, bool ink) {
    switch (slot) {
        case 0:  // Chat: speech bubble with three dots
            RoundRectRing(f, cx - 38, cy - 30, 76, 50, 18, 6, ink);
            FillTriangle(f, cx - 22, cy + 14, cx - 4, cy + 14, cx - 28, cy + 34, ink);
            FillTriangle(f, cx - 18, cy + 13, cx - 8, cy + 13, cx - 22, cy + 24, !ink);
            for (int i = -1; i <= 1; ++i) Disk(f, cx + i * 17, cy - 5, 6, ink);
            break;
        case 1:  // Story: open book
            for (int side : {-1, 1}) {
                Stroke(f, cx, cy - 18, cx + side * 20, cy - 26, 6, ink);
                Stroke(f, cx + side * 20, cy - 26, cx + side * 40, cy - 22, 6, ink);
                Stroke(f, cx + side * 40, cy - 22, cx + side * 40, cy + 22, 6, ink);
                Stroke(f, cx + side * 40, cy + 22, cx + side * 20, cy + 18, 6, ink);
                Stroke(f, cx + side * 20, cy + 18, cx, cy + 26, 6, ink);
                for (int i = 0; i < 3; ++i)
                    Stroke(f, cx + side * 10, cy - 10 + i * 11, cx + side * 30, cy - 12 + i * 11, 3, ink);
            }
            Stroke(f, cx, cy - 18, cx, cy + 26, 6, ink);
            break;
        case 2:  // Game: die showing five
            RoundRectRing(f, cx - 32, cy - 32, 64, 64, 14, 6, ink);
            for (int dx : {-14, 14})
                for (int dy : {-14, 14}) Disk(f, cx + dx, cy + dy, 6, ink);
            Disk(f, cx, cy, 6, ink);
            break;
        case 3:  // Feed: a fish
            Ring(f, cx - 8, cy, 30, 20, 6, ink);
            FillTriangle(f, cx + 16, cy, cx + 42, cy - 20, cx + 42, cy + 20, ink);
            Disk(f, cx - 22, cy - 4, 5, ink);
            Arc(f, cx - 2, cy, 10, 14, -60, 60, 4, ink);
            break;
        case 4:  // Sleep: crescent moon and a star
            Disk(f, cx - 6, cy + 2, 30, ink);
            Disk(f, cx + 8, cy - 8, 26, !ink);
            Sparkle(f, cx + 26, cy - 22, 13, ink);
            Sparkle(f, cx + 34, cy + 16, 7, ink);
            break;
        default:
            break;
    }
}

void ActionRail(MonoFrame& f, uint8_t selection) {
    const int top = SceneComposer::kRailTop, slot_w = SceneComposer::kRailSlotWidth;
    for (int slot = 0; slot < 5; ++slot) {
        const int x = slot * slot_w + 8, y = top + 6, w = slot_w - 16, h = 88;
        const bool selected = slot == selection;
        if (selected) {
            // Inverted card: the one thing on screen that is mostly black.
            FillRoundRect(f, x, y, w, h, 20);
        } else {
            RoundRectRing(f, x, y, w, h, 20, 3);
        }
        RailIcon(f, slot, x + w / 2, y + h / 2, !selected);
    }
}

// ---- Scenes --------------------------------------------------------------------

void HomeScene(MonoFrame& f, const SceneModel& model) {
    // Background twinkles and a dithered cushion under the pet.
    Sparkle(f, 110, 90, 18);
    Sparkle(f, 160, 150, 9);
    Sparkle(f, 690, 80, 14);
    Sparkle(f, 640, 220, 10);
    Sparkle(f, 96, 280, 8);
    Disk(f, 720, 300, 4);
    Disk(f, 70, 190, 4);
    Shade(f, 400, 356, 200, 20, 6);
    Ring(f, 400, 356, 200, 20, 3);
    SittingCat(f, 400, 158, model.emotion);
    ActionRail(f, model.home_selection);
}

void ReaderScene(MonoFrame& f, const SceneModel& model) {
    // Hardcover, two pages, shaded gutter and page edges at the bottom.
    FillRoundRect(f, 24, 18, 752, 446, 24);
    for (int side : {0, 1}) {
        const int x = side ? 404 : 44;
        FillRoundRect(f, x, 34, 352, 404, 16, false);
        for (int i = 0; i < 3; ++i) {  // gutter shadow, darker towards the spine
            const int gx = side ? 404 + i * 5 : 391 - i * 5;
            ShadeRect(f, gx, 44, 5, 384, 6 - i * 2);
        }
        for (int i = 0; i < 2; ++i)
            Stroke(f, x + 18, 444 + i * 6, x + 334, 444 + i * 6, 1, false);
    }
    CatHead(f, 220, 236, 62, model.emotion);
    Sparkle(f, 96, 86, 12);
    Sparkle(f, 340, 96, 8);
    Sparkle(f, 330, 360, 10);
    // Right page: soft text bars and a page indicator.
    const int bars[] = {260, 230, 270, 200, 250, 150};
    for (int i = 0; i < 6; ++i) {
        const int y = 92 + i * 44;
        Stroke(f, 452, y + 8, 452 + bars[i], y + 8, 8);
    }
    const int indicators = 5;
    for (int i = 0; i < indicators; ++i) {
        const int x = 520 + i * 30, y = 400;
        if (static_cast<int>(model.book_page % indicators) == i) Disk(f, x, y, 9);
        else Ring(f, x, y, 7, 7, 3);
    }
}

// Fixed picture kept on the unpowered ePaper: night sky, moon, and TuanTuan
// curled up asleep on a cushion.
void SleepScene(MonoFrame& f) {
    // Black sky above a rolling white hill with a stippled horizon.
    for (int y = 0; y < kH; ++y) Span(f, 0, kW - 1, y, true);
    FillEllipse(f, 400, 660, 720, 350, false);
    for (int i = 0; i < 4; ++i)
        for (int y = 280; y < kH; ++y) {
            const int outer = HalfWidth(y - 660, 726 + i * 6, 356 + i * 6);
            if (outer < 0) continue;
            const int inner = HalfWidth(y - 660, 720 + i * 6, 350 + i * 6);
            const uint8_t light = static_cast<uint8_t>(~BayerMask(y, 6 + i * 3));
            if (inner < 0) {
                Span(f, 400 - outer, 400 + outer, y, false, light);
            } else {
                Span(f, 400 - outer, 400 - inner - 1, y, false, light);
                Span(f, 400 + inner + 1, 400 + outer, y, false, light);
            }
        }

    // Moon with a halo ring, and stars.
    Disk(f, 676, 88, 46, false);
    Disk(f, 700, 70, 40, true);
    Ring(f, 676, 88, 64, 64, 2, false);
    const int stars[][3] = {{60, 40, 12}, {250, 70, 8}, {360, 40, 11}, {470, 110, 7},
                            {560, 40, 9}, {430, 200, 6}, {770, 190, 8}, {30, 200, 6}};
    for (const auto& s : stars) Sparkle(f, s[0], s[1], s[2], false);
    const int dots[][2] = {{300, 20}, {400, 150}, {120, 230}, {600, 150}, {730, 250},
                           {520, 230}, {500, 20}, {190, 30}};
    for (const auto& d : dots) Disk(f, d[0], d[1], 2, false);

    // Cushion.
    FillEllipse(f, 420, 352, 240, 24, false);
    Shade(f, 420, 352, 240, 24, 6);
    Ring(f, 420, 352, 240, 24, 4);

    // Curled-up body, tail wrapped along the front, head resting on paws.
    const int bx = 480, by = 290;
    FillEllipse(f, bx, by, 150, 74, false);  // white halo against the sky
    Blob(f, bx, by, 142, 66, 7);
    Stroke(f, bx + 10, by - 60, bx + 4, by - 38, 9);  // back stripes
    Stroke(f, bx + 44, by - 56, bx + 36, by - 34, 9);
    Stroke(f, bx + 78, by - 44, bx + 70, by - 24, 9);
    Arc(f, bx - 20, by + 18, 140, 42, 0, 118, 22);
    Arc(f, bx - 20, by + 18, 140, 42, 3, 115, 10, false);
    const int hx = 330, hy = 280;
    CatHead(f, hx, hy, 72, Emotion::Neutral, /*asleep=*/true, /*halo=*/true);
    Blob(f, hx - 32, hy + 70, 26, 14, 5);
    Blob(f, hx + 32, hy + 70, 26, 14, 5);

    // Zzz rising into the sky.
    LetterZ(f, 196, 170, 20, 6, false);
    LetterZ(f, 146, 110, 28, 7, false);
    LetterZ(f, 84, 42, 38, 8, false);
}

}  // namespace

void SceneComposer::Render(const SceneModel& model, MonoFrame& output) {
    output.Clear();
    if (model.kind == SceneKind::Reader) {
        ReaderScene(output, model);
    } else if (model.kind == SceneKind::Sleep) {
        SleepScene(output);
    } else {
        HomeScene(output, model);
    }
}

}  // namespace kid_app
