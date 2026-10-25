#include "kid_app/scene_model.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace kid_app {

void MonoFrame::Clear() { pixels_.fill(0xFF); }

void MonoFrame::SetPixel(int x, int y, bool black) {
    if (x < 0 || x >= kWidth || y < 0 || y >= kHeight) return;
    const auto index = static_cast<size_t>(y * kStride + x / 8);
    const uint8_t mask = static_cast<uint8_t>(0x80u >> (x % 8));
    if (black) pixels_[index] &= static_cast<uint8_t>(~mask);
    else pixels_[index] |= mask;
}

bool MonoFrame::IsBlack(int x, int y) const {
    if (x < 0 || x >= kWidth || y < 0 || y >= kHeight) return false;
    const auto index = static_cast<size_t>(y * kStride + x / 8);
    return (pixels_[index] & (0x80u >> (x % 8))) == 0;
}

namespace {

void Line(MonoFrame& f, int x0, int y0, int x1, int y1) {
    const int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (true) {
        f.SetPixel(x0, y0, true);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void Rectangle(MonoFrame& f, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    Line(f, x, y, x + w - 1, y);
    Line(f, x, y, x, y + h - 1);
    Line(f, x + w - 1, y, x + w - 1, y + h - 1);
    Line(f, x, y + h - 1, x + w - 1, y + h - 1);
}

void Ellipse(MonoFrame& f, int cx, int cy, int rx, int ry, bool filled) {
    if (rx <= 0 || ry <= 0) return;
    const int64_t rx2 = static_cast<int64_t>(rx) * rx;
    const int64_t ry2 = static_cast<int64_t>(ry) * ry;
    const int64_t limit = rx2 * ry2;
    const int64_t inner_rx2 = static_cast<int64_t>(rx - 1) * (rx - 1);
    const int64_t inner_ry2 = static_cast<int64_t>(ry - 1) * (ry - 1);
    const int64_t inner_limit = inner_rx2 * inner_ry2;

    for (int y = std::max(0, cy - ry); y <= std::min(MonoFrame::kHeight - 1, cy + ry); ++y) {
        for (int x = std::max(0, cx - rx); x <= std::min(MonoFrame::kWidth - 1, cx + rx); ++x) {
            const int64_t dx = x - cx, dy = y - cy;
            const bool in_outer = dx * dx * ry2 + dy * dy * rx2 <= limit;
            if (!in_outer) continue;
            const bool in_inner =
                rx > 1 && ry > 1 &&
                dx * dx * inner_ry2 + dy * dy * inner_rx2 <= inner_limit;
            if (filled || !in_inner) f.SetPixel(x, y, true);
        }
    }
}

void ThickLine(MonoFrame& f, int x0, int y0, int x1, int y1, int thickness) {
    for (int offset = -thickness / 2; offset <= thickness / 2; ++offset) {
        Line(f, x0, y0 + offset, x1, y1 + offset);
    }
}

void PetFace(MonoFrame& f, int cx, int cy, int scale, Emotion emotion) {
    // Scale in percent: 100 for home, 55 for book. Static shape only.
    auto s = [scale](int d) { return d * scale / 100; };
    const int rx = s(125), ry = s(108);
    Ellipse(f, cx, cy, rx, ry, false);
    // Ears: fixed line art intended for 1-bit ePaper, not animation.
    Line(f, cx - s(96), cy - s(53), cx - s(105), cy - s(155));
    Line(f, cx - s(105), cy - s(155), cx - s(27), cy - s(101));
    Line(f, cx + s(96), cy - s(53), cx + s(105), cy - s(155));
    Line(f, cx + s(105), cy - s(155), cx + s(27), cy - s(101));

    const int ex = s(47), ey = cy - s(8);
    if (emotion == Emotion::Happy) {
        ThickLine(f, cx - ex - s(18), ey, cx - ex, ey - s(12), std::max(1, s(4)));
        ThickLine(f, cx - ex, ey - s(12), cx - ex + s(18), ey, std::max(1, s(4)));
        ThickLine(f, cx + ex - s(18), ey, cx + ex, ey - s(12), std::max(1, s(4)));
        ThickLine(f, cx + ex, ey - s(12), cx + ex + s(18), ey, std::max(1, s(4)));
    } else if (emotion == Emotion::Sleepy) {
        ThickLine(f, cx - ex - s(20), ey, cx - ex + s(20), ey, std::max(1, s(4)));
        ThickLine(f, cx + ex - s(20), ey, cx + ex + s(20), ey, std::max(1, s(4)));
    } else {
        Ellipse(f, cx - ex, ey, std::max(2, s(emotion == Emotion::Surprised ? 12 : 8)),
                std::max(2, s(emotion == Emotion::Surprised ? 15 : 10)), true);
        Ellipse(f, cx + ex, ey, std::max(2, s(emotion == Emotion::Curious ? 13 : 8)),
                std::max(2, s(emotion == Emotion::Curious ? 14 : 10)), true);
    }

    // Nose and mouth: use separate discrete poses, never per-audio-frame updates.
    Ellipse(f, cx, cy + s(30), std::max(2, s(7)), std::max(2, s(5)), true);
    if (emotion == Emotion::Sleepy || emotion == Emotion::Surprised) {
        Ellipse(f, cx, cy + s(57), std::max(2, s(12)), std::max(2, s(15)), false);
    } else if (emotion == Emotion::Curious) {
        Line(f, cx - s(12), cy + s(60), cx + s(12), cy + s(60));
    } else {
        Line(f, cx - s(27), cy + s(48), cx - s(12), cy + s(64));
        Line(f, cx - s(12), cy + s(64), cx + s(12), cy + s(64));
        Line(f, cx + s(12), cy + s(64), cx + s(27), cy + s(48));
    }
}

void HomeScene(MonoFrame& f, Emotion emotion) {
    PetFace(f, 400, 252, 100, emotion);
    // Minimal non-text navigation indicators, adapted later by LVGL.
    Rectangle(f, 100, 425, 600, 2);
    Ellipse(f, 345, 452, 7, 7, true);
    Ellipse(f, 400, 452, 7, 7, false);
    Ellipse(f, 455, 452, 7, 7, false);
}

void ReaderScene(MonoFrame& f, const SceneModel& model) {
    Rectangle(f, 38, 30, 724, 414);
    Rectangle(f, 56, 48, 688, 378);
    Line(f, 400, 48, 400, 425);
    PetFace(f, 220, 245, 55, model.emotion);
    const int indicators = 5;
    for (int i = 0; i < indicators; ++i) {
        Ellipse(f, 575 + i * 28, 387, 7, 7,
                static_cast<int>(model.book_page % indicators) == i);
    }
}

}  // namespace

void SceneComposer::Render(const SceneModel& model, MonoFrame& output) {
    output.Clear();
    if (model.kind == SceneKind::Reader) {
        ReaderScene(output, model);
    } else {
        HomeScene(output, model.emotion);
    }
}

}  // namespace kid_app
