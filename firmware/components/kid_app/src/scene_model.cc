#include "kid_app/scene_model.h"

#include "kid_app/activity_list.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace kid_app {

uint32_t PackScene(const SceneModel& scene) {
    return (static_cast<uint32_t>(scene.book_page) << 16) |
           ((static_cast<uint32_t>(scene.pose) & 0x0F) << 12) |
           ((static_cast<uint32_t>(scene.emotion) & 0x0F) << 8) |
           ((static_cast<uint32_t>(scene.home_selection) & 0x0F) << 4) |
           (static_cast<uint32_t>(scene.kind) & 0x0F);
}

SceneModel UnpackScene(uint32_t bits) {
    SceneModel scene;
    scene.kind = static_cast<SceneKind>(bits & 0x0F);
    scene.home_selection = static_cast<uint8_t>((bits >> 4) & 0x0F);
    scene.emotion = static_cast<Emotion>((bits >> 8) & 0x0F);
    scene.pose = static_cast<ScenePose>((bits >> 12) & 0x0F);
    scene.book_page = static_cast<uint16_t>(bits >> 16);
    return scene;
}

SceneModel BookMissingScene(BookMissing reason) {
    SceneModel scene{};
    scene.kind = SceneKind::Reader;
    scene.emotion = Emotion::Curious;
    scene.book_page = static_cast<uint16_t>(static_cast<uint16_t>(reason) << 8);
    return scene;
}

BookMissing BookMissingOf(const SceneModel& scene) {
    if (scene.kind != SceneKind::Reader || (scene.book_page & 0xFF) != 0) return BookMissing::None;
    const uint16_t reason = static_cast<uint16_t>(scene.book_page >> 8);
    return reason <= static_cast<uint16_t>(BookMissing::BadBook) ? static_cast<BookMissing>(reason)
                                                                 : BookMissing::None;
}

uint32_t PackLook(const HomeLook& look) {
    uint32_t bits = 0;
    for (size_t i = 0; i < look.worn.size(); ++i)
        if (look.worn[i] < 16) bits |= uint32_t(look.worn[i]) << (4 * i);
    if (look.need < 16) bits |= uint32_t(look.need) << 20;
    if (look.need_level < 4) bits |= uint32_t(look.need_level) << 24;
    if (look.festival < 16) bits |= uint32_t(look.festival) << 26;
    return bits;
}

