#pragma once

#include "kid_app/book_reader.h"
#include "kid_app/input_hub.h"
#include "kid_app/scene_model.h"
#include "kid_app/shelf_view.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace kid_app {

// V9 bookshelf (docs/V9_MENU_DEEPENING_PROPOSAL.md B1). "Story" first shows
// the books on the card, three to a row: the dial picks, a press opens, a
// long press goes home. Pure logic: the board scans /sdcard/books once (a
// file named like a book, validated by opening it), reads the series info and
// the first-page thumbnail, and feeds the result in here.

// Book ids are what StoryPackFilePort::OpenBook accepts: 3..63 chars of
// [a-z0-9_-] (no _ or - first), so the id buffer is 64 bytes.
constexpr size_t kShelfIdBytes = 64;

// "name.kpb" -> id "name". Songs and lullabies ("song_*", "lullaby_*") are
// sound content, not books; a name OpenBook would refuse is not a book.
bool ShelfBookId(const char* file_name, char* id /* kShelfIdBytes */);

// 4x4 sample of an 800x480 1-bit page (MonoFrame convention) into a
// ShelfView::kThumbBytes thumbnail: a block with 4 or more black pixels of
// 16 is black, so thin line art survives.
bool DownsampleThumb(const uint8_t* mono, uint8_t* out);

struct ShelfBook {
    char id[kShelfIdBytes] = {};
    ShelfEntry entry{};
    uint8_t series_count = 0;
    const uint8_t* thumb = nullptr;  // owned by the board, stable for the run
};

struct ShelfStep {
    bool publish = false;  // the picture changed
    bool pick = false;     // open the book `index`
    bool home = false;
    uint8_t index = 0;
};

class BookShelf {
public:
    // On the shelf (and with a thumbnail slot) at most this many books.
    static constexpr size_t kMaxBooks = 24;
    // A scan collects up to this many first, sorts, then keeps kMaxBooks, so
    // which books are dropped does not depend on the card's directory order.
    static constexpr size_t kCollectMax = 64;

    // False when full (kCollectMax) or when the id is already on the shelf.
    bool Add(const ShelfBook& book);
    // Reading order (ShelfBefore), then file name; makes the order stable.
    void Sort();
    // Keep the first `keep` books (after Sort); returns how many were dropped.
    size_t Truncate(size_t keep);
    bool SetThumb(size_t i, const uint8_t* thumb);
    size_t Count() const { return count_; }
    const ShelfBook& At(size_t i) const { return books_[i]; }
    int Find(const char* id) const;  // -1 = not on the shelf
    // One book (or none) keeps the old behavior: open it at once.
    bool UseShelf() const { return count_ >= 2; }

    // Start a visit; the ring sits on `prefer_id` (the book read last) if it
    // is there, else on the first book.
    void Enter(const char* prefer_id);
    ShelfStep OnInput(InputKind kind);
    uint8_t Selected() const { return selected_; }
    // recent: content ids of lately finished books (0 = newest); a book with
    // content id 0 (KPB1) is never marked.
    ShelfView View(const uint16_t* recent, size_t recent_count, bool bedtime) const;

private:
    std::array<ShelfBook, kCollectMax> books_{};
    size_t count_ = 0;
    uint8_t selected_ = 0;
};

// The Shelf scene: book_page packs the row start (bits 8-14) and selection
// (bits 0-7) so the planner and the packed scene word see every move.
SceneModel ShelfScene(const ShelfView& view);

}  // namespace kid_app
