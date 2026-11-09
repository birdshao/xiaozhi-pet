#include "kid_app/book_shelf.h"

#include <algorithm>
#include <cstring>

namespace kid_app {
namespace {
bool StartsWith(const char* s, const char* prefix) {
    return std::strncmp(s, prefix, std::strlen(prefix)) == 0;
}
// Same rule as StoryPackFilePort::OpenBook (ValidBookId).
bool IdChar(char c, bool first) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (!first && (c == '_' || c == '-'));
}
bool Before(const ShelfBook& a, const ShelfBook& b) {
    if (ShelfBefore(a.entry, b.entry)) return true;
    if (ShelfBefore(b.entry, a.entry)) return false;
    return std::strcmp(a.id, b.id) < 0;
}
}  // namespace

bool ShelfBookId(const char* file_name, char* id) {
    if (!file_name || !id) return false;
    const size_t n = std::strlen(file_name);
    if (n <= 4) return false;
    const char* ext = file_name + n - 4;
    const bool kpb = ext[0] == '.' && (ext[1] == 'k' || ext[1] == 'K') &&
                     (ext[2] == 'p' || ext[2] == 'P') && (ext[3] == 'b' || ext[3] == 'B');
    const size_t len = n - 4;
    if (!kpb || len < 3 || len >= kShelfIdBytes) return false;
    for (size_t i = 0; i < len; ++i)
        if (!IdChar(file_name[i], i == 0)) return false;
    if (StartsWith(file_name, "song_") || StartsWith(file_name, "lullaby_")) return false;
    std::memcpy(id, file_name, len);
    id[len] = '\0';
    return true;
}

bool DownsampleThumb(const uint8_t* mono, uint8_t* out) {
    if (!mono || !out) return false;
    for (int ty = 0; ty < ShelfView::kThumbH; ++ty) {
        for (int tx = 0; tx < ShelfView::kThumbStride; ++tx) {
            uint8_t byte = 0;
            for (int bit = 0; bit < 8; ++bit) {
                const int px = (tx * 8 + bit) * 4;
                int black = 0;
                for (int dy = 0; dy < 4; ++dy)
                    for (int dx = 0; dx < 4; ++dx) {
                        const int x = px + dx, y = ty * 4 + dy;
                        const uint8_t v = mono[y * MonoFrame::kStride + x / 8];
                        if (!(v & (0x80 >> (x % 8)))) ++black;
                    }
                if (black < 4) byte = static_cast<uint8_t>(byte | (0x80 >> bit));
            }
            out[ty * ShelfView::kThumbStride + tx] = byte;
        }
    }
    return true;
}

bool BookShelf::Add(const ShelfBook& book) {
    if (count_ >= kCollectMax || !book.id[0] || Find(book.id) >= 0) return false;
    books_[count_++] = book;
    return true;
}

size_t BookShelf::Truncate(size_t keep) {
    if (keep >= count_) return 0;
    const size_t dropped = count_ - keep;
    for (size_t i = keep; i < count_; ++i) books_[i] = ShelfBook{};
    count_ = keep;
    if (selected_ >= count_) selected_ = 0;
    return dropped;
}

bool BookShelf::SetThumb(size_t i, const uint8_t* thumb) {
    if (i >= count_) return false;
    books_[i].thumb = thumb;
    return true;
}

void BookShelf::Sort() { std::sort(books_.begin(), books_.begin() + count_, Before); }

int BookShelf::Find(const char* id) const {
    if (!id) return -1;
    for (size_t i = 0; i < count_; ++i)
        if (std::strcmp(books_[i].id, id) == 0) return static_cast<int>(i);
    return -1;
}

void BookShelf::Enter(const char* prefer_id) {
    const int at = Find(prefer_id);
    selected_ = at >= 0 ? static_cast<uint8_t>(at) : 0;
}

ShelfStep BookShelf::OnInput(InputKind kind) {
    ShelfStep step{};
    if (count_ == 0) {
        if (kind == InputKind::DialLong) step.home = true;
        return step;
    }
    switch (kind) {
        case InputKind::DialDown:
            selected_ = static_cast<uint8_t>((selected_ + 1) % count_);
            step.publish = true;
            break;
        case InputKind::DialUp:
            selected_ = static_cast<uint8_t>((selected_ + count_ - 1) % count_);
            step.publish = true;
            break;
        case InputKind::DialPress:
            step.pick = true;
            step.index = selected_;
            break;
        case InputKind::DialLong:
            step.home = true;
            break;
        default:
            break;
    }
    return step;
}

ShelfView BookShelf::View(const uint16_t* recent, size_t recent_count, bool bedtime) const {
    ShelfView view{};
    view.count = static_cast<uint8_t>(count_);
    view.selected = selected_;
    view.first = static_cast<uint8_t>(selected_ / ShelfView::kVisible * ShelfView::kVisible);
    view.bedtime = bedtime;
    for (size_t i = 0; i < ShelfView::kVisible; ++i) {
        const size_t at = view.first + i;
        if (at >= count_) break;
        const ShelfBook& book = books_[at];
        ShelfCard& card = view.cards[i];
        card.present = true;
        card.thumb = book.thumb;
        card.series_index = book.entry.series_id ? static_cast<uint8_t>(book.entry.series_index) : 0;
        card.series_count = book.entry.series_id ? book.series_count : 0;
        if (book.entry.content_id != 0 && recent)
            for (size_t r = 0; r < recent_count; ++r)
                if (recent[r] == book.entry.content_id) card.read = true;
    }
    return view;
}

SceneModel ShelfScene(const ShelfView& view) {
    SceneModel scene{};
    scene.kind = SceneKind::Shelf;
    scene.emotion = Emotion::Curious;
    scene.shelf = view;
    scene.book_page = static_cast<uint16_t>(((view.first & 0x7F) << 8) | view.selected);
    return scene;
}

}  // namespace kid_app
