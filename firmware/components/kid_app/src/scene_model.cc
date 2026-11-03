#include "kid_app/scene_model.h"

#include <algorithm>
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

void Crescent(MonoFrame& f, int cx, int cy, int r, int offset) {
    for (int y = cy - r; y <= cy + r; ++y) {
        for (int x = cx - r; x <= cx + r; ++x) {
            const int dx = x - cx, dy = y - cy, ox = dx - offset;
            if (dx * dx + dy * dy <= r * r && ox * ox + dy * dy > r * r)
                f.SetPixel(x, y, true);
        }
    }
}

// Rail icons: bold line art centred on (cx, cy), readable without text.
void RailIcon(MonoFrame& f, int slot, int cx, int cy) {
    switch (slot) {
        case 0:  // Chat: speech bubble with tail and three dots
            Ellipse(f, cx, cy - 6, 36, 24, false);
            Ellipse(f, cx, cy - 6, 35, 23, false);
            ThickLine(f, cx - 18, cy + 14, cx - 28, cy + 30, 3);
            ThickLine(f, cx - 28, cy + 30, cx - 4, cy + 17, 3);
            for (int i = -1; i <= 1; ++i) Ellipse(f, cx + i * 14, cy - 6, 4, 4, true);
            break;
        case 1:  // Story: open book
            Rectangle(f, cx - 38, cy - 24, 38, 48);
            Rectangle(f, cx - 37, cy - 23, 36, 46);
            Rectangle(f, cx, cy - 24, 38, 48);
            Rectangle(f, cx + 1, cy - 23, 36, 46);
            for (int i = 0; i < 3; ++i) {
                Line(f, cx - 30, cy - 12 + i * 12, cx - 8, cy - 12 + i * 12);
                Line(f, cx + 8, cy - 12 + i * 12, cx + 30, cy - 12 + i * 12);
            }
            break;
        case 2:  // Game: die showing five
            Rectangle(f, cx - 28, cy - 28, 56, 56);
            Rectangle(f, cx - 27, cy - 27, 54, 54);
            for (int dx : {-14, 14})
                for (int dy : {-14, 14}) Ellipse(f, cx + dx, cy + dy, 5, 5, true);
            Ellipse(f, cx, cy, 5, 5, true);
            break;
        case 3:  // Feed: carrot with leaves
            ThickLine(f, cx - 16, cy - 16, cx + 6, cy + 30, 3);
            ThickLine(f, cx + 16, cy - 16, cx + 6, cy + 30, 3);
            ThickLine(f, cx - 16, cy - 16, cx + 16, cy - 16, 3);
            Line(f, cx - 6, cy - 16, cx - 14, cy - 34);
            Line(f, cx, cy - 16, cx, cy - 36);
            Line(f, cx + 6, cy - 16, cx + 14, cy - 34);
            break;
        case 4:  // Sleep: crescent moon and a star
            Crescent(f, cx - 6, cy, 28, 14);
            ThickLine(f, cx + 22, cy - 22, cx + 34, cy - 22, 2);
            ThickLine(f, cx + 28, cy - 28, cx + 28, cy - 16, 2);
            break;
        default:
            break;
    }
}

void ActionRail(MonoFrame& f, uint8_t selection) {
    const int top = SceneComposer::kRailTop, slot_w = SceneComposer::kRailSlotWidth;
    ThickLine(f, 0, top, MonoFrame::kWidth - 1, top, 2);
    for (int slot = 0; slot < 5; ++slot) {
        RailIcon(f, slot, slot * slot_w + slot_w / 2, top + 50);
    }
    if (selection >= 5) return;  // nothing highlighted
    // Thick 4 px frame around the selected slot.
    for (int i = 0; i < 4; ++i)
        Rectangle(f, selection * slot_w + 6 + i, top + 6 + i, slot_w - 12 - 2 * i, 88 - 2 * i);
}

// Fixed picture kept on the unpowered ePaper: sleeping face, moon, Zzz.
void SleepScene(MonoFrame& f) {
    PetFace(f, 400, 260, 100, Emotion::Sleepy);
    Crescent(f, 680, 85, 42, 20);
    ThickLine(f, 740, 50, 756, 50, 2);
    ThickLine(f, 748, 42, 748, 58, 2);
    // Three growing "Z" letters rising to the right of the head.
    for (int i = 0; i < 3; ++i) {
        const int size = 16 + i * 10, x = 540 + i * 32, y = 232 - i * 40;
        ThickLine(f, x, y, x + size, y, 3);
        ThickLine(f, x + size, y, x, y + size, 3);
        ThickLine(f, x, y + size, x + size, y + size, 3);
    }
}

void HomeScene(MonoFrame& f, const SceneModel& model) {
    PetFace(f, 400, 215, 90, model.emotion);
    ActionRail(f, model.home_selection);
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
    } else if (model.kind == SceneKind::Sleep) {
        SleepScene(output);
    } else {
        HomeScene(output, model);
    }
}

}  // namespace kid_app
