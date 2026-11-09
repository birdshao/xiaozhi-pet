// V9 1B bookshelf: file filter, order, paging, dial, view, picture, refresh.
#include "kid_app/book_shelf.h"
#include "kid_app/refresh_planner.h"
#include "kid_app/scene_model.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace kid_app;

namespace {

ShelfBook Book(const char* id, uint16_t content, uint16_t series = 0, uint16_t index = 0,
               uint8_t series_count = 0, const uint8_t* thumb = nullptr) {
    ShelfBook b{};
    std::snprintf(b.id, sizeof(b.id), "%s", id);
    b.entry.content_id = content;
    b.entry.series_id = series;
    b.entry.series_index = index;
    b.series_count = series_count;
    b.thumb = thumb;
    return b;
}

BookShelf Shelf(int n) {
    BookShelf s;
    for (int i = 0; i < n; ++i) {
        char id[16];
        std::snprintf(id, sizeof(id), "c%d", 100 + i);
        const bool added = s.Add(Book(id, static_cast<uint16_t>(100 + i)));
        assert(added);
    }
    s.Sort();
    return s;
}

int Ink(const SceneModel& m) {
    MonoFrame f;
    SceneComposer::Render(m, f);
    int n = 0;
    for (int y = 0; y < MonoFrame::kHeight; ++y)
        for (int x = 0; x < MonoFrame::kWidth; ++x) n += f.IsBlack(x, y) ? 1 : 0;
    return n;
}

int Diff(const SceneModel& a, const SceneModel& b) {
    MonoFrame fa, fb;
    SceneComposer::Render(a, fa);
    SceneComposer::Render(b, fb);
    int n = 0;
    for (int y = 0; y < MonoFrame::kHeight; ++y)
        for (int x = 0; x < MonoFrame::kWidth; ++x) n += fa.IsBlack(x, y) != fb.IsBlack(x, y);
    return n;
}

void FileFilter() {
    char id[kShelfIdBytes];
    bool ok = ShelfBookId("forest_friends.kpb", id);
    assert(ok && std::strcmp(id, "forest_friends") == 0);
    ok = ShelfBookId("c1203.kpb", id);
    assert(ok && std::strcmp(id, "c1203") == 0);
    ok = ShelfBookId("tuan-1.KPB", id);  // extension case does not matter
    assert(ok && std::strcmp(id, "tuan-1") == 0);
    // What OpenBook would refuse is not a book: upper case, short, leading _.
    assert(!ShelfBookId("TUAN-1.kpb", id));
    assert(!ShelfBookId("ab.kpb", id));
    assert(!ShelfBookId("_abc.kpb", id));
    assert(!ShelfBookId("-abc.kpb", id));
    // Songs and lullabies are sound content, never books.
    assert(!ShelfBookId("song_twinkle.kpb", id));
    assert(!ShelfBookId("lullaby_moon.kpb", id));
    assert(!ShelfBookId("notes.txt", id));
    assert(!ShelfBookId("book.kpb.bak", id));
    assert(!ShelfBookId(".kpb", id));
    assert(!ShelfBookId("a b.kpb", id));
    assert(!ShelfBookId("../x.kpb", id));
    // The pack tools allow long names (up to 63 chars, like OpenBook).
    const std::string long63(63, 'a');
    const std::string long64(64, 'a');
    ok = ShelfBookId((long63 + ".kpb").c_str(), id);
    assert(ok && std::strlen(id) == 63);
    assert(!ShelfBookId((long64 + ".kpb").c_str(), id));
    ok = ShelfBookId("a_book_with_a_name_longer_than_fifteen.kpb", id);
    assert(ok);
    assert(!ShelfBookId(nullptr, id));
    assert(!ShelfBookId("a.kpb", nullptr));
}

void OrderAndCap() {
    BookShelf s;
    // Series 7 (books 30, 31), standalone 5, standalone 40, series 7 stays together.
    const bool a1 = s.Add(Book("b40", 40)), a2 = s.Add(Book("s2", 31, 7, 2, 2)),
               a3 = s.Add(Book("b5", 5)), a4 = s.Add(Book("s1", 30, 7, 1, 2));
    assert(a1 && a2 && a3 && a4);
    const bool twice = s.Add(Book("b5", 5));  // same file twice
    assert(!twice);
    s.Sort();
    assert(s.Count() == 4);
    const char* want[] = {"b5", "s1", "s2", "b40"};
    for (size_t i = 0; i < 4; ++i) assert(std::strcmp(s.At(i).id, want[i]) == 0);
    assert(s.Find("s2") == 2 && s.Find("none") == -1 && s.Find(nullptr) == -1);

    // More books than fit: collect, sort, then keep the first 24 in reading
    // order, whatever order the card listed them in.
    BookShelf full;
    for (size_t i = 0; i < BookShelf::kCollectMax; ++i) {
        char id[16];
        std::snprintf(id, sizeof(id), "x%zu", BookShelf::kCollectMax - i);  // reverse
        const bool added = full.Add(Book(id, static_cast<uint16_t>(BookShelf::kCollectMax - i)));
        assert(added);
    }
    const bool over = full.Add(Book("extra", 999));
    assert(!over);
    full.Sort();
    const size_t dropped = full.Truncate(BookShelf::kMaxBooks);
    assert(dropped == BookShelf::kCollectMax - BookShelf::kMaxBooks);
    assert(full.Count() == BookShelf::kMaxBooks);
    assert(full.At(0).entry.content_id == 1 && full.At(23).entry.content_id == 24);
    const size_t none_dropped = full.Truncate(100);
    assert(none_dropped == 0 && full.Count() == BookShelf::kMaxBooks);
    // A thumbnail slot is per kept book.
    uint8_t dot = 0;
    const bool set = full.SetThumb(23, &dot), set_out = full.SetThumb(24, &dot);
    assert(set && !set_out && full.At(23).thumb == &dot);

    // KPB1 books have no content id: order by file name, deterministic.
    BookShelf legacy;
    const bool z = legacy.Add(Book("zeta", 0)), al = legacy.Add(Book("alpha", 0));
    assert(z && al);
    legacy.Sort();
    assert(std::strcmp(legacy.At(0).id, "alpha") == 0);
}

void DirectOpen() {
    assert(!Shelf(0).UseShelf());
    assert(!Shelf(1).UseShelf());  // old behavior: one book opens at once
    assert(Shelf(2).UseShelf());
}

void Dial() {
    BookShelf s = Shelf(5);
    s.Enter(nullptr);
    assert(s.Selected() == 0);
    ShelfStep step = s.OnInput(InputKind::DialDown);
    assert(step.publish && s.Selected() == 1 && !step.pick && !step.home);
    s.OnInput(InputKind::DialDown);
    s.OnInput(InputKind::DialDown);
    s.OnInput(InputKind::DialDown);
    assert(s.Selected() == 4);
    s.OnInput(InputKind::DialDown);  // wraps to the first book
    assert(s.Selected() == 0);
    s.OnInput(InputKind::DialUp);  // and back to the last
    assert(s.Selected() == 4);
    step = s.OnInput(InputKind::DialPress);
    assert(step.pick && step.index == 4 && !step.home);
    step = s.OnInput(InputKind::DialLong);
    assert(step.home && !step.pick);
    step = s.OnInput(InputKind::Talk);  // anything else does nothing
    assert(!step.publish && !step.pick && !step.home);
    // Enter remembers the book that was read last.
    s.Enter("c102");
    assert(s.Selected() == 2);
    s.Enter("gone");
    assert(s.Selected() == 0);
    // An empty shelf never picks.
    BookShelf none;
    step = none.OnInput(InputKind::DialPress);
    assert(!step.pick);
}

void View() {
    BookShelf s = Shelf(7);
    s.Enter(nullptr);
    ShelfView v = s.View(nullptr, 0, false);
    assert(v.count == 7 && v.first == 0 && v.selected == 0 && !v.bedtime);
    assert(v.cards[0].present && v.cards[1].present && v.cards[2].present);
    for (int i = 0; i < 3; ++i) s.OnInput(InputKind::DialDown);
    v = s.View(nullptr, 0, false);
    assert(v.first == 3 && v.selected == 3);  // the next row of three
    for (int i = 0; i < 3; ++i) s.OnInput(InputKind::DialDown);
    v = s.View(nullptr, 0, true);
    assert(v.first == 6 && v.selected == 6 && v.bedtime);
    assert(v.cards[0].present && !v.cards[1].present && !v.cards[2].present);

    // Read marks come from the recent finished books (content ids).
    s.Enter(nullptr);
    const uint16_t recent[] = {101, 777, 105};
    v = s.View(recent, 3, false);
    assert(!v.cards[0].read && v.cards[1].read && !v.cards[2].read);
    // KPB1 books (content id 0) are never "read".
    BookShelf legacy;
    legacy.Add(Book("a", 0));
    legacy.Add(Book("b", 0));
    legacy.Sort();
    const uint16_t zero[] = {0, 0};
    v = legacy.View(zero, 2, false);
    assert(!v.cards[0].read && !v.cards[1].read);
    // Series info reaches the card.
    BookShelf ser;
    ser.Add(Book("t1", 30, 7, 1, 3));
    ser.Add(Book("t2", 31, 7, 2, 3));
    ser.Sort();
    v = ser.View(nullptr, 0, false);
    assert(v.cards[1].series_index == 2 && v.cards[1].series_count == 3);
}

void Thumb() {
    std::vector<uint8_t> white(MonoFrame::kSize, 0xFF), black(MonoFrame::kSize, 0x00);
    std::vector<uint8_t> out(ShelfView::kThumbBytes, 0x55);
    DownsampleThumb(white.data(), out.data());
    for (uint8_t b : out) assert(b == 0xFF);
    DownsampleThumb(black.data(), out.data());
    for (uint8_t b : out) assert(b == 0x00);
    // A thin line (one pixel row in every 4x4 block = 4 black of 16) stays ink.
    std::vector<uint8_t> lines(MonoFrame::kSize, 0xFF);
    for (int y = 0; y < MonoFrame::kHeight; y += 4)
        for (int x = 0; x < MonoFrame::kStride; ++x) lines[y * MonoFrame::kStride + x] = 0x00;
    DownsampleThumb(lines.data(), out.data());
    for (uint8_t b : out) assert(b == 0x00);
    // 3 of 16 black stays paper.
    std::vector<uint8_t> faint(MonoFrame::kSize, 0xFF);
    for (int y = 0; y < MonoFrame::kHeight; y += 4)
        for (int x = 0; x < MonoFrame::kWidth; x += 4) {
            for (int k = 0; k < 3; ++k) {
                const int px = x + k;
                faint[y * MonoFrame::kStride + px / 8] &= static_cast<uint8_t>(~(0x80 >> (px % 8)));
            }
        }
    DownsampleThumb(faint.data(), out.data());
    for (uint8_t b : out) assert(b == 0xFF);
    assert(DownsampleThumb(nullptr, out.data()) == false);
}

void Picture() {
    std::vector<uint8_t> dark(ShelfView::kThumbBytes, 0x00), light(ShelfView::kThumbBytes, 0xFF);
    BookShelf s;
    for (int i = 0; i < 4; ++i) {
        char id[16];
        std::snprintf(id, sizeof(id), "c%d", 100 + i);
        s.Add(Book(id, static_cast<uint16_t>(100 + i), 0, 0, 0, i == 0 ? dark.data() : light.data()));
    }
    s.Sort();
    s.Enter(nullptr);
    const SceneModel a = ShelfScene(s.View(nullptr, 0, false));
    assert(a.kind == SceneKind::Shelf);
    assert(Ink(a) > 2000);

    // The status-band boxes and the bottom corners stay empty.
    MonoFrame f;
    SceneComposer::Render(a, f);
    for (int y = 0; y < MonoFrame::kHeight; ++y)
        for (int x = 0; x < MonoFrame::kWidth; ++x)
            if (SceneComposer::InStatusBox(x, y)) assert(!f.IsBlack(x, y));
    for (int y = MonoFrame::kHeight - 12; y < MonoFrame::kHeight; ++y)
        for (int x : {0, 1, 2, 3, MonoFrame::kWidth - 4, MonoFrame::kWidth - 1})
            assert(!f.IsBlack(x, y));

    // The thumbnail is drawn: the dark cover covers more than the light ones.
    BookShelf s2 = s;
    s2.OnInput(InputKind::DialDown);
    const SceneModel b = ShelfScene(s2.View(nullptr, 0, false));
    assert(Diff(a, b) > 200);  // selection ring moved
    // Read tick, bedtime moon and series dots each change the picture.
    const uint16_t recent[] = {100};
    assert(Diff(a, ShelfScene(s.View(recent, 1, false))) > 30);
    assert(Diff(a, ShelfScene(s.View(nullptr, 0, true))) > 100);
    BookShelf ser;
    ser.Add(Book("t1", 30, 7, 1, 3, light.data()));
    ser.Add(Book("t2", 31, 7, 2, 3, light.data()));
    ser.Sort();
    BookShelf plain;
    plain.Add(Book("t1", 30, 0, 0, 0, light.data()));
    plain.Add(Book("t2", 31, 0, 0, 0, light.data()));
    plain.Sort();
    assert(Diff(ShelfScene(ser.View(nullptr, 0, false)),
                ShelfScene(plain.View(nullptr, 0, false))) > 30);
    // A missing thumbnail still draws a cover (a plain book).
    BookShelf nothumb;
    nothumb.Add(Book("t1", 30));
    nothumb.Add(Book("t2", 31));
    nothumb.Sort();
    assert(Ink(ShelfScene(nothumb.View(nullptr, 0, false))) > 1500);
    // An empty view does not crash.
    assert(Ink(ShelfScene(ShelfView{})) >= 0);
    // Packed scene word keeps the kind and the selection.
    const SceneModel un = UnpackScene(PackScene(b));
    assert(un.kind == SceneKind::Shelf && un.book_page == b.book_page);
    assert(PackScene(a) != PackScene(b));
}

void Refresh() {
    BookShelf s = Shelf(7);
    s.Enter(nullptr);
    SceneModel home{};
    RefreshPlanner planner;
    const SceneModel a = ShelfScene(s.View(nullptr, 0, false));
    assert(planner.Plan(a) == RefreshMode::Full);  // first picture
    planner.Completed(home, RefreshMode::Full, true);
    assert(planner.Plan(a) == RefreshMode::Full);  // entering from Home
    planner.Completed(a, RefreshMode::Full, true);
    s.OnInput(InputKind::DialDown);
    const SceneModel b = ShelfScene(s.View(nullptr, 0, false));
    assert(planner.Plan(b) == RefreshMode::Partial);  // the ring moves
    planner.Completed(b, RefreshMode::Partial, true);
    s.OnInput(InputKind::DialDown);  // third card -> still the first row
    const SceneModel c = ShelfScene(s.View(nullptr, 0, false));
    assert(planner.Plan(c) == RefreshMode::Partial);
    planner.Completed(c, RefreshMode::Partial, true);
    s.OnInput(InputKind::DialDown);  // next row: a clean full refresh
    const SceneModel d = ShelfScene(s.View(nullptr, 0, false));
    assert(planner.Plan(d) == RefreshMode::Full);
    // Ghosting budget: partial moves eventually ask for a full refresh.
    planner.Completed(d, RefreshMode::Full, true);
    BookShelf t = Shelf(3);
    t.Enter(nullptr);
    bool full_seen = false;
    for (int i = 0; i < 80 && !full_seen; ++i) {
        t.OnInput(InputKind::DialDown);
        const SceneModel m = ShelfScene(t.View(nullptr, 0, false));
        const RefreshMode mode = planner.Plan(m);
        full_seen = mode == RefreshMode::Full;
        planner.Completed(m, mode, true);
    }
    assert(full_seen);
}

}  // namespace

int main() {
    FileFilter();
    OrderAndCap();
    DirectOpen();
    Dial();
    View();
    Thumb();
    Picture();
    Refresh();
    std::puts("book shelf ok");
    return 0;
}
