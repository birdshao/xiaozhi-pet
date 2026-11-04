#include "kid_app/icon_choice.h"

#include "kid_app/scene_model.h"

#include <algorithm>

namespace kid_app {

void IconChoice::Reset(uint8_t count, uint8_t start) {
    count_ = std::min<uint8_t>(count, kMaxOptions);
    index_ = count_ && start < count_ ? start : 0;
}

bool IconChoice::Move(int delta) {
    if (!count_ || !delta) return false;
    int next = static_cast<int>(index_) + (delta < 0 ? -1 : 1);
    next = std::max(0, std::min(next, static_cast<int>(count_) - 1));
    if (next == index_) return false;
    index_ = static_cast<uint8_t>(next);
    return true;
}

void DrawChoiceRing(MonoFrame& frame, const PageRect& ring, uint8_t margin,
                    ChoiceRing style) {
    if (ring.w <= 2 * margin || ring.h <= 2 * margin) return;
    const int x0 = ring.x, y0 = ring.y;
    const int x1 = ring.x + ring.w - 1, y1 = ring.y + ring.h - 1;
    // Distance d from the outer edge: black where d < black_px.
    const int black_px = style == ChoiceRing::Chosen      ? margin
                         : style == ChoiceRing::Highlight ? std::min<int>(kChoiceRingPx, margin)
                                                          : 0;
    for (int y = y0; y <= y1; ++y) {
        const int dy = std::min(y - y0, y1 - y);
        if (dy < margin) {
            // Top/bottom band: whole row, colour by row depth.
            frame.PaintSpan(x0, x1, y, 0xFF, dy < black_px);
            if (dy >= black_px)  // keep the side rings on white rows
                for (int d = 0; d < black_px; ++d) {
                    frame.SetPixel(x0 + d, y, true);
                    frame.SetPixel(x1 - d, y, true);
                }
            continue;
        }
        for (int d = 0; d < margin; ++d) {
            frame.SetPixel(x0 + d, y, d < black_px);
            frame.SetPixel(x1 - d, y, d < black_px);
        }
    }
}

void OpposeRect(MonoFrame& previous, const MonoFrame& next, const PageRect& rect) {
    const int x1 = std::min<int>(rect.x + rect.w, MonoFrame::kWidth);
    const int y1 = std::min<int>(rect.y + rect.h, MonoFrame::kHeight);
    for (int y = rect.y; y < y1; ++y)
        for (int x = rect.x; x < x1; ++x) previous.SetPixel(x, y, !next.IsBlack(x, y));
}

}  // namespace kid_app
