// V9 bookshelf picture (SceneKind::Shelf). Included by scene_model.cc inside
// its anonymous namespace (it uses the file's drawing primitives); not a
// translation unit of its own. No text: three covers (first-page thumbnails)
// in a row, a thick ring on the selected one, a check badge on books read
// lately, dots for the place in a series, row dots below, and the moon on top
// in the evening (reading then starts the bedtime story). Nothing is drawn in
// the status-band corner boxes or the bottom corners.

constexpr int kShelfCardW = 232, kShelfCardH = 200, kShelfCardGap = 24;
constexpr int kShelfCardX0 = (kW - (3 * kShelfCardW + 2 * kShelfCardGap)) / 2;
constexpr int kShelfCardY = 150;

// A plain cover for a book without a thumbnail: closed book with a spine.
void ShelfPlainCover(MonoFrame& f, int cx, int cy) {
    RoundRectRing(f, cx - 70, cy - 50, 140, 100, 8, 5);
    FillRect(f, cx - 70, cy - 50, 16, 100);
    Stroke(f, cx - 30, cy - 12, cx + 40, cy - 12, 4);
    Stroke(f, cx - 30, cy + 14, cx + 20, cy + 14, 4);
}

void ShelfCover(MonoFrame& f, const ShelfCard& card, int x, bool selected) {
    const int y = kShelfCardY;
    RoundRectRing(f, x, y, kShelfCardW, kShelfCardH, 14, 2);
    if (selected) RoundRectRing(f, x - 10, y - 10, kShelfCardW + 20, kShelfCardH + 20, 20, 6);
    const int tx = x + (kShelfCardW - ShelfView::kThumbW) / 2, ty = y + 22;
    if (card.thumb) {
        for (int row = 0; row < ShelfView::kThumbH; ++row)
            for (int col = 0; col < ShelfView::kThumbW; ++col)
                if (!(card.thumb[row * ShelfView::kThumbStride + col / 8] & (0x80 >> (col % 8))))
                    f.SetPixel(tx + col, ty + row, true);
        RoundRectRing(f, tx - 3, ty - 3, ShelfView::kThumbW + 6, ShelfView::kThumbH + 6, 4, 3);
    } else {
        ShelfPlainCover(f, x + kShelfCardW / 2, ty + ShelfView::kThumbH / 2);
    }
    if (card.series_count > 1) {  // where this book sits in its series
        const int n = std::min<int>(card.series_count, 8);
        const int step = 24;
        const int x0 = x + kShelfCardW / 2 - (n - 1) * step / 2;
        for (int i = 0; i < n; ++i) {
            const bool here = i + 1 == card.series_index;
            Disk(f, x0 + i * step, y + 168, 8);
            if (!here) Disk(f, x0 + i * step, y + 168, 5, false);
        }
    }
    if (card.read) {  // finished lately
        Disk(f, x + kShelfCardW - 26, y + 26, 22);
        CheckMark(f, x + kShelfCardW - 26, y + 26, 22, 5, false);
    }
}

void ShelfPicture(MonoFrame& f, const ShelfView& view) {
    if (view.bedtime) Moon(f, 400, 84, 32, true);
    for (size_t i = 0; i < ShelfView::kVisible; ++i) {
        if (!view.cards[i].present) continue;
        const int x = kShelfCardX0 + static_cast<int>(i) * (kShelfCardW + kShelfCardGap);
        ShelfCover(f, view.cards[i], x, view.first + static_cast<int>(i) == view.selected);
    }
    const int pages = (view.count + 2) / 3;
    if (pages > 1) {  // which row of the shelf
        const int step = 28;
        const int x0 = kW / 2 - (pages - 1) * step / 2;
        const int current = view.first / 3;
        for (int p = 0; p < pages && p < 8; ++p) {
            Disk(f, x0 + p * step, 420, 8);
            if (p != current) Disk(f, x0 + p * step, 420, 5, false);
        }
    }
}