HomeLook UnpackLook(uint32_t bits) {
    HomeLook look{};
    for (size_t i = 0; i < look.worn.size(); ++i)
        look.worn[i] = static_cast<uint8_t>((bits >> (4 * i)) & 0x0F);
    look.need = static_cast<uint8_t>((bits >> 20) & 0x0F);
    look.need_level = static_cast<uint8_t>((bits >> 24) & 0x03);
    look.festival = static_cast<uint8_t>((bits >> 26) & 0x0F);
    return look;
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

// Eye override for animation poses: Closed whatever the emotion (blink,
// asleep), WinkLeft closes only the viewer's left eye, Laugh is ^ ^ and
// Spiral is dizzy swirls.
enum class Eyes : uint8_t { ByEmotion, Closed, WinkLeft, Laugh, Spiral };

// Archimedean swirl from the centre out, about two turns.
void Spiral(MonoFrame& f, int cx, int cy, int r, int w) {
    int px = cx, py = cy;
    const int steps = 48;
    for (int i = 1; i <= steps; ++i) {
        const double a = i * 4.0 * kPi / steps;
        const double rr = r * static_cast<double>(i) / steps;
        const int x = cx + static_cast<int>(std::lround(rr * std::cos(a)));
        const int y = cy + static_cast<int>(std::lround(rr * std::sin(a)));
        Stroke(f, px, py, x, y, w);
        px = x;
        py = y;
    }
}

// halo: a white rim around head and ears so the outline stays visible on a
// black background.
void CatHead(MonoFrame& f, int cx, int cy, int scale, Emotion emotion,
             Eyes eyes = Eyes::ByEmotion, bool halo = false) {
    auto s = [scale](int d) { return d * scale / 100; };
    const int line = std::max(2, s(7));
    const int perk = emotion == Emotion::Listening ? s(24) : 0;  // ears up, listening
    if (halo) {
        for (int side : {-1, 1})
            FillTriangle(f, cx + side * s(130), cy - s(4), cx + side * s(106), cy - s(154) - perk,
                         cx + side * s(12), cy - s(88), false);
        FillEllipse(f, cx, cy, s(124) + 8, s(100) + 8, false);
    }

    // Ears: solid black triangles with a dithered inner ear.
    for (int side : {-1, 1}) {
        FillTriangle(f, cx + side * s(118), cy - s(10), cx + side * s(100), cy - s(138) - perk,
                     cx + side * s(22), cy - s(84));
        const int ix0 = cx + side * s(96), iy0 = cy - s(40), ix1 = cx + side * s(92),
                  iy1 = cy - s(108) - perk, ix2 = cx + side * s(46), iy2 = cy - s(78);
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
    for (int side : {-1, 1}) {
        const int x = cx + side * ex;
        if (eyes == Eyes::Closed) {  // peaceful closed eyes
            Arc(f, x, ey - s(6), s(20), s(13), 15, 165, std::max(2, s(8)));
            continue;
        }
        if (eyes == Eyes::Laugh || (eyes == Eyes::WinkLeft && side < 0)) {  // ^
            Arc(f, x, ey + s(8), s(20), s(17), 195, 345, std::max(2, s(9)));
            continue;
        }
        if (eyes == Eyes::Spiral) {
            Spiral(f, x, ey, s(22), std::max(2, s(5)));
            continue;
        }
        switch (emotion) {
            case Emotion::Happy:  // ^ ^
                Arc(f, x, ey + s(8), s(20), s(17), 195, 345, std::max(2, s(9)));
                break;
            case Emotion::Sleepy:  // drowsy: half-lidded lines with lashes
                Arc(f, x, ey - s(4), s(20), s(14), 15, 165, std::max(2, s(8)));
                Stroke(f, cx + side * (ex + s(18)), ey + s(4), cx + side * (ex + s(26)), ey + s(12),
                       std::max(1, s(4)));
                break;
            case Emotion::Surprised:  // wide rings, small pupils
                Ring(f, x, ey, s(22), s(26), std::max(2, s(6)));
                Disk(f, x, ey, std::max(2, s(9)));
                break;
            case Emotion::Listening:  // big eyes glancing towards the raised ear
                FillEllipse(f, x, ey, s(20), s(24));
                Disk(f, x + s(8), ey - s(8), std::max(1, s(7)), false);
                Disk(f, x - s(5), ey + s(9), std::max(1, s(3)), false);
                break;
            case Emotion::Thinking:  // open rings, pupils looking up at the cloud
                Ring(f, x, ey, s(21), s(24), std::max(2, s(6)));
                Disk(f, x + s(5), ey - s(10), std::max(2, s(10)));
                break;
            case Emotion::Curious:  // big shiny eyes glancing up
            case Emotion::Neutral:
            default: {
                const bool curious = emotion == Emotion::Curious;
                FillEllipse(f, x, ey, s(curious ? 21 : 18), s(curious ? 25 : 22));
                Disk(f, x + s(curious ? 6 : -5), ey - s(curious ? 10 : 8), std::max(1, s(7)), false);
                Disk(f, x + s(curious ? -6 : 6), ey + s(8), std::max(1, s(3)), false);
                break;
            }
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
    } else if (emotion == Emotion::Speaking) {
        // Round open mouth with a dithered tongue, under the "w" lips.
        FillEllipse(f, cx, cy + s(56), s(17), s(16));
        FillEllipse(f, cx, cy + s(64), s(10), s(6), false);
        Shade(f, cx, cy + s(64), s(10), s(6), 6);
        Arc(f, cx - s(11), cy + s(36), s(11), s(9), 0, 180, mw);
        Arc(f, cx + s(11), cy + s(36), s(11), s(9), 0, 180, mw);
    } else if (emotion == Emotion::Thinking) {
        // Small pondering squiggle, a little off to the side.
        Arc(f, cx - s(2), cy + s(52), s(8), s(5), 180, 360, mw);
        Arc(f, cx + s(14), cy + s(52), s(8), s(5), 0, 180, mw);
    } else {
        // Classic cat "w" mouth.
        const int r = emotion == Emotion::Sleepy ? s(8) : s(11);
        Arc(f, cx - r, cy + s(35), r, s(9), 0, 180, mw);
        Arc(f, cx + r, cy + s(35), r, s(9), 0, 180, mw);
    }
}

// Filled heart: two lobes centred on row cy, tip r * 4 / 3 below.
void Heart(MonoFrame& f, int cx, int cy, int r) {
    Disk(f, cx - r / 2, cy, r / 2 + 2);
    Disk(f, cx + r / 2, cy, r / 2 + 2);
    FillTriangle(f, cx - r - 1, cy + 2, cx + r + 1, cy + 2, cx, cy + r * 4 / 3);
}

// Good-morning stretch (ScenePose::Sunrise): both arms up beside the head.
// Arms go behind the head, the paws are drawn on top of it.
void StretchArms(MonoFrame& f, int cx, int cy) {
    for (int side : {-1, 1}) {
        Stroke(f, cx + side * 64, cy + 118, cx + side * 150, cy - 26, 34);
        Stroke(f, cx + side * 64, cy + 118, cx + side * 150, cy - 26, 20, false);
    }
}

void StretchPaws(MonoFrame& f, int cx, int cy) {
    for (int side : {-1, 1}) {
        const int px = cx + side * 156, py = cy - 42;
        Blob(f, px, py, 28, 30, 6);
        for (int dx : {-10, 0, 10}) Stroke(f, px + dx, py - 22, px + dx, py - 12, 4);
        // Little "stretch" lines above the paws.
        Arc(f, px, py, 50, 50, side < 0 ? 215 : 285, side < 0 ? 255 : 325, 5);
    }
}

// Good-morning sky (V3-04), top-left corner: a sun whose white halo clears
// the night twinkles. Rising: a cloud hides its lower half, short rays and
// sleepy eyes. Risen: rays all around and a smile.
void MorningSun(MonoFrame& f, bool risen) {
    const int cx = 132, cy = 140, r = 40;
    FillEllipse(f, cx, cy, 92, 88, false);
    const int rays = 12;
    for (int i = 0; i < rays; ++i) {
        const double a = (i * 360.0 / rays + 15) * kPi / 180.0;
        if (!risen && std::sin(a) > -0.2) continue;  // behind the cloud
        const int r0 = r + 12, r1 = r + (risen ? 40 : 26);
        Stroke(f, cx + static_cast<int>(std::lround(r0 * std::cos(a))),
               cy + static_cast<int>(std::lround(r0 * std::sin(a))),
               cx + static_cast<int>(std::lround(r1 * std::cos(a))),
               cy + static_cast<int>(std::lround(r1 * std::sin(a))), 7);
    }
    Blob(f, cx, cy, r, r, 6);
    Shade(f, cx, cy, r - 8, r - 8, 3);
    if (risen) {
        for (int side : {-1, 1}) Disk(f, cx + side * 14, cy - 6, 5);
        Arc(f, cx, cy + 4, 16, 12, 20, 160, 5);
    } else {
        for (int side : {-1, 1}) Arc(f, cx + side * 14, cy - 10, 9, 6, 15, 165, 4);
    }
    if (risen) return;
    // Cloud: dark bumps first, then the white inside, so only the outer
    // outline remains.
    const int bumps[][4] = {{-34, 26, 36, 24}, {14, 18, 40, 30}, {52, 34, 30, 20}, {-4, 42, 70, 20}};
    for (const auto& b : bumps) FillEllipse(f, cx + b[0], cy + b[1], b[2] + 6, b[3] + 6);
    for (const auto& b : bumps) FillEllipse(f, cx + b[0], cy + b[1], b[2], b[3], false);
}

// Thought cloud: overlapping lobes with one shared outline, three dots inside.
void ThoughtCloud(MonoFrame& f, int cx, int cy, bool dots = true) {
    static const int kLobes[][3] = {{-34, 6, 19}, {-13, -8, 22}, {13, -10, 21}, {34, 5, 18},
                                    {0, 11, 21}};
    for (const auto& l : kLobes) Disk(f, cx + l[0], cy + l[1], l[2] + 4);
    for (const auto& l : kLobes) Disk(f, cx + l[0], cy + l[1], l[2], false);
    if (dots)
        for (int i = -1; i <= 1; ++i) Disk(f, cx + i * 18, cy + 3, 5);
}

// Lightning bolt, h pixels tall around (cx, cy): two slanted triangles (the
// status band's charging mark, scaled). wide is the width in percent of
// that mark; negative mirrors it (top tip on the left). level < 16 dithers.
void Bolt(MonoFrame& f, int cx, int cy, int h, bool ink = true, int level = 16,
          int wide = 100) {
    auto k = [h](int d) { return d * h / 32; };
    auto w = [h, wide](int d) { return d * h * wide / 3200; };
    FillTriangle(f, cx + w(7), cy - k(16), cx - w(7), cy + k(2), cx + w(4), cy + k(2), ink,
                 level);
    FillTriangle(f, cx - w(4), cy - k(3), cx + w(9), cy - k(3), cx - w(5), cy + k(16), ink,
                 level);
}

// V3-05 "eating electricity" (ScenePose::Charging): the left arm holds up
// a chunky outlined bolt like an ice lolly, a bite out of the side facing
// the mouth, zaps around it. The arm goes behind the head.
void ChargingArm(MonoFrame& f, int cx, int cy) {
    Stroke(f, cx - 70, cy + 120, cx - 162, cy + 56, 34);
    Stroke(f, cx - 70, cy + 120, cx - 162, cy + 56, 20, false);
}

void ChargingSnack(MonoFrame& f, int cx, int cy) {
    // In the free space left of the head; its lower tip sits on the paw.
    const int bx = cx - 192, by = cy - 28, wide = -150;
    Bolt(f, bx, by, 118, false, 16, wide);  // white rim over the background
    Bolt(f, bx, by, 100, true, 16, wide);
    Bolt(f, bx, by + 1, 62, false, 16, wide);
    Bolt(f, bx, by + 1, 62, true, 5, wide);
    Disk(f, bx + 6, by - 26, 10, false);  // the bite
    const int px = cx - 168, py = cy + 52;
    Blob(f, px, py, 26, 22, 6);
    for (int dx : {-9, 0, 9}) Stroke(f, px + dx, py - 18, px + dx, py - 9, 4);
    Sparkle(f, bx + 66, by - 62, 9);  // zaps
    Sparkle(f, bx - 62, by + 56, 9);
}

// V3-05 yawn (ScenePose::Yawn): a wide open mouth with a dithered tongue
// over the "w" lips, and a thought bubble with an almost empty battery.
void YawnMouth(MonoFrame& f, int cx, int cy) {
    FillEllipse(f, cx, cy + 58, 20, 26);
    FillEllipse(f, cx, cy + 72, 12, 8, false);
    Shade(f, cx, cy + 72, 12, 8, 6);
}

void BatteryBubble(MonoFrame& f, int cx, int cy) {
    // Where the thinking cloud goes: below the status band (y >= 40).
    Ring(f, cx + 128, cy - 50, 7, 7, 4);
    Ring(f, cx + 148, cy - 72, 10, 10, 4);
    ThoughtCloud(f, cx + 226, cy - 76, /*dots=*/false);
    const int bx = cx + 222, by = cy - 74;
    RoundRectRing(f, bx - 26, by - 12, 48, 26, 5, 4);
    FillRoundRect(f, bx + 22, by - 5, 5, 12, 1);  // terminal nub
    FillRoundRect(f, bx - 20, by - 6, 7, 14, 1);  // one last bar
}

// V4-03 life-tip bubbles (ScenePose::TipHot / TipCold): the thought cloud
// below the status band holds a glass of water or a coat.
void TipBubble(MonoFrame& f, int cx, int cy, bool hot) {
    Ring(f, cx + 128, cy - 50, 7, 7, 4);
    Ring(f, cx + 148, cy - 72, 10, 10, 4);
    ThoughtCloud(f, cx + 226, cy - 76, /*dots=*/false);
    const int bx = cx + 222, by = cy - 74;
    if (hot) {  // glass with water and a handle
        Ring(f, bx + 22, by + 2, 9, 9, 4);
        FillRoundRect(f, bx - 16, by - 20, 36, 40, 5);
        FillRoundRect(f, bx - 11, by - 16, 26, 32, 3, false);
        FillRoundRect(f, bx - 11, by - 3, 26, 19, 2);  // the water
        Sparkle(f, bx + 2, by - 11, 4, false);
    } else {  // coat: outlined sleeves and body, zip, collar
        for (int side : {-1, 1}) {
            Stroke(f, bx + side * 14, by - 8, bx + side * 31, by + 8, 14);
            Stroke(f, bx + side * 14, by - 8, bx + side * 31, by + 8, 7, false);
        }
        FillRoundRect(f, bx - 18, by - 14, 36, 38, 5);
        FillRoundRect(f, bx - 14, by - 10, 28, 30, 3, false);
        Stroke(f, bx, by - 4, bx, by + 20, 3);   // zip
        FillTriangle(f, bx - 11, by - 14, bx + 11, by - 14, bx, by - 1);  // collar
    }
}

// Sweat drops (hot) / shiver lines (cold) beside the head.
void TipFace(MonoFrame& f, int cx, int cy, bool hot) {
    if (hot) {
        for (int side : {-1, 1}) {
            const int dx = cx + side * 128, dy = cy - 46;
            FillTriangle(f, dx, dy - 22, dx - 9, dy, dx + 9, dy);
            Disk(f, dx, dy + 2, 9);
        }
    } else {
        for (int side : {-1, 1})
            for (int i = 0; i < 2; ++i)
                Arc(f, cx + side * (142 + i * 16), cy - 10, 26, 44, side < 0 ? 150 : -30,
                    side < 0 ? 210 : 30, 5);
    }
}

// Sitting pose for the home screen: tail, body, paws, collar bell, head,
// plus the animation pose overlays (all above the action rail).
void SittingCat(MonoFrame& f, int cx, int cy, Emotion emotion, ScenePose pose) {
    // Battery poses bring their own face and no expression ornaments.
    const bool charging = pose == ScenePose::Charging, yawn = pose == ScenePose::Yawn;
    const bool tip_hot = pose == ScenePose::TipHot, tip_cold = pose == ScenePose::TipCold;
    if (tip_hot || tip_cold) emotion = Emotion::Neutral;  // the bubble speaks, no ornaments
    if (charging) emotion = Emotion::Happy;
    if (yawn) emotion = Emotion::Neutral;
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
    const bool wave = pose == ScenePose::Wave || pose == ScenePose::Morning;
    if (wave) {  // arm raised from the body, behind the head
        Stroke(f, cx + 70, cy + 120, cx + 138, cy + 62, 34);
        Stroke(f, cx + 70, cy + 120, cx + 138, cy + 62, 20, false);
    }
    const bool stretch = pose == ScenePose::Sunrise;
    if (stretch) StretchArms(f, cx, cy);
    const bool listening = emotion == Emotion::Listening && !wave && !stretch;
    if (listening) {  // arm up to the perked ear, behind the head
        Stroke(f, cx + 84, cy + 112, cx + 142, cy - 8, 44);
        Stroke(f, cx + 84, cy + 112, cx + 142, cy - 8, 30, false);
    }
    if (charging) ChargingArm(f, cx, cy);
    const bool dizzy = pose == ScenePose::Dizzy1 || pose == ScenePose::Dizzy2;
    Eyes eyes = Eyes::ByEmotion;
    if (pose == ScenePose::Blink || stretch || yawn) eyes = Eyes::Closed;
    if (charging) eyes = Eyes::Laugh;
    if (pose == ScenePose::Wave) eyes = Eyes::WinkLeft;
    if (pose == ScenePose::Giggle) eyes = Eyes::Laugh;
    if (dizzy) eyes = Eyes::Spiral;
    // Stretching TuanTuan yawns: the round "O" mouth.
    CatHead(f, cx, cy, 100, stretch ? Emotion::Surprised : emotion, eyes);
    if (stretch) StretchPaws(f, cx, cy);
    if (charging) ChargingSnack(f, cx, cy);
    if (yawn) {
        YawnMouth(f, cx, cy);
        BatteryBubble(f, cx, cy);
    }
    if (tip_hot || tip_cold) {
        TipFace(f, cx, cy, tip_hot);
        TipBubble(f, cx, cy, tip_hot);
    }
    if (pose == ScenePose::Giggle) {  // wobble lines on both sides of the head
        for (int side : {-1, 1})
            for (int i = 0; i < 2; ++i)
                Arc(f, cx + side * (142 + i * 16), cy - 10, 26, 44, side < 0 ? 150 : -30,
                    side < 0 ? 210 : 30, 5);
    }
    if (dizzy) {  // stars on a dashed orbit over the head; frames alternate
        const int ox = cx, oy = cy - 112, rx = 132, ry = 26;
        for (int deg = 0; deg < 360; deg += 30) Arc(f, ox, oy, rx, ry, deg, deg + 16, 3);
        const int turn = pose == ScenePose::Dizzy1 ? 20 : 80;
        for (int i = 0; i < 3; ++i) {
            const double a = (turn + i * 120) * kPi / 180.0;
            const int sx = ox + static_cast<int>(std::lround(rx * std::cos(a)));
            const int sy = oy + static_cast<int>(std::lround(ry * std::sin(a)));
            Sparkle(f, sx, sy, 20);
        }
    }
    if (wave) {  // paw pads up, with motion lines
        const int px = cx + 142, py = cy + 48;
        Blob(f, px, py, 28, 30, 6);
        for (int dx : {-10, 0, 10}) Stroke(f, px + dx, py - 22, px + dx, py - 12, 4);
        Arc(f, px, py, 48, 48, -70, -20, 5);
        Arc(f, px, py, 62, 62, -66, -24, 5);
    }
    if (listening) {  // paw cupped at the ear, sound waves coming in
        const int px = cx + 146, py = cy - 30;
        Blob(f, px, py, 30, 34, 6);
        for (int dy : {-12, 0, 12}) Stroke(f, px + 14, py + dy, px + 24, py + dy, 4);
        for (int i = 0; i < 3; ++i) Arc(f, px, py, 58 + i * 20, 62 + i * 20, -40, 40, 6);
    }
    if (emotion == Emotion::Thinking) {  // bubbles rising to a thought cloud
        // The cloud stays below the status band's right box (y >= 40).
        Ring(f, cx + 128, cy - 50, 7, 7, 4);
        Ring(f, cx + 148, cy - 72, 10, 10, 4);
        ThoughtCloud(f, cx + 226, cy - 76);
    }
    if (emotion == Emotion::Speaking) {  // sound arcs spreading from the mouth
        for (int r : {166, 190}) {
            Arc(f, cx, cy + 56, r, r, -20, 20, 7);
            Arc(f, cx, cy + 56, r, r, 160, 200, 7);
        }
    }
    if (pose == ScenePose::Hearts1) Heart(f, cx + 150, cy - 64, 16);
    if (pose == ScenePose::Hearts2) {
        Heart(f, cx + 168, cy - 110, 26);
        Heart(f, cx + 136, cy - 40, 12);
        Heart(f, cx - 166, cy - 86, 16);
    }
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

// ---- Status band (V2-05) ---------------------------------------------------------
// No fonts on the device: numbers are chunky seven-segment digits built from
// solid rectangles; icons are strokes, disks and rings. Everything stays in
// the two SceneComposer status boxes, so a status change is a small partial
// refresh that never touches the pet or the rail.

void FillRect(MonoFrame& f, int x, int y, int w, int h, bool ink = true) {
    for (int py = std::max(y, 0); py < std::min(y + h, kH); ++py) Span(f, x, x + w - 1, py, ink);
}

constexpr uint8_t kSegA = 0x40, kSegB = 0x20, kSegC = 0x10, kSegD = 0x08, kSegE = 0x04,
                  kSegF = 0x02, kSegG = 0x01;  // a top, clockwise, g middle
constexpr uint8_t kDigitSegments[10] = {0x7E, 0x30, 0x6D, 0x79, 0x33,
                                        0x5B, 0x5F, 0x70, 0x7F, 0x7B};
constexpr int kDigitW = 18, kDigitH = 30, kSegT = 5, kStatusY = 5;

void SevenSegment(MonoFrame& f, int x, int y, uint8_t segments, int w = kDigitW,
                  bool ink = true) {
    const int h = kDigitH, t = kSegT, half = h / 2;
    if (segments & kSegA) FillRect(f, x, y, w, t, ink);
    if (segments & kSegB) FillRect(f, x + w - t, y, t, half + 1, ink);
    if (segments & kSegC) FillRect(f, x + w - t, y + half, t, h - half, ink);
    if (segments & kSegD) FillRect(f, x, y + h - t, w, t, ink);
    if (segments & kSegE) FillRect(f, x, y + half, t, h - half, ink);
    if (segments & kSegF) FillRect(f, x, y, t, half + 1, ink);
    if (segments & kSegG) FillRect(f, x, y + half - t / 2, w, t, ink);
}

void Digit(MonoFrame& f, int x, int y, int value, bool ink = true) {
    SevenSegment(f, x, y, kDigitSegments[Clamp(value, 0, 9)], kDigitW, ink);
}

// Left box: HH:MM, quiet-hours moon, Wi-Fi.
void StatusLeft(MonoFrame& f, const StatusBar& s) {
    if (s.hour < 24 && s.minute < 60) {
        Digit(f, 14, kStatusY, s.hour / 10);
        Digit(f, 37, kStatusY, s.hour % 10);
        FillRect(f, 60, kStatusY + 7, kSegT, kSegT);
        FillRect(f, 60, kStatusY + 18, kSegT, kSegT);
        Digit(f, 71, kStatusY, s.minute / 10);
        Digit(f, 94, kStatusY, s.minute % 10);
    }
    if (s.quiet) {  // crescent moon
        Disk(f, 136, 20, 13);
        Disk(f, 143, 14, 11, false);
    }
    if (s.network != NetworkState::Unknown) {
        const int cx = 190, cy = 32;
        Disk(f, cx, cy - 2, 3);
        for (int r : {9, 16, 23}) Arc(f, cx, cy, r, r, 225, 315, 4);
        if (s.network == NetworkState::Offline) {  // struck through
            Stroke(f, 172, 8, 208, 34, 9, false);
            Stroke(f, 172, 8, 208, 34, 4);
        }
    }
    if (s.parent_request) {  // D5: an envelope, a request waits for the parent
        RoundRectRing(f, 216, 11, 28, 20, 3, 3);
        Stroke(f, 218, 13, 230, 23, 3);
        Stroke(f, 242, 13, 230, 23, 3);
    } else if (s.link_repair) {  // the home server link needs pairing again
        Ring(f, 230, 21, 13, 13, 3);
        Stroke(f, 224, 15, 236, 27, 3);
        Stroke(f, 236, 15, 224, 27, 3);
    }
}

// Right box: temperature in degC, charging bolt, battery with four bars.
void StatusRight(MonoFrame& f, const StatusBar& s) {
    const int temp = s.temp_c;
    if (s.temp_c != StatusBar::kNoTemp && temp >= -9 && temp <= 99) {
        if (temp < 0) SevenSegment(f, 638, kStatusY, kSegG);
        else if (temp >= 10) Digit(f, 638, kStatusY, temp / 10);
        Digit(f, 661, kStatusY, std::abs(temp) % 10);
        Ring(f, 690, kStatusY + 5, 5, 5, 2);                               // degree
        SevenSegment(f, 699, kStatusY, kSegA | kSegD | kSegE | kSegF, 14);  // C
    }
    if (s.battery_bars > StatusBar::kBatteryBars) return;
    RoundRectRing(f, 744, 7, 42, 26, 4, 3);
    FillRect(f, 786, 14, 4, 12);  // terminal nub
    for (int i = 0; i < s.battery_bars; ++i) FillRect(f, 749 + i * 9, 12, 7, 16);
    if (s.charging) {
        FillTriangle(f, 734, 4, 720, 22, 731, 22);
        FillTriangle(f, 723, 17, 736, 17, 722, 36);
    }
}

void StatusBand(MonoFrame& f, const StatusBar& status) {
    StatusLeft(f, status);
    StatusRight(f, status);
}

// ---- Parent menu (V3-06) ---------------------------------------------------------
// One row per ParentItem under a header (gear = settings, house = a long
// press goes home). Still no text: an icon names each row, times are
// seven-segment HH:MM, volumes ten rising bars, switches a pill with a knob.
// The highlighted row has a thick frame; the row being edited turns black
// with white content and up/down arrows (the dial changes the value).

constexpr int kParentRowTop = 72, kParentRowH = 45, kParentValueX = 150;

// HH:MM from minutes after midnight, digits 30 px tall around row centre cy.
void ClockDigits(MonoFrame& f, int x, int cy, uint16_t minute, bool ink) {
    const int y = cy - kDigitH / 2, hour = (minute / 60) % 24, mins = minute % 60;
    Digit(f, x, y, hour / 10, ink);
    Digit(f, x + 23, y, hour % 10, ink);
    FillRect(f, x + 46, y + 7, kSegT, kSegT, ink);
    FillRect(f, x + 46, y + 18, kSegT, kSegT, ink);
    Digit(f, x + 57, y, mins / 10, ink);
    Digit(f, x + 80, y, mins % 10, ink);
}

// Ten bars rising left to right; filled up to pct (steps of 10 %).
void VolumeBars(MonoFrame& f, int x, int cy, uint8_t pct, bool ink) {
    const int filled = Clamp(pct / 10, 0, 10);
    for (int i = 0; i < 10; ++i) {
        const int h = 8 + i * 3, bx = x + i * 30, by = cy + 16 - h;
        if (i < filled) {
            FillRect(f, bx, by, 20, h, ink);
        } else {  // empty bar: outline only
            FillRect(f, bx, by, 20, 3, ink);
            FillRect(f, bx, by + h - 3, 20, 3, ink);
            FillRect(f, bx, by, 3, h, ink);
            FillRect(f, bx + 17, by, 3, h, ink);
        }
    }
}

void Switch(MonoFrame& f, int x, int cy, bool on, bool ink) {
    if (on) {
        FillRoundRect(f, x, cy - 18, 96, 36, 18, ink);
        Disk(f, x + 78, cy, 12, !ink);
    } else {
        RoundRectRing(f, x, cy - 18, 96, 36, 18, 4, ink);
        Disk(f, x + 18, cy, 10, ink);
    }
}

void Speaker(MonoFrame& f, int cx, int cy, bool ink) {
    FillRect(f, cx - 20, cy - 7, 10, 14, ink);
    FillTriangle(f, cx - 10, cy - 7, cx + 2, cy - 16, cx + 2, cy + 16, ink);
    FillTriangle(f, cx - 10, cy - 7, cx - 10, cy + 7, cx + 2, cy + 16, ink);
}

// Sun ray at `deg` degrees (clockwise from +x), from radius r0 to r1.
void Ray(MonoFrame& f, int cx, int cy, int deg, int r0, int r1, bool ink) {
    const double a = deg * kPi / 180.0, c = std::cos(a), s = std::sin(a);
    Stroke(f, cx + static_cast<int>(std::lround(r0 * c)), cy + static_cast<int>(std::lround(r0 * s)),
           cx + static_cast<int>(std::lround(r1 * c)), cy + static_cast<int>(std::lround(r1 * s)), 4,
           ink);
}

void Moon(MonoFrame& f, int cx, int cy, int r, bool ink) {
    Disk(f, cx, cy, r, ink);
    Disk(f, cx + r / 2, cy - r / 2, r * 6 / 7, !ink);
}

// Cloud (the home server); scale 1 fits a menu row (about 36 x 26 px).
void Cloud(MonoFrame& f, int cx, int cy, int scale, bool ink) {
    Disk(f, cx - 9 * scale, cy + 3 * scale, 8 * scale, ink);
    Disk(f, cx + scale, cy - 3 * scale, 11 * scale, ink);
    Disk(f, cx + 11 * scale, cy + 3 * scale, 8 * scale, ink);
    FillRect(f, cx - 9 * scale, cy + 3 * scale, 20 * scale, 8 * scale + 1, ink);
}

void CheckMark(MonoFrame& f, int cx, int cy, int size, int w, bool ink) {
    Stroke(f, cx - size, cy, cx - size / 3, cy + size * 2 / 3, w, ink);
    Stroke(f, cx - size / 3, cy + size * 2 / 3, cx + size, cy - size * 2 / 3, w, ink);
}

void Cross(MonoFrame& f, int cx, int cy, int size, int w, bool ink) {
    Stroke(f, cx - size, cy - size, cx + size, cy + size, w, ink);
    Stroke(f, cx - size, cy + size, cx + size, cy - size, w, ink);
}

// Seven-segment digit of any size (the pairing code).
void SizedDigit(MonoFrame& f, int x, int y, int value, int w, int h, int t, bool ink = true) {
    const uint8_t s = kDigitSegments[Clamp(value, 0, 9)];
    const int half = h / 2;
    if (s & kSegA) FillRect(f, x, y, w, t, ink);
    if (s & kSegB) FillRect(f, x + w - t, y, t, half + 1, ink);
    if (s & kSegC) FillRect(f, x + w - t, y + half, t, h - half, ink);
    if (s & kSegD) FillRect(f, x, y + h - t, w, t, ink);
    if (s & kSegE) FillRect(f, x, y + half, t, h - half, ink);
    if (s & kSegF) FillRect(f, x, y, t, half + 1, ink);
    if (s & kSegG) FillRect(f, x, y + half - t / 2, w, t, ink);
}

// Six digits (leading zeros) left to right from x; digit w x h, gap apart.
void CodeDigits(MonoFrame& f, int x, int y, uint32_t code, int w, int h, int t, int gap, bool ink) {
    uint32_t divisor = 100000;
    for (int i = 0; i < 6; ++i, divisor /= 10)
        SizedDigit(f, x + i * (w + gap), y, static_cast<int>(code / divisor % 10), w, h, t, ink);
}

// Server link state around (cx, cy): check = connected, dots = working,
// dashes = not connected yet, cross = failed, ringed cross = pair again.
void LinkGlyph(MonoFrame& f, LinkState state, int cx, int cy, int scale, bool ink) {
    switch (state) {
        case LinkState::Paired: CheckMark(f, cx, cy, 16 * scale, 5 * scale, ink); break;
        case LinkState::Connecting:
        case LinkState::Saving:
        case LinkState::ShowCode:
            for (int i = -1; i <= 1; ++i) Disk(f, cx + i * 24 * scale, cy, 5 * scale, ink);
            break;
        case LinkState::Unpaired:
            for (int i = -1; i <= 1; ++i)
                FillRect(f, cx + i * 24 * scale - 8 * scale, cy - 2 * scale, 16 * scale, 5 * scale, ink);
            break;
        case LinkState::NoServer:
        case LinkState::Failed: Cross(f, cx, cy, 14 * scale, 5 * scale, ink); break;
        case LinkState::NeedsRepair:
            Ring(f, cx, cy, 20 * scale, 20 * scale, 4 * scale, ink);
            Cross(f, cx, cy, 10 * scale, 4 * scale, ink);
            break;
    }
}

void ParentIcon(MonoFrame& f, ParentItem item, int cx, int cy, bool ink) {
    switch (item) {
        case ParentItem::Wake:  // sun
            Disk(f, cx, cy, 11, ink);
            for (int a = 0; a < 360; a += 45) Ray(f, cx, cy, a, 14, 19, ink);
            break;
        case ParentItem::Evening:  // sun going down behind the horizon
            Disk(f, cx, cy + 8, 13, ink);
            FillRect(f, cx - 22, cy + 9, 44, 12, !ink);
            Stroke(f, cx - 24, cy + 12, cx + 24, cy + 12, 4, ink);
            for (int a : {210, 250, 290, 330}) Ray(f, cx, cy + 8, a, 17, 23, ink);
            break;
        case ParentItem::QuietStart:  // moon, arrow down: quiet hours begin
        case ParentItem::QuietEnd: {  // moon, arrow up: quiet hours end
            Moon(f, cx - 8, cy, 15, ink);
            const bool down = item == ParentItem::QuietStart;
            const int tip = down ? cy + 18 : cy - 18, tail = down ? cy - 14 : cy + 14;
            Stroke(f, cx + 20, tail, cx + 20, tip - (down ? 6 : -6), 4, ink);
            FillTriangle(f, cx + 12, tip + (down ? -8 : 8), cx + 28, tip + (down ? -8 : 8), cx + 20,
                         tip, ink);
            break;
        }
        case ParentItem::Nap:  // Zz
            LetterZ(f, cx - 20, cy - 2, 16, 4, ink);
            LetterZ(f, cx + 4, cy - 18, 11, 3, ink);
            break;
        case ParentItem::Volume:
            Speaker(f, cx - 4, cy, ink);
            Arc(f, cx, cy, 10, 10, -50, 50, 3, ink);
            Arc(f, cx, cy, 18, 18, -50, 50, 3, ink);
            break;
        case ParentItem::QuietVolume:
            Speaker(f, cx - 4, cy, ink);
            Moon(f, cx + 16, cy - 8, 9, ink);
            break;
        case ParentItem::Motion:  // the board, wobbling
            RoundRectRing(f, cx - 11, cy - 17, 22, 34, 5, 4, ink);
            Arc(f, cx, cy, 24, 18, 150, 210, 3, ink);
            Arc(f, cx, cy, 24, 18, -30, 30, 3, ink);
            break;
        case ParentItem::Server:  // the home server
            Cloud(f, cx, cy, 1, ink);
            break;
    }
}

void ParentRow(MonoFrame& f, const ParentView& view, uint8_t row) {
    const auto item = static_cast<ParentItem>(row);
    const int top = kParentRowTop + row * kParentRowH, cy = top + kParentRowH / 2;
    const bool selected = row == view.cursor, editing = selected && view.editing;
    if (editing) FillRoundRect(f, 12, top + 2, 776, kParentRowH - 4, 14);
    else if (selected) RoundRectRing(f, 12, top + 2, 776, kParentRowH - 4, 14, 5);
    else if (row + 1 < kParentItemCount) FillRect(f, 40, top + kParentRowH - 1, 720, 1);
    const bool ink = !editing;
    ParentIcon(f, item, 72, cy, ink);
    const auto& s = view.settings;
    switch (item) {
        case ParentItem::Wake: ClockDigits(f, kParentValueX, cy, s.schedule.wake_min, ink); break;
        case ParentItem::Evening: ClockDigits(f, kParentValueX, cy, s.schedule.evening_min, ink); break;
        case ParentItem::QuietStart:
            ClockDigits(f, kParentValueX, cy, s.schedule.quiet_start_min, ink);
            break;
        case ParentItem::QuietEnd:
            ClockDigits(f, kParentValueX, cy, s.schedule.quiet_end_min, ink);
            break;
        case ParentItem::Nap: Switch(f, kParentValueX, cy, s.schedule.nap_enabled, ink); break;
        case ParentItem::Volume: VolumeBars(f, kParentValueX, cy, s.volume_cap_pct, ink); break;
        case ParentItem::QuietVolume: VolumeBars(f, kParentValueX, cy, s.quiet_volume_pct, ink); break;
        case ParentItem::Motion: Switch(f, kParentValueX, cy, s.motion_enabled, ink); break;
        case ParentItem::Server:
            if (view.link.state == LinkState::ShowCode)
                CodeDigits(f, kParentValueX, cy - kDigitH / 2, view.link.code, kDigitW, kDigitH, kSegT,
                           5, ink);
            else
                LinkGlyph(f, view.link.state, kParentValueX + 40, cy, 1, ink);
            break;
    }
    if (editing) {  // the dial changes this value
        FillTriangle(f, 740, cy - 4, 760, cy - 4, 750, cy - 18, false);
        FillTriangle(f, 740, cy + 4, 760, cy + 4, 750, cy + 18, false);
    }
}

// Header right: house = a long dial press leaves (home, or back to the menu).
void ParentHeader(MonoFrame& f) {
    FillTriangle(f, 726, 32, 752, 8, 778, 32);
    FillRect(f, 734, 32, 36, 24);
    FillRect(f, 747, 40, 10, 16, false);
    FillRect(f, 12, 62, 776, 3);
}

void Envelope(MonoFrame& f, int cx, int cy, int scale, bool ink = true) {
    RoundRectRing(f, cx - 14 * scale, cy - 10 * scale, 28 * scale, 20 * scale, 3 * scale, 3 * scale, ink);
    Stroke(f, cx - 12 * scale, cy - 8 * scale, cx, cy + 2 * scale, 3 * scale, ink);
    Stroke(f, cx + 12 * scale, cy - 8 * scale, cx, cy + 2 * scale, 3 * scale, ink);
}

// Temperament the request asks for, as a face: curious = round eyes and a
// question mark, gentle = smiling closed eyes, lively = big laugh, sparkles.
void TemperamentFace(MonoFrame& f, int cx, int cy, uint8_t temperament) {
    Ring(f, cx, cy, 70, 70, 7);
    switch (temperament) {
        case 0:  // Curious
            Disk(f, cx - 26, cy - 14, 11);
            Disk(f, cx + 26, cy - 14, 11);
            Ring(f, cx, cy + 30, 10, 10, 4);
            QuestionMark(f, cx + 110, cy - 10, 20, 7);
            break;
        case 1:  // Gentle
            Arc(f, cx - 26, cy - 6, 13, 10, 200, 340, 5);
            Arc(f, cx + 26, cy - 6, 13, 10, 200, 340, 5);
            Arc(f, cx, cy + 14, 24, 16, 20, 160, 5);
            break;
        default:  // Lively
            Disk(f, cx - 26, cy - 16, 9);
            Disk(f, cx + 26, cy - 16, 9);
            FillEllipse(f, cx, cy + 22, 28, 20);
            FillRect(f, cx - 30, cy, 60, 22, false);
            Sparkle(f, cx - 110, cy - 30, 22);
            Sparkle(f, cx + 110, cy - 30, 22);
            break;
    }
}

// One answer card: the highlighted one is black with a white glyph.
void ChoiceCard(MonoFrame& f, int x, bool selected, bool check) {
    const int y = 330, w = 220, h = 130, cx = x + w / 2, cy = y + h / 2;
    if (selected) FillRoundRect(f, x, y, w, h, 20);
    else RoundRectRing(f, x, y, w, h, 20, 6);
    if (check) CheckMark(f, cx, cy, 40, 14, !selected);
    else Cross(f, cx, cy, 34, 14, !selected);
}

// D5 request page: what the parent panel asks (wipe memory: a crossed-out
// heart; temperament: the new face), then check / cross. Dots: more to come.
void RequestPage(MonoFrame& f, const RequestView& r) {
    Envelope(f, 44, 32, 2);
    ParentHeader(f);
    for (int i = 0; i < std::min<int>(r.more, 7); ++i) Disk(f, 340 + i * 20, 32, 6);
    if (r.kind == RequestKind::MemoryClear) {
        Heart(f, 400, 160, 70);
        Stroke(f, 300, 90, 500, 290, 26, false);
        Stroke(f, 300, 90, 500, 290, 12);
    } else {
        TemperamentFace(f, 400, 190, r.temperament);
    }
    ChoiceCard(f, 150, r.confirm, true);
    ChoiceCard(f, 430, !r.confirm, false);
}

// "Connect to the home server" page: a big cloud, then the six-digit code
// to type into the parent panel, or the link state.
void LinkPage(MonoFrame& f, const LinkView& link) {
    Cloud(f, 44, 32, 1, true);
    ParentHeader(f);
    Cloud(f, 400, 150, 4, true);
    if (link.state == LinkState::ShowCode) {
        constexpr int w = 64, h = 110, t = 14, gap = 22;
        CodeDigits(f, 400 - (6 * w + 5 * gap) / 2, 250, link.code, w, h, t, gap, true);
    } else {
        LinkGlyph(f, link.state, 400, 330, 3, true);
    }
}

void ParentScene(MonoFrame& f, const ParentView& view) {
    if (view.page == ParentPage::Request) {
        RequestPage(f, view.request);
        return;
    }
    if (view.page == ParentPage::Link) {
        LinkPage(f, view.link);
        return;
    }
    // Gear: settings.
    Disk(f, 44, 32, 17);
    for (int a = 0; a < 360; a += 45) Ray(f, 44, 32, a, 16, 22, true);
    Disk(f, 44, 32, 7, false);
    ParentHeader(f);
    for (uint8_t row = 0; row < kParentItemCount; ++row) ParentRow(f, view, row);
}

// ---- Mini games (V4-06) ----------------------------------------------------------
// TuanTuan and the question card above SceneComposer::kGameRailTop, two or
// three answer cards below it. The dial highlight is a thick frame around
// one card, so moving it only touches the answer rail; TuanTuan's reaction
// only touches the question area. Pictures come from the GameArt catalogue
// (no text, no bitmaps from the card): (cx, cy) is the centre, r the
// half-size of the square the picture fits in.

constexpr int kGameCardW = 200, kGameCardH = 140, kGameCardY = 322, kGameFrame = 16;

// Dice-style positions in units of r / 100.
void GameDots(MonoFrame& f, int n, int cx, int cy, int r) {
    static const int kPos[5][5][2] = {
        {{0, 0}},
        {{-45, -45}, {45, 45}},
        {{-50, -50}, {0, 0}, {50, 50}},
        {{-45, -45}, {45, -45}, {-45, 45}, {45, 45}},
        {{-50, -50}, {50, -50}, {0, 0}, {-50, 50}, {50, 50}}};
    const int dot = std::max(4, r * 23 / 100);
    for (int i = 0; i < n; ++i)
        Disk(f, cx + kPos[n - 1][i][0] * r / 100, cy + kPos[n - 1][i][1] * r / 100, dot);
}

// Building blocks: up to three on the ground, the rest stacked on top.
void GameBlocks(MonoFrame& f, int n, int cx, int cy, int r) {
    const int b = r * 50 / 100, gap = std::max(3, r / 12);
    const int bottom = std::min(n, 3), top = n - bottom;
    const int rows = top > 0 ? 2 : 1;
    const int y0 = cy - (rows * b + (rows - 1) * gap) / 2;
    for (int row = 0; row < rows; ++row) {
        const int count = row == rows - 1 ? bottom : top;
        const int width = count * b + (count - 1) * gap;
        const int y = y0 + row * (b + gap);
        for (int i = 0; i < count; ++i) FillRect(f, cx - width / 2 + i * (b + gap), y, b, b);
    }
}

void GameBall(MonoFrame& f, int cx, int cy, int radius) {
    const int w = std::max(3, radius / 8);
    Blob(f, cx, cy, radius, radius, w);
    Arc(f, cx, cy, radius * 6 / 10, radius * 6 / 10, 200, 340, w);
    Arc(f, cx, cy, radius * 6 / 10, radius * 6 / 10, 20, 160, w);
}

// V4-08 microphone games (MicArt): candles, the clapping hands, the blowing face.
void MicArtIcon(MonoFrame& f, MicArt art, int cx, int cy, int r) {
    const int w = std::max(3, r / 10);
    auto p = [r](int percent) { return r * percent / 100; };
    switch (art) {
        case MicArt::CandleLit:
        case MicArt::CandleOut:
            FillRect(f, cx - p(24), cy - p(6), 2 * p(24), p(66));
            Stroke(f, cx, cy - p(6), cx, cy - p(24), std::max(2, w / 2));
            if (art == MicArt::CandleLit) {
                FillEllipse(f, cx, cy - p(46), p(15), p(20));
                FillTriangle(f, cx, cy - p(88), cx - p(14), cy - p(48), cx + p(14), cy - p(48));
            } else {  // a thin curl of smoke
                Arc(f, cx - p(9), cy - p(44), p(13), p(13), -90, 90, w);
                Arc(f, cx + p(9), cy - p(70), p(13), p(13), 90, 270, w);
            }
            break;
        case MicArt::Hand:
        case MicArt::HandHit:
            for (int side : {-1, 1}) {  // two palms about to meet
                if (art == MicArt::HandHit) FillEllipse(f, cx + side * p(30), cy, p(26), p(50));
                else Blob(f, cx + side * p(34), cy, p(26), p(50), w);
            }
            if (art == MicArt::HandHit) {
                Sparkle(f, cx, cy - p(66), p(18));
                Sparkle(f, cx - p(70), cy - p(54), p(10));
                Sparkle(f, cx + p(70), cy - p(54), p(10));
            }
            break;
        case MicArt::Blow:
            Blob(f, cx - p(30), cy, p(52), p(52), w);
            Disk(f, cx - p(18), cy - p(14), p(7));
            Disk(f, cx - p(52), cy - p(14), p(7));
            Disk(f, cx - p(10), cy + p(16), p(9));  // the round mouth
            for (int k = -1; k <= 1; ++k)           // the air
                Stroke(f, cx + p(36), cy + p(16) + k * p(26), cx + p(92), cy + p(16) + k * p(34), w);
            break;
    }
}

#include "play_art_draw.inc"  // V8: PlayArt pictures and the non-card boards

void CareArtIcon(MonoFrame& f, uint16_t art, int cx, int cy, int r);  // V8, below

void GameArtIcon(MonoFrame& f, uint16_t art, int cx, int cy, int r) {
    if (CareArtValid(art)) {  // V8 care menu cards share the answer-card rail
        CareArtIcon(f, art, cx, cy, r);
        return;
    }
    if (PlayArtValid(art)) {
        PlayArtIcon(f, art, cx, cy, r);
        return;
    }
    if (MicArtValid(art)) {
        MicArtIcon(f, static_cast<MicArt>(art), cx, cy, r);
        return;
    }
    const int w = std::max(3, r / 10);
    auto p = [r](int percent) { return r * percent / 100; };
    switch (static_cast<GameArt>(art)) {
        case GameArt::Circle:
            Disk(f, cx, cy, p(85));
            break;
        case GameArt::Square:
            FillRect(f, cx - p(75), cy - p(75), 2 * p(75), 2 * p(75));
            break;
        case GameArt::Triangle:
            FillTriangle(f, cx, cy - p(85), cx - p(92), cy + p(72), cx + p(92), cy + p(72));
            break;
        case GameArt::Star: {
            const int outer = p(95), inner = p(40), sy = cy + p(8);
            int ox[5], oy[5], ix[5], iy[5];
            for (int k = 0; k < 5; ++k) {
                const double a = (-90 + 72 * k) * kPi / 180.0, b = a + 36 * kPi / 180.0;
                ox[k] = cx + static_cast<int>(std::lround(outer * std::cos(a)));
                oy[k] = sy + static_cast<int>(std::lround(outer * std::sin(a)));
                ix[k] = cx + static_cast<int>(std::lround(inner * std::cos(b)));
                iy[k] = sy + static_cast<int>(std::lround(inner * std::sin(b)));
            }
            for (int k = 0; k < 5; ++k) {
                FillTriangle(f, cx, sy, ox[k], oy[k], ix[k], iy[k]);
                FillTriangle(f, cx, sy, ox[k], oy[k], ix[(k + 4) % 5], iy[(k + 4) % 5]);
            }
            break;
        }
        case GameArt::Heart:
            Heart(f, cx, cy - p(32), p(76));
            break;
        case GameArt::Dots1: case GameArt::Dots2: case GameArt::Dots3:
        case GameArt::Dots4: case GameArt::Dots5:
            GameDots(f, art - static_cast<int>(GameArt::Dots1) + 1, cx, cy, r);
            break;
        case GameArt::Blocks1: case GameArt::Blocks2: case GameArt::Blocks3:
        case GameArt::Blocks4: case GameArt::Blocks5:
            GameBlocks(f, art - static_cast<int>(GameArt::Blocks1) + 1, cx, cy, r);
            break;
        case GameArt::BallBig:
            GameBall(f, cx, cy, p(90));
            break;
        case GameArt::BallSmall:
            GameBall(f, cx, cy + p(45), p(40));
            break;
        case GameArt::Cat:
            CatHead(f, cx, cy + p(18), r * 100 / 165, Emotion::Happy);
            break;
        case GameArt::Dog:
            for (int side : {-1, 1})  // floppy ears behind the head
                FillEllipse(f, cx + side * p(62), cy + p(10), p(22), p(46));
            Blob(f, cx, cy + p(8), p(60), p(58), w);
            for (int side : {-1, 1}) Disk(f, cx + side * p(22), cy - p(6), p(9));
            FillEllipse(f, cx, cy + p(24), p(15), p(10));
            for (int side : {-1, 1})
                Arc(f, cx + side * p(11), cy + p(36), p(11), p(9), 0, 180, std::max(2, w * 2 / 3));
            break;
        case GameArt::Bird:
            for (int side : {-1, 1})
                Stroke(f, cx - p(10) + side * p(14), cy + p(52), cx - p(10) + side * p(18),
                       cy + p(88), w);
            Blob(f, cx - p(10), cy + p(8), p(62), p(50), w);
            FillTriangle(f, cx + p(44), cy - p(14), cx + p(92), cy - p(2), cx + p(44), cy + p(10));
            Disk(f, cx + p(20), cy - p(14), p(9));
            Arc(f, cx - p(22), cy + p(10), p(30), p(20), 0, 180, w);
            break;
        case GameArt::Fish:
            FillTriangle(f, cx + p(40), cy, cx + p(95), cy - p(44), cx + p(95), cy + p(44));
            Blob(f, cx - p(12), cy, p(62), p(42), w);
            Disk(f, cx - p(42), cy - p(8), p(9));
            Arc(f, cx - p(14), cy, p(14), p(26), -70, 70, w);
            break;
        case GameArt::Listen:
            FillRect(f, cx - p(70), cy - p(22), p(28), p(44));
            FillTriangle(f, cx - p(42), cy - p(22), cx - p(2), cy - p(62), cx - p(2), cy + p(62));
            FillTriangle(f, cx - p(42), cy - p(22), cx - p(42), cy + p(22), cx - p(2), cy + p(62));
            for (int k = 1; k <= 3; ++k) Arc(f, cx + p(4), cy, p(22) * k, p(22) * k, -45, 45, w);
            break;
        case GameArt::None:
            break;
    }
}

// Left edge of answer card i of n, centred on the panel.
int GameCardX(uint8_t i, uint8_t n) {
    const int pitch = n >= 3 ? 260 : 300;
    const int centre = 400 + (2 * static_cast<int>(i) - (n - 1)) * pitch / 2;
    return centre - kGameCardW / 2;
}

// V4-07 tilt ball: seven cells on a line; the hole at its end (a ring while
// the ball sits in it), the ball (the GameBall picture) on its cell.
constexpr int kBallTrackY = 392, kBallCellPitch = 100, kBallTrackX = 100;

void BallTrack(MonoFrame& f, const GameView& g) {
    Stroke(f, kBallTrackX - 50, kBallTrackY + 52, kBallTrackX + 6 * kBallCellPitch + 50,
           kBallTrackY + 52, 5);
    for (int i = 0; i < 7; ++i) {
        const int x = kBallTrackX + i * kBallCellPitch;
        if (i == g.hole) {
            if (i == g.ball) Ring(f, x, kBallTrackY, 46, 46, 6);
            else Disk(f, x, kBallTrackY, 44);
        } else if (i != g.ball) {
            Disk(f, x, kBallTrackY + 40, 5);
        }
    }
    GameBall(f, kBallTrackX + g.ball * kBallCellPitch, kBallTrackY - (g.ball == g.hole ? 0 : 6),
             g.ball == g.hole ? 30 : 38);
}

// V4-07 peekaboo: the board as a phone-sized card, hollow face up, solid
// while TuanTuan hides (face down).
void PeekabooCard(MonoFrame& f, const GameView& g) {
    if (g.hidden) {
        FillRoundRect(f, 300, 336, 200, 112, 22);
    } else {
        RoundRectRing(f, 300, 336, 200, 112, 22, 6);
        Disk(f, 400, 392, 14);
    }
}

void GameScene(MonoFrame& f, const GameView& g) {
    // TuanTuan watches; a correct answer (and only that) gets hearts.
    const Emotion emotion = g.hidden                               ? Emotion::Sleepy
                            : g.feedback == GameFeedback::Right    ? Emotion::Happy
                            : g.feedback == GameFeedback::TryAgain ? Emotion::Curious
                                                                   : Emotion::Neutral;
    const Eyes eyes = g.hidden                             ? Eyes::Closed
                      : g.feedback == GameFeedback::Right  ? Eyes::Laugh
                      : g.feedback == GameFeedback::Reveal ? Eyes::WinkLeft
                                                           : Eyes::ByEmotion;
    CatHead(f, 150, 172, 62, emotion, eyes);
    if (g.feedback == GameFeedback::Right) {
        Heart(f, 52, 64, 22);
        Heart(f, 254, 54, 16);
        Sparkle(f, 276, 150, 14);
    } else if (g.feedback == GameFeedback::TryAgain) {
        QuestionMark(f, 274, 84, 22, 8);
    } else if (g.feedback == GameFeedback::Reveal) {
        Sparkle(f, 262, 74, 16);
        Sparkle(f, 44, 104, 10);
    }
    // Question card (clear of the status corner boxes, like every scene).
    RoundRectRing(f, 336, 26, 292, 252, 28, 6);
    GameArtIcon(f, g.question_art ? g.question_art : static_cast<uint16_t>(GameArt::Listen),
                482, 152, 96);
    // Progress: done rounds filled, the current one ringed with a dot.
    for (int i = 0; i < g.rounds && i < 5; ++i) {
        const int x = 720, y = 64 + i * 44;
        if (i + 1 < g.round) {
            Disk(f, x, y, 12);
        } else if (i + 1 == g.round) {
            Ring(f, x, y, 14, 14, 4);
            Disk(f, x, y, 5);
        } else {
            Ring(f, x, y, 9, 9, 3);
        }
    }
    // V8 games: their own boards, or the answer cards once the shake game asks.
    if (g.play != 0 && g.play != 5) {
        PlayBoard(f, g);
        return;
    }
    if (g.play == 5 && g.option_count == 0) return;
    // Motion games draw their own rail (no answer cards).
    if (g.motion == 1) {
        BallTrack(f, g);
        return;
    }
    if (g.motion == 2) {
        PeekabooCard(f, g);
        return;
    }
    // Answer rail.
    const uint8_t n = std::min<uint8_t>(g.option_count, 3);
    for (uint8_t i = 0; i < n; ++i) {
        const int x = GameCardX(i, n);
        RoundRectRing(f, x, kGameCardY, kGameCardW, kGameCardH, 24, 5);
        GameArtIcon(f, g.options[i], x + kGameCardW / 2, kGameCardY + kGameCardH / 2, 58);
        if (i == g.highlight)
            RoundRectRing(f, x - kGameFrame, kGameCardY - kGameFrame, kGameCardW + 2 * kGameFrame,
                          kGameCardH + 2 * kGameFrame, 24 + kGameFrame, 10);
    }
}

// ---- V8 care and growth ----------------------------------------------------------
// Clothes, the little room, festival props, the "I need something" bubble
// and the care / diary / sticker-album scenes. Everything is drawn from
// the primitives above (no bitmaps). Accessories take the same (cx, cy,
// scale) as CatHead, so the home pet (scale 100) and the care pet (70)
// wear the same things. Item numbers follow the wardrobe table in
// care_store.cc (WardrobeItem::item per WearSlot).

constexpr uint8_t kSlotHead = 0, kSlotNeck = 1, kSlotFace = 2, kSlotBed = 3, kSlotLamp = 4;

void FilledStar(MonoFrame& f, int cx, int cy, int outer, bool ink = true) {
    const int inner = outer * 42 / 100;
    int ox[5], oy[5], ix[5], iy[5];
    for (int k = 0; k < 5; ++k) {
        const double a = (-90 + 72 * k) * kPi / 180.0, b = a + 36 * kPi / 180.0;
        ox[k] = cx + static_cast<int>(std::lround(outer * std::cos(a)));
        oy[k] = cy + static_cast<int>(std::lround(outer * std::sin(a)));
        ix[k] = cx + static_cast<int>(std::lround(inner * std::cos(b)));
        iy[k] = cy + static_cast<int>(std::lround(inner * std::sin(b)));
    }
    for (int k = 0; k < 5; ++k) {
        FillTriangle(f, cx, cy, ox[k], oy[k], ix[k], iy[k], ink);
        FillTriangle(f, cx, cy, ox[k], oy[k], ix[(k + 4) % 5], iy[(k + 4) % 5], ink);
    }
}

// Head items sit between the ears, above the forehead stripes.
void HeadItem(MonoFrame& f, uint8_t item, int cx, int cy, int scale) {
    auto s = [scale](int d) { return d * scale / 100; };
    const int w = std::max(2, s(6));
    switch (item) {
        case 1:  // cap: a dome with a brim to the right and a button
            for (int dy = -s(38); dy <= 0; ++dy) {
                const int hw = HalfWidth(dy, s(64), s(38));
                if (hw >= 0) Span(f, cx - hw, cx + hw, cy - s(86) + dy, true);
            }
            Stroke(f, cx - s(10), cy - s(86), cx + s(98), cy - s(78), std::max(3, s(12)));
            Disk(f, cx, cy - s(126), std::max(2, s(8)));
            Arc(f, cx, cy - s(86), s(40), s(28), 200, 340, w, false);
            break;
        case 2:  // bow on the right ear
            FillTriangle(f, cx + s(70), cy - s(96), cx + s(30), cy - s(122), cx + s(30), cy - s(70));
            FillTriangle(f, cx + s(70), cy - s(96), cx + s(110), cy - s(122), cx + s(110), cy - s(70));
            Disk(f, cx + s(70), cy - s(96), std::max(3, s(12)), false);
            Ring(f, cx + s(70), cy - s(96), std::max(3, s(12)), std::max(3, s(12)), w);
            break;
        case 3: {  // crown with three points
            const int top = cy - s(98);
            FillRect(f, cx - s(56), top, 2 * s(56), s(18));
            for (int k = -1; k <= 1; ++k) {
                const int x = cx + k * s(40);
                FillTriangle(f, x - s(22), top + 1, x + s(22), top + 1, x, top - s(34));
                Disk(f, x, top - s(36), std::max(2, s(7)));
            }
            Disk(f, cx, top + s(9), std::max(2, s(6)), false);
            break;
        }
        case 4:  // flower on the left ear
            for (int k = 0; k < 5; ++k) {
                const double a = (-90 + 72 * k) * kPi / 180.0;
                Disk(f, cx - s(68) + static_cast<int>(std::lround(s(16) * std::cos(a))),
                     cy - s(98) + static_cast<int>(std::lround(s(16) * std::sin(a))),
                     std::max(3, s(11)));
            }
            Disk(f, cx - s(68), cy - s(98), std::max(3, s(9)), false);
            Ring(f, cx - s(68), cy - s(98), std::max(3, s(9)), std::max(3, s(9)), std::max(1, s(3)));
            break;
        default:
            break;
    }
}

// Festival hat for New Year (replaces the head item for the day).
void PartyHat(MonoFrame& f, int cx, int cy, int scale) {
    auto s = [scale](int d) { return d * scale / 100; };
    FillTriangle(f, cx - s(36), cy - s(84), cx + s(36), cy - s(84), cx + s(8), cy - s(146));
    for (int k = 1; k <= 2; ++k)
        Stroke(f, cx - s(36) + k * s(16), cy - s(84) - k * s(20), cx + s(36) - k * s(8),
               cy - s(84) - k * s(20), std::max(2, s(5)), false);
    Disk(f, cx + s(8), cy - s(146), std::max(3, s(12)));
}

void NeckItem(MonoFrame& f, uint8_t item, int cx, int cy, int scale) {
    auto s = [scale](int d) { return d * scale / 100; };
    switch (item) {
        case 1:  // striped scarf with a hanging end
            FillRoundRect(f, cx - s(86), cy + s(92), 2 * s(86), s(28), s(13));
            FillRoundRect(f, cx + s(36), cy + s(104), s(30), s(62), s(10));
            for (int k = -3; k <= 3; ++k)
                Stroke(f, cx + k * s(22), cy + s(96), cx + k * s(22) + s(8), cy + s(116),
                       std::max(2, s(5)), false);
            break;
        case 2:  // bow tie
            FillTriangle(f, cx, cy + s(108), cx - s(40), cy + s(88), cx - s(40), cy + s(128));
            FillTriangle(f, cx, cy + s(108), cx + s(40), cy + s(88), cx + s(40), cy + s(128));
            Disk(f, cx, cy + s(108), std::max(3, s(11)));
            break;
        case 3:  // medal on a ribbon
            Stroke(f, cx - s(26), cy + s(96), cx - s(4), cy + s(136), std::max(3, s(9)));
            Stroke(f, cx + s(26), cy + s(96), cx + s(4), cy + s(136), std::max(3, s(9)));
            Disk(f, cx, cy + s(150), std::max(6, s(22)));
            FilledStar(f, cx, cy + s(151), std::max(4, s(14)), false);
            break;
        default:
            break;
    }
}

void FaceItem(MonoFrame& f, uint8_t item, int cx, int cy, int scale) {
    auto s = [scale](int d) { return d * scale / 100; };
    const int w = std::max(2, s(6)), ex = s(50), ey = cy + s(2);
    if (item == 1) {  // round glasses
        for (int side : {-1, 1}) Ring(f, cx + side * ex, ey, s(32), s(30), w);
        Arc(f, cx, ey + s(4), s(18), s(12), 200, 340, w);
    } else if (item == 2) {  // sunglasses
        for (int side : {-1, 1}) {
            FillEllipse(f, cx + side * ex, ey, s(34), s(26));
            Arc(f, cx + side * ex - s(10), ey - s(8), s(12), s(8), 200, 290, std::max(1, s(4)),
                false);
        }
        Stroke(f, cx - s(18), ey - s(6), cx + s(18), ey - s(6), w);
    }
}

void Accessories(MonoFrame& f, const std::array<uint8_t, 5>& worn, uint8_t festival, int cx,
                 int cy, int scale, bool neck = true) {
    if (neck) NeckItem(f, worn[kSlotNeck], cx, cy, scale);
    FaceItem(f, worn[kSlotFace], cx, cy, scale);
    if (festival == 1) PartyHat(f, cx, cy, scale);  // Festival::NewYear
    else HeadItem(f, worn[kSlotHead], cx, cy, scale);
}

// The little room: a bed bottom left, a lamp on the right (home only).
void RoomBed(MonoFrame& f) {
    RoundRectRing(f, 36, 268, 22, 104, 8, 6);                    // headboard
    FillRoundRect(f, 40, 318, 196, 40, 12, false);
    RoundRectRing(f, 40, 318, 196, 40, 12, 6);                    // mattress
    ShadeRect(f, 110, 324, 120, 28, 5);                           // blanket
    Blob(f, 84, 310, 30, 16, 5);                                  // pillow
    Stroke(f, 48, 358, 48, 374, 6);
    Stroke(f, 228, 358, 228, 374, 6);
}

void RoomLamp(MonoFrame& f, uint8_t item) {
    if (item == 2) {  // star night light
        FilledStar(f, 704, 300, 44);
        Sparkle(f, 650, 250, 10);
        Sparkle(f, 760, 240, 8);
        FillRoundRect(f, 676, 346, 56, 20, 8);
        return;
    }
    FillTriangle(f, 666, 222, 742, 222, 724, 176);
    FillTriangle(f, 666, 222, 684, 176, 724, 176);
    Stroke(f, 704, 222, 704, 348, 7);
    FillEllipse(f, 704, 356, 40, 12);
    for (int deg : {60, 90, 120}) Ray(f, 704, 222, deg, 18, 36, false);  // light (white on cushion)
}

// Festival props on the left, beside the pet.
void FestivalProp(MonoFrame& f, uint8_t festival) {
    const int x = 196, y = 232;
    switch (festival) {
        case 2:  // Spring Festival
        case 3:  // Lantern
            Stroke(f, x, y - 70, x, y - 40, 4);
            FillRect(f, x - 22, y - 44, 44, 10);
            FillEllipse(f, x, y, 46, 38);
            for (int k = -1; k <= 1; ++k) Arc(f, x, y, 16 + 14 * (k + 1), 38, 270 - 20, 270 + 20, 3, false);
            FillRect(f, x - 22, y + 34, 44, 10);
            for (int k = -1; k <= 1; ++k) Stroke(f, x + k * 10, y + 46, x + k * 12, y + 74, 4);
            break;
        case 4:  // Children's Day: a balloon
            Ring(f, x, y - 30, 36, 44, 6);
            Shade(f, x, y - 30, 30, 38, 6);
            FillTriangle(f, x - 8, y + 18, x + 8, y + 18, x, y + 8);
            Arc(f, x + 14, y + 44, 14, 26, 120, 240, 3);
            Arc(f, x - 12, y + 68, 14, 22, -60, 60, 3);
            break;
        case 5:  // Dragon Boat: a rice dumpling
            FillTriangle(f, x, y - 44, x - 50, y + 40, x + 50, y + 40);
            FillTriangle(f, x, y - 30, x - 34, y + 30, x + 34, y + 30, false);
            FillTriangle(f, x, y - 30, x - 34, y + 30, x + 34, y + 30, true, 6);
            Stroke(f, x - 40, y + 10, x + 40, y + 10, 6);
            break;
        case 6:  // Mid-Autumn: moon and a mooncake
            Moon(f, x - 96, y - 28, 24, true);
            Disk(f, x, y + 10, 44);
            Ring(f, x, y + 10, 30, 30, 4, false);
            FilledStar(f, x, y + 12, 16, false);
            break;
        case 7: {  // Birthday: a cake with a candle
            FillRoundRect(f, x - 52, y + 2, 104, 50, 10);
            FillRoundRect(f, x - 52, y + 2, 104, 18, 8, false);
            RoundRectRing(f, x - 52, y + 2, 104, 50, 10, 5);
            for (int k = -2; k <= 2; ++k) Disk(f, x + k * 20, y + 20, 6, false);
            FillRect(f, x - 5, y - 34, 10, 34);
            FillEllipse(f, x, y - 46, 8, 12);
            break;
        }
        default:
            break;
    }
}

// "I would like ...": a thought bubble left of the head (level >= 2).
void NeedBubble(MonoFrame& f, uint8_t need, uint8_t level) {
    if (!need || level < 2) return;
    const int bx = 214, by = 104, w = level >= 3 ? 7 : 4;
    Blob(f, bx, by, 46, 38, w);
    Ring(f, 262, 140, 8, 8, 3);
    Ring(f, 278, 156, 5, 5, 3);
    switch (need) {
        case 1:  // hungry: a fish
            GameArtIcon(f, static_cast<uint16_t>(GameArt::Fish), bx, by, 26);
            break;
        case 2:  // sleepy: moon and a Z
            Moon(f, bx - 8, by + 2, 18, true);
            LetterZ(f, bx + 18, by - 10, 14, 4);
            break;
        case 3:  // dirty: mud spots and a water drop
            for (int k = 0; k < 3; ++k) Shade(f, bx - 20 + k * 12, by + 12 - (k % 2) * 8, 8, 6, 10);
            FillEllipse(f, bx + 18, by - 2, 10, 13);
            FillTriangle(f, bx + 10, by - 6, bx + 26, by - 6, bx + 18, by - 24);
            break;
        case 4:  // bored: a ball
            GameBall(f, bx, by, 22);
            break;
        default:
            break;
    }
}

// ---- care pictures for cards (CareArt) ----

void HabitIcon(MonoFrame& f, uint8_t habit, int cx, int cy, int r) {
    auto p = [r](int percent) { return r * percent / 100; };
    const int w = std::max(3, r / 10);
    switch (habit) {
        case 1:  // brush teeth: a toothbrush
            Stroke(f, cx - p(80), cy + p(40), cx + p(40), cy - p(20), std::max(4, p(16)));
            FillRoundRect(f, cx + p(24), cy - p(58), p(54), p(30), p(8));
            for (int k = 0; k < 4; ++k)
                Stroke(f, cx + p(30) + k * p(14), cy - p(58), cx + p(30) + k * p(14), cy - p(80), w);
            break;
        case 2:  // wash hands: a palm and drops
            Blob(f, cx - p(10), cy + p(14), p(42), p(52), w);
            for (int k = 0; k < 3; ++k) {
                const int x = cx + p(52) + (k % 2) * p(14), y = cy - p(50) + k * p(34);
                FillEllipse(f, x, y, p(9), p(12));
                FillTriangle(f, x - p(8), y - p(4), x + p(8), y - p(4), x, y - p(22));
            }
            break;
        case 3:  // tidy toys: a toy box
            FillRoundRect(f, cx - p(70), cy - p(10), p(140), p(70), p(10));
            GameBall(f, cx - p(26), cy - p(30), p(24));
            FillRect(f, cx + p(10), cy - p(50), p(40), p(40));
            break;
        case 4:  // drink water: a cup
            FillTriangle(f, cx - p(46), cy - p(60), cx + p(46), cy - p(60), cx + p(32), cy + p(70));
            FillTriangle(f, cx - p(46), cy - p(60), cx - p(32), cy + p(70), cx + p(32), cy + p(70));
            FillTriangle(f, cx - p(34), cy - p(46), cx + p(34), cy - p(46), cx, cy - p(30), false);
            Arc(f, cx + p(46), cy, p(22), p(26), -80, 80, w);
            break;
        case 5:  // dress self: a T-shirt
            FillRect(f, cx - p(40), cy - p(50), p(80), p(110));
            FillTriangle(f, cx - p(40), cy - p(50), cx - p(90), cy - p(10), cx - p(40), cy + p(10));
            FillTriangle(f, cx + p(40), cy - p(50), cx + p(90), cy - p(10), cx + p(40), cy + p(10));
            Disk(f, cx, cy - p(50), p(18), false);
            break;
        case 6:  // eat veggies: broccoli
            Stroke(f, cx, cy + p(70), cx, cy, std::max(4, p(20)));
            for (int k = -1; k <= 1; ++k) Disk(f, cx + k * p(34), cy - p(20) - (k == 0) * p(24), p(32));
            break;
        case 7:  // bed on time: moon over a pillow
            Moon(f, cx, cy - p(30), p(46), true);
            Blob(f, cx, cy + p(56), p(70), p(22), w);
            break;
        case 8:  // read: the rail's open book
            RailIcon(f, 1, cx, cy, true);
            break;
        default:
            break;
    }
}

void FoodIcon(MonoFrame& f, uint8_t food, int cx, int cy, int r) {
    auto p = [r](int percent) { return r * percent / 100; };
    const int w = std::max(3, r / 10);
    switch (food) {
        case 1:  // fish
            GameArtIcon(f, static_cast<uint16_t>(GameArt::Fish), cx, cy, r);
            break;
        case 2:  // milk bottle
            FillRoundRect(f, cx - p(36), cy - p(40), p(72), p(110), p(16));
            FillRect(f, cx - p(20), cy - p(70), p(40), p(34));
            FillRect(f, cx - p(26), cy - p(84), p(52), p(16));
            ShadeRect(f, cx - p(26), cy - p(10), p(52), p(60), 4);
            break;
        case 3:  // apple
            Disk(f, cx - p(22), cy + p(10), p(48));
            Disk(f, cx + p(22), cy + p(10), p(48));
            Stroke(f, cx, cy - p(40), cx + p(8), cy - p(78), w);
            FillEllipse(f, cx + p(32), cy - p(66), p(22), p(10));
            Disk(f, cx - p(28), cy - p(10), p(10), false);
            break;
        case 4:  // carrot
            FillTriangle(f, cx - p(30), cy - p(40), cx + p(30), cy - p(40), cx, cy + p(90));
            for (int k = 0; k < 3; ++k)
                Stroke(f, cx - p(14) + k * p(8), cy - p(10) + k * p(26), cx + p(10) - k * p(4),
                       cy - p(4) + k * p(26), std::max(2, w / 2), false);
            for (int k = -1; k <= 1; ++k) Stroke(f, cx, cy - p(42), cx + k * p(30), cy - p(88), w);
            break;
        case 5:  // cookie
            Disk(f, cx, cy, p(76));
            for (int k = 0; k < 5; ++k) {
                const double a = (k * 72 + 20) * kPi / 180.0;
                Disk(f, cx + static_cast<int>(std::lround(p(40) * std::cos(a))),
                     cy + static_cast<int>(std::lround(p(40) * std::sin(a))), p(10), false);
            }
            break;
        case 6:  // rice ball
            FillTriangle(f, cx, cy - p(80), cx - p(80), cy + p(60), cx + p(80), cy + p(60), false);
            for (int k = 0; k < 3; ++k) {
                const int t = std::max(3, w);
                Stroke(f, cx, cy - p(80) + k, cx - p(80) + k, cy + p(60), t);
                Stroke(f, cx, cy - p(80) + k, cx + p(80) - k, cy + p(60), t);
            }
            Stroke(f, cx - p(80), cy + p(60), cx + p(80), cy + p(60), w);
            FillRect(f, cx - p(30), cy + p(10), p(60), p(50));
            break;
        default:
            break;
    }
}

void CareArtIcon(MonoFrame& f, uint16_t art, int cx, int cy, int r) {
    auto p = [r](int percent) { return r * percent / 100; };
    const int w = std::max(3, r / 10);
    if (art >= static_cast<uint16_t>(CareArt::Fish) && art <= static_cast<uint16_t>(CareArt::RiceBall)) {
        FoodIcon(f, static_cast<uint8_t>(art - static_cast<uint16_t>(CareArt::Fish) + 1), cx, cy, r);
        return;
    }
    if (art >= static_cast<uint16_t>(CareArt::HabitFirst) && art <= static_cast<uint16_t>(CareArt::HabitLast)) {
        HabitIcon(f, static_cast<uint8_t>(art - 250), cx, cy, r);
        return;
    }
    if (art >= static_cast<uint16_t>(CareArt::WearFirst) && art <= static_cast<uint16_t>(CareArt::WearLast)) {
        // The wardrobe item on a small TuanTuan head (wardrobe table order).
        static const uint8_t kSlot[12] = {0, 0, 0, 0, 1, 1, 1, 2, 2, 3, 4, 4};
        static const uint8_t kItem[12] = {1, 2, 3, 4, 1, 2, 3, 1, 2, 1, 1, 2};
        const size_t i = art - static_cast<uint16_t>(CareArt::WearFirst);
        if (kSlot[i] == kSlotBed) {
            FillRoundRect(f, cx - p(80), cy, p(160), p(40), p(10));
            Blob(f, cx - p(46), cy - p(10), p(28), p(16), w);
            FillRect(f, cx - p(86), cy - p(40), p(12), p(90));
            return;
        }
        if (kSlot[i] == kSlotLamp) {
            if (kItem[i] == 2) {
                FilledStar(f, cx, cy, p(80));
            } else {
                FillTriangle(f, cx - p(50), cy - p(20), cx + p(50), cy - p(20), cx + p(26), cy - p(80));
                FillTriangle(f, cx - p(50), cy - p(20), cx - p(26), cy - p(80), cx + p(26), cy - p(80));
                Stroke(f, cx, cy - p(20), cx, cy + p(70), w);
                FillEllipse(f, cx, cy + p(76), p(44), p(12));
            }
            return;
        }
        const int scale = r * 100 / 170;
        const int hy = cy + p(30);
        CatHead(f, cx, hy, scale, Emotion::Happy);
        std::array<uint8_t, 5> worn{};
        worn[kSlot[i]] = kItem[i];
        Accessories(f, worn, 0, cx, hy, scale);
        return;
    }
    switch (static_cast<CareArt>(art)) {
        case CareArt::Bubbles:
            Ring(f, cx - p(30), cy + p(20), p(40), p(40), w);
            Ring(f, cx + p(36), cy - p(10), p(30), p(30), w);
            Ring(f, cx + p(10), cy - p(60), p(18), p(18), w);
            Arc(f, cx - p(40), cy + p(8), p(18), p(18), 200, 260, w);
            break;
        case CareArt::Comb:
            FillRoundRect(f, cx - p(80), cy - p(50), p(160), p(36), p(12));
            for (int k = 0; k < 8; ++k)
                FillRect(f, cx - p(74) + k * p(20), cy - p(16), p(10), p(64));
            break;
        case CareArt::Feather:
            FillEllipse(f, cx + p(8), cy - p(10), p(30), p(70));
            Stroke(f, cx - p(40), cy + p(80), cx + p(26), cy - p(70), std::max(2, w / 2), false);
            Stroke(f, cx - p(40), cy + p(80), cx - p(20), cy + p(36), w);
            break;
        case CareArt::Hanger:
            Arc(f, cx, cy - p(56), p(18), p(18), 180, 400, w);
            Stroke(f, cx, cy - p(38), cx - p(84), cy + p(36), w);
            Stroke(f, cx, cy - p(38), cx + p(84), cy + p(36), w);
            Stroke(f, cx - p(84), cy + p(36), cx + p(84), cy + p(36), w);
            break;
        case CareArt::Check:
            RoundRectRing(f, cx - p(70), cy - p(70), p(140), p(140), p(20), w);
            CheckMark(f, cx, cy, p(46), std::max(4, p(14)), true);
            break;
        case CareArt::Sticker:  // the album: a star sticker with a peeled corner
            Disk(f, cx, cy, p(80));
            FilledStar(f, cx, cy + p(4), p(52), false);
            FillTriangle(f, cx + p(40), cy + p(70), cx + p(80), cy + p(30), cx + p(84), cy + p(84),
                         false);
            Stroke(f, cx + p(40), cy + p(70), cx + p(80), cy + p(30), w);
            break;
        case CareArt::ToyBlock:
            FillRect(f, cx - p(50), cy - p(50), p(100), p(100));
            FillRect(f, cx - p(30), cy - p(30), p(60), p(60), false);
            Disk(f, cx, cy, p(14));
            break;
        case CareArt::ToyBall:
            GameBall(f, cx, cy, p(70));
            break;
        case CareArt::ToyBear:
            for (int side : {-1, 1}) Disk(f, cx + side * p(44), cy - p(46), p(22));
            Blob(f, cx, cy, p(58), p(54), w);
            for (int side : {-1, 1}) Disk(f, cx + side * p(20), cy - p(8), p(8));
            FillEllipse(f, cx, cy + p(18), p(12), p(9));
            break;
        case CareArt::Lock:
            Arc(f, cx, cy - p(20), p(36), p(44), 180, 360, std::max(4, p(14)));
            FillRoundRect(f, cx - p(56), cy - p(20), p(112), p(90), p(14));
            Disk(f, cx, cy + p(16), p(12), false);
            FillRect(f, cx - p(5), cy + p(16), p(10), p(30), false);
            break;
        default:
            break;
    }
}

// ---- care scenes ----

// Progress as dots: done filled, the rest rings (right panel, y = 250).
void ProgressDots(MonoFrame& f, uint8_t done, uint8_t goal, int cx, int y) {
    if (!goal) return;
    const int pitch = 44, x0 = cx - (goal - 1) * pitch / 2;
    for (uint8_t i = 0; i < goal; ++i) {
        if (i < done) Disk(f, x0 + i * pitch, y, 14);
        else Ring(f, x0 + i * pitch, y, 13, 13, 4);
    }
}

void Hearts(MonoFrame& f, uint8_t n, int cx, int cy) {
    for (uint8_t i = 0; i < n && i < 3; ++i) Heart(f, cx + (static_cast<int>(i) - (n - 1) / 2) * 70, cy, 20);
}

// One sticker badge: earned = a ringed picture, not yet = a dashed ring
// with a question mark. Pictures reuse the rail and care icons.
void StickerBadge(MonoFrame& f, uint8_t id, bool earned, int cx, int cy) {
    if (!earned) {
        for (int deg = 0; deg < 360; deg += 30) Arc(f, cx, cy, 62, 62, deg, deg + 16, 5);
        QuestionMark(f, cx, cy - 6, 20, 8);
        return;
    }
    Ring(f, cx, cy, 64, 64, 9);
    auto count_dots = [&](int n) {
        for (int k = 0; k < n; ++k) Disk(f, cx - (n - 1) * 9 + k * 18, cy + 40, 6);
    };
    switch (id) {
        case 0: case 1: case 2:  // books 1 / 5 / 10
            RailIcon(f, 1, cx, cy - 8, true);
            count_dots(id + 1);
            break;
        case 3: case 4:  // games 1 / 10
            RailIcon(f, 2, cx, cy - 8, true);
            count_dots(id - 2);
            break;
        case 5: Heart(f, cx, cy - 16, 22); break;                     // first like
        case 6: FilledStar(f, cx, cy, 40); count_dots(3); break;      // seven days
        case 7: case 8:                                               // feeds 1 / 10
            FoodIcon(f, 1, cx, cy - 8, 34);
            count_dots(id - 6);
            break;
        case 9: CareArtIcon(f, static_cast<uint16_t>(CareArt::Bubbles), cx, cy, 38); break;
        case 10: CareArtIcon(f, static_cast<uint16_t>(CareArt::Comb), cx, cy, 38); break;
        case 11: CareArtIcon(f, static_cast<uint16_t>(CareArt::Feather), cx, cy, 38); break;
        case 12: Moon(f, cx - 4, cy, 30, true); break;
        case 13: case 14: case 15:  // habit 3 / 7 / 14 days
            CheckMark(f, cx, cy - 10, 26, 9, true);
            count_dots(id - 12);
            break;
        case 16:  // birthday: a small cake with a candle
            FillRoundRect(f, cx - 34, cy - 4, 68, 34, 8);
            FillRect(f, cx - 4, cy - 30, 8, 26);
            FillEllipse(f, cx, cy - 38, 6, 9);
            break;
        case 17:  // festival: a lantern
            FillEllipse(f, cx, cy, 30, 24);
            FillRect(f, cx - 14, cy - 30, 28, 8);
            FillRect(f, cx - 14, cy + 22, 28, 8);
            break;
        case 18: FilledStar(f, cx, cy, 46); break;  // care star
        default: QuestionMark(f, cx, cy - 6, 20, 8); break;
    }
}

void AlbumScene(MonoFrame& f, const CareView& c) {
    const int per_page = 6;
    for (int i = 0; i < per_page; ++i) {
        const int id = c.page * per_page + i;
        if (id >= 19) break;  // kStickerCount
        const int cx = 170 + (i % 3) * 230, cy = 132 + (i / 3) * 180;
        StickerBadge(f, static_cast<uint8_t>(id), (c.stickers >> id) & 1u, cx, cy);
    }
    for (int p = 0; p < c.pages; ++p) {
        if (p == c.page) Disk(f, 400 + (2 * p - (c.pages - 1)) * 22, 440, 11);
        else Ring(f, 400 + (2 * p - (c.pages - 1)) * 22, 440, 9, 9, 3);
    }
}

void DiaryScene(MonoFrame& f, const CareView& c) {
    CatHead(f, 150, 190, 60, Emotion::Happy, Eyes::Laugh);
    Accessories(f, c.worn, 0, 150, 190, 60);
    // The page: one row per thing that happened today.
    RoundRectRing(f, 300, 40, 460, 400, 24, 6);
    Stroke(f, 330, 40, 330, 440, 3);
    int y = 92;
    auto row = [&]() { const int at = y; y += 76; return at; };
    if (c.food) FoodIcon(f, c.food, 420, row(), 34);
    if (c.book) RailIcon(f, 1, 420, row(), true);
    if (c.game) RailIcon(f, 2, 420, row(), true);
    if (c.cares) {
        const int at = row();
        for (int k = 0; k < c.cares && k < 4; ++k) Heart(f, 390 + k * 64, at - 12, 18);
    }
    if (c.habit_done) {
        const int at = row();
        CheckMark(f, 420, at, 28, 10, true);
    }
    if (y == 92) {  // a quiet day: a calm moon
        Moon(f, 530, 220, 60, true);
        Sparkle(f, 640, 140, 16);
    }
}

void CareScene(MonoFrame& f, const CareView& c) {
    if (c.kind == CareKind::Album) {
        AlbumScene(f, c);
        return;
    }
    if (c.kind == CareKind::Diary) {
        DiaryScene(f, c);
        return;
    }
    const bool resting = c.kind == CareKind::Lull && c.goal && c.progress * 2 >= c.goal;
    const bool happy = c.hearts > 0;
    const Emotion emotion = happy ? Emotion::Happy : resting ? Emotion::Sleepy : Emotion::Neutral;
    const Eyes eyes = happy ? Eyes::Laugh : resting ? Eyes::Closed : Eyes::ByEmotion;
    const bool dress = c.kind == CareKind::Dress;
    const int hx = dress ? 400 : 200, hy = dress ? 160 : 160, scale = 80;
    CatHead(f, hx, hy, scale, emotion, eyes);
    Accessories(f, c.worn, 0, hx, hy, scale);
    // Hearts after a care, top right (clear of the corner boxes).
    if (happy && !dress && c.kind != CareKind::Feed) Hearts(f, c.hearts, 580, 64);
    if (dress) {
        if (happy) Heart(f, 560, 70, 20);
    } else {
        const int px = 580;
        switch (c.kind) {
            case CareKind::Feed:  // the bowl
                FillEllipse(f, px, 210, 110, 34);
                FillEllipse(f, px, 196, 96, 18, false);
                Ring(f, px, 196, 96, 18, 5);
                if (c.hearts) Hearts(f, c.hearts, px, 100);
                break;
            case CareKind::Bath:
                Ring(f, px, 230, 140, 46, 7);
                ShadeRect(f, px - 120, 232, 240, 30, 4);
                for (uint8_t i = 0; i < c.progress && i < 3; ++i)
                    Ring(f, px - 80 + i * 80, 170 - (i % 2) * 40, 26 + i * 4, 26 + i * 4, 5);
                if (c.phase == 1 && !happy) {  // rinse: turn the board over
                    Arc(f, px, 90, 60, 40, 200, 340, 7);
                    FillTriangle(f, px + 56, 76, px + 76, 100, px + 40, 104);
                    for (int k = -2; k <= 2; ++k) FillEllipse(f, px + k * 26, 140 + (k & 1) * 10, 5, 9);
                }
                break;
            case CareKind::Brush:
                CareArtIcon(f, static_cast<uint16_t>(CareArt::Comb), px, 130, 70);
                ProgressDots(f, c.progress, c.goal, px, 250);
                break;
            case CareKind::Tickle:
                CareArtIcon(f, static_cast<uint16_t>(CareArt::Feather), px, 130, 70);
                ProgressDots(f, c.progress, c.goal, px, 250);
                break;
            case CareKind::Lull:
                Moon(f, px - 20, 110, 50, true);
                LetterZ(f, px + 60, 80, 26, 6);
                ProgressDots(f, static_cast<uint8_t>(c.progress / 2), static_cast<uint8_t>(c.goal / 2), px,
                             250);
                break;
            case CareKind::Habit:
                if (c.option_count) {
                    CareArtIcon(f, c.options[0], px, 150, 80);
                    if (c.phase) {  // ticked today: a big check in a ring
                        Disk(f, px + 90, 230, 44, false);
                        Ring(f, px + 90, 230, 44, 44, 7);
                        CheckMark(f, px + 90, 230, 26, 9, true);
                    }
                }
                break;
            case CareKind::Tidy: {  // toys go into the box one by one
                const uint16_t toys[3] = {static_cast<uint16_t>(CareArt::ToyBlock),
                                          static_cast<uint16_t>(CareArt::ToyBall),
                                          static_cast<uint16_t>(CareArt::ToyBear)};
                for (uint8_t i = 0; i < 3; ++i) {  // in the box: peeking over the edge
                    if (i < c.progress) CareArtIcon(f, toys[i], 520 + i * 80, 160, 34);
                }
                FillRoundRect(f, 460, 178, 260, 100, 16);
                FillRoundRect(f, 476, 194, 228, 20, 8, false);
                for (uint8_t i = 0; i < 3; ++i) {  // still on the floor
                    if (i >= c.progress) CareArtIcon(f, toys[i], 380 + (i % 2) * 40, 80 + i * 90, 38);
                }
                break;
            }
            default:
                break;
        }
    }
    // Cards, exactly like the game answer rail.
    const uint8_t n = std::min<uint8_t>(c.option_count, 3);
    for (uint8_t i = 0; i < n; ++i) {
        const int x = GameCardX(i, n);
        RoundRectRing(f, x, kGameCardY, kGameCardW, kGameCardH, 24, 5);
        GameArtIcon(f, c.options[i], x + kGameCardW / 2, kGameCardY + kGameCardH / 2, 58);
        if (i == c.highlight)
            RoundRectRing(f, x - kGameFrame, kGameCardY - kGameFrame, kGameCardW + 2 * kGameFrame,
                          kGameCardH + 2 * kGameFrame, 24 + kGameFrame, 10);
    }
}

// ---- Scenes --------------------------------------------------------------------

void HomeScene(MonoFrame& f, const SceneModel& model) {
    const HomeLook& look = model.look;
    const bool bed = look.worn[kSlotBed] != 0, lamp = look.worn[kSlotLamp] != 0;
    const bool prop = look.festival >= 2, bubble = look.need && look.need_level >= 2;
    // Background twinkles and a dithered cushion under the pet (V8: none
    // under the room items, the festival prop or the need bubble).
    if (!bubble) Sparkle(f, 110, 90, 18);
    if (!prop && !bubble) Sparkle(f, 160, 150, 9);
    // Not under the thought cloud or the low-battery bubble.
    if (model.emotion != Emotion::Thinking && model.pose != ScenePose::Yawn &&
        model.pose != ScenePose::TipHot && model.pose != ScenePose::TipCold)
        Sparkle(f, 690, 80, 14);
    if (!lamp) Sparkle(f, 640, 220, 10);
    if (!bed) Sparkle(f, 96, 280, 8);
    if (!lamp) Disk(f, 720, 300, 4);
    if (!prop) Disk(f, 70, 190, 4);
    Shade(f, 400, 356, 200, 20, 6);
    Ring(f, 400, 356, 200, 20, 3);
    if (bed) RoomBed(f);
    if (lamp) RoomLamp(f, look.worn[kSlotLamp]);
    if (prop) FestivalProp(f, look.festival);
    if (model.pose == ScenePose::Sunrise || model.pose == ScenePose::Morning)
        MorningSun(f, model.pose == ScenePose::Morning);
    // V8: a resting, neutral TuanTuan shows how she feels; voice faces,
    // animations and battery / tip pictures keep their own face.
    Emotion emotion = model.emotion;
    const bool resting = model.pose == ScenePose::Still && emotion == Emotion::Neutral;
    if (resting && bubble && look.need == 2) emotion = Emotion::Sleepy;   // Need::Sleepy
    if (resting && bubble && look.need == 4) emotion = Emotion::Curious;  // Need::Bored
    SittingCat(f, 400, 158, emotion, model.pose);
    Accessories(f, look.worn, look.festival, 400, 158, 100);
    if (bubble && model.pose == ScenePose::Still) NeedBubble(f, look.need, look.need_level);
    ActionRail(f, model.home_selection);
    StatusBand(f, model.status);
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
    // V8 series: a small book with an arrow, "the next book is waiting"
    // (book_page bit 14, see BookScene).
    if ((model.book_page & 0x4000) != 0) {
        FillRoundRect(f, 640, 330, 70, 50, 8);
        FillRoundRect(f, 646, 336, 58, 38, 4, false);
        Stroke(f, 722, 355, 756, 355, 6);
        Stroke(f, 744, 343, 756, 355, 6);
        Stroke(f, 744, 367, 756, 355, 6);
    }
}

// No book to read (no fonts: the child reads the icons). TuanTuan wonders
// on the left; on the right a memory card with an arrow into the slot (no
// card) or an open book with blank pages (no book); at the bottom the home
// card that a press / long press goes back to.
void BookMissingPicture(MonoFrame& f, BookMissing reason) {
    CatHead(f, 190, 200, 82, Emotion::Curious);
    QuestionMark(f, 318, 82, 26, 10);
    const int cx = 560;
    if (reason == BookMissing::NoCard) {
        // Card with a clipped corner and contacts, an arrow down into a slot.
        const int x = 490, y = 40, w = 140, h = 180, cut = 40;
        FillRoundRect(f, x, y, w, h, 14);
        FillRoundRect(f, x + 10, y + 10, w - 20, h - 20, 8, false);
        FillTriangle(f, x + w - cut - 1, y - 1, x + w + 1, y - 1, x + w + 1, y + cut + 1, false);
        Stroke(f, x + w - cut, y + 2, x + w - 2, y + cut, 10);
        for (int i = 0; i < 4; ++i) FillRoundRect(f, x + 24 + i * 22, y + 26, 12, 40, 4);
        Stroke(f, cx, y + h + 14, cx, 300, 16);
        FillTriangle(f, cx - 40, 290, cx + 40, 290, cx, 336);
        FillRoundRect(f, cx - 110, 344, 220, 16, 8);
    } else {
        // Open book, both pages left blank.
        for (int side : {-1, 1}) {
            Stroke(f, cx, 96, cx + side * 90, 66, 10);
            Stroke(f, cx + side * 90, 66, cx + side * 170, 84, 10);
            Stroke(f, cx + side * 170, 84, cx + side * 170, 290, 10);
            Stroke(f, cx + side * 170, 290, cx + side * 90, 272, 10);
            Stroke(f, cx + side * 90, 272, cx, 302, 10);
        }
        Stroke(f, cx, 96, cx, 302, 10);
        for (int i = 0; i < 3; ++i) Disk(f, cx - 40 + i * 40, 336, 7);  // "nothing here"
    }
    // Home: the inverted rail card with the cat's house.
    const int hx = 400, hy = 420;
    FillRoundRect(f, hx - 72, hy - 46, 144, 92, 20);
    FillTriangle(f, hx - 40, hy - 4, hx + 40, hy - 4, hx, hy - 36, false);
    FillRoundRect(f, hx - 28, hy - 6, 56, 38, 4, false);
    FillRoundRect(f, hx - 9, hy + 10, 18, 22, 3);
}

// Fixed picture kept on the unpowered ePaper: night sky, moon, and TuanTuan
// curled up asleep on a cushion.
void SleepScene(MonoFrame& f, ScenePose pose) {
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
    CatHead(f, hx, hy, 72, Emotion::Neutral, Eyes::Closed, /*halo=*/true);
    Blob(f, hx - 32, hy + 70, 26, 14, 5);
    Blob(f, hx + 32, hy + 70, 26, 14, 5);

    // Zzz rising into the sky; the falling-asleep animation adds one at a time.
    const int letters = pose == ScenePose::Zzz1 ? 1 : pose == ScenePose::Zzz2 ? 2 : 3;
    LetterZ(f, 196, 170, 20, 6, false);
    if (letters >= 2) LetterZ(f, 146, 110, 28, 7, false);
    if (letters >= 3) LetterZ(f, 84, 42, 38, 8, false);
}

}  // namespace

void SceneComposer::Render(const SceneModel& model, MonoFrame& output) {
    output.Clear();
    if (BookMissingOf(model) != BookMissing::None) {
        BookMissingPicture(output, BookMissingOf(model));
    } else if (model.kind == SceneKind::Reader) {
        ReaderScene(output, model);
    } else if (model.kind == SceneKind::Sleep) {
        SleepScene(output, model.pose);
    } else if (model.kind == SceneKind::Parent) {
        ParentScene(output, model.parent);
    } else if (model.kind == SceneKind::Game) {
        GameScene(output, model.game);
    } else if (model.kind == SceneKind::Care) {
        CareScene(output, model.care);  // V8
    } else {
        HomeScene(output, model);
    }
}

}  // namespace kid_app
