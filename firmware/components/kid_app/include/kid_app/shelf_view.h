#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace kid_app {

// V9 bookshelf picture data (SceneKind::Shelf). Three covers per row; a
// cover is a 1-bit thumbnail of the book's first page (MonoFrame convention:
// bit 1 = white, MSB first, kThumbStride bytes per row) sampled once at boot
// by the board, which owns the bytes for the whole run. Not packed into the
// scene word (like the game and care views), only the selection is.
struct ShelfCard {
    bool present = false;
    bool read = false;               // finished lately (recent_books)
    uint8_t series_index = 0;        // 1-based, 0 = standalone
    uint8_t series_count = 0;        // books in the series, 0/1 = no dots
    const uint8_t* thumb = nullptr;  // kThumbBytes, or none (a plain cover)

    bool operator==(const ShelfCard& o) const {
        return present == o.present && read == o.read && series_index == o.series_index &&
               series_count == o.series_count && thumb == o.thumb;
    }
    bool operator!=(const ShelfCard& o) const { return !(*this == o); }
};

struct ShelfView {
    static constexpr size_t kVisible = 3;
    // 800x480 page sampled 4x4 -> 200x120.
    static constexpr int kThumbW = 200;
    static constexpr int kThumbH = 120;
    static constexpr int kThumbStride = kThumbW / 8;
    static constexpr size_t kThumbBytes = static_cast<size_t>(kThumbStride) * kThumbH;

    uint8_t count = 0;     // books on the shelf
    uint8_t first = 0;     // index of the left cover
    uint8_t selected = 0;  // index with the ring
    bool bedtime = false;  // evening: the moon on top, reading will be bedtime mode
    std::array<ShelfCard, kVisible> cards{};

    bool operator==(const ShelfView& o) const {
        return count == o.count && first == o.first && selected == o.selected &&
               bedtime == o.bedtime && cards == o.cards;
    }
    bool operator!=(const ShelfView& o) const { return !(*this == o); }
};

}  // namespace kid_app
