// V8 branching picture books and bedtime story mode, on the KPB2 v3 test book
// tools/make_branch_test_book.py makes (pages: 1 -> 2 [left 3 / right 4],
// 3 -> 5, 4 -> 6, 5 = ending A, 6 = ending B; series 7, book 1 of 2).
// Usage: test_branch_book <branch-test/books> <fixtures/kpb2>
#include "kid_app/book_reader.h"
#include "kid_app/content_pack_reader.h"
#include "kid_app/memory_store.h"
#include "kid_app/scene_model.h"
#include "kid_app/story_pack_file_port.h"
#include "kid_app/story_pack_reader.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace kid_app;

namespace {

int g_checks = 0;
#define CHECK(cond)                                                           \
    do {                                                                      \
        ++g_checks;                                                           \
        if (!(cond)) {                                                        \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,       \
                         __LINE__, #cond);                                    \
            std::exit(1);                                                     \
        }                                                                     \
    } while (0)

class MemoryFile final : public StoryPackFile {
public:
    explicit MemoryFile(std::vector<uint8_t> b) : bytes(std::move(b)) {}
    uint64_t Size() const override { return bytes.size(); }
    bool ReadAt(uint32_t offset, void* out, size_t size) override {
        if (uint64_t(offset) + size > bytes.size()) return false;
        std::copy(bytes.begin() + offset, bytes.begin() + offset + size,
                  static_cast<uint8_t*>(out));
        return true;
    }
    std::vector<uint8_t> bytes;
};

uint32_t Crc(const uint8_t* d, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        crc ^= d[i];
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
    }
    return ~crc;
}
void Put16(std::vector<uint8_t>& b, size_t at, uint16_t v) {
    b[at] = static_cast<uint8_t>(v);
    b[at + 1] = static_cast<uint8_t>(v >> 8);
}
void Put32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + i] = static_cast<uint8_t>(v >> (8 * i));
}
void Reseal(std::vector<uint8_t>& b) {
    const uint32_t data = b[40] | (b[41] << 8) | (b[42] << 16) | (uint32_t(b[43]) << 24);
    Put32(b, 60, Crc(b.data() + 96, data - 96));
    Put32(b, 92, Crc(b.data(), 92));
}

// Record offsets in this book: 6 page records, then the 3 question records.
constexpr size_t kPageAt = 96;
constexpr size_t kQuestionAt = 96 + 6 * 96;
size_t NextAt(int page) { return kPageAt + (page - 1) * 96 + 64; }
size_t GotoAt(int question, int option) { return kQuestionAt + question * 192 + 32 + option * 48 + 3; }

bool Opens(std::vector<uint8_t> bytes) {
    MemoryFile file(std::move(bytes));
    ContentPackReader reader;
    return reader.Open(file);
}

struct Loaded {
    MemoryFile file;
    ContentPackReader reader;
    BookOutline outline;
    explicit Loaded(std::vector<uint8_t> b) : file(std::move(b)) {
        CHECK(reader.Open(file));
        CHECK(BuildBookOutline(reader, &outline));
    }
};

BookView View(uint16_t page, uint8_t question = 0, uint8_t highlight = 0) {
    BookView v{};
    v.page = page;
    v.question = question;
    v.highlight = highlight;
    return v;
}

// Plays the reader with a clock; Shown() is the panel confirming the view.
struct Walk {
    BookReader r;
    uint32_t t = 0;
    BookStep last{};
    void Open(const BookOutline* o, bool bedtime = false) {
        r.Open(o, t, BookMissing::None, bedtime);
        Shown();
    }
    BookStep Shown() {
        last = r.OnShown(r.View(), t);
        return last;
    }
    BookStep In(InputKind k) {
        last = r.OnInput(k, t);
        return last;
    }
    BookStep Wait(uint32_t ms) {
        t += ms;
        last = r.Tick(t);
        return last;
    }
};

void TestFormat(const std::vector<uint8_t>& good) {
    Loaded book(good);
    const ContentPackReader& c = book.reader;
    CHECK(c.Version() == 3 && c.Branching() && c.ContentId() == 901 && c.PageCount() == 6);
    CHECK(c.SeriesId() == 7 && c.SeriesIndex() == 1 && c.SeriesCount() == 2 &&
          c.SeriesNext() == 902);
    const BookOutline& o = book.outline;
    CHECK(o.branching && o.series_next == 902 && o.pages == 6);
    CHECK(o.page[1].branch_q == 0 && o.page[1].questions[0].branch);
    CHECK(o.page[1].questions[0].goto_page[0] == 3 && o.page[1].questions[0].goto_page[1] == 4);
    CHECK(o.page[0].next == 0 && o.page[2].next == 5 && o.page[3].next == 6 &&
          o.page[4].next == ContentPackReader::kNextEnd);
    CHECK(o.page[2].branch_q == 0xFF && !o.page[2].questions[0].branch);
}

void TestRejects(const std::vector<uint8_t>& good) {
    struct Case {
        const char* name;
        void (*patch)(std::vector<uint8_t>&);
    };
    const Case bad[] = {
        {"jump to a page that does not exist", [](std::vector<uint8_t>& b) { b[GotoAt(0, 0)] = 9; }},
        {"jump to itself", [](std::vector<uint8_t>& b) { b[GotoAt(0, 0)] = 2; }},
        {"jump byte 0x80", [](std::vector<uint8_t>& b) { b[GotoAt(0, 0)] = 0x80; }},
        {"only one option jumps", [](std::vector<uint8_t>& b) { b[GotoAt(0, 1)] = 0; }},
        {"unreachable page", [](std::vector<uint8_t>& b) { b[GotoAt(0, 1)] = 3; }},
        {"loop 4 -> 2", [](std::vector<uint8_t>& b) { Put16(b, NextAt(4), 2); }},
        {"loop 5 -> 1 without restart", [](std::vector<uint8_t>& b) { Put16(b, NextAt(5), 1); }},
        {"next page missing", [](std::vector<uint8_t>& b) { Put16(b, NextAt(1), 40); }},
        {"next to itself", [](std::vector<uint8_t>& b) { Put16(b, NextAt(4), 4); }},
        {"last page only restarts", [](std::vector<uint8_t>& b) { Put16(b, NextAt(6), 0x8001); }},
        {"restart from page 1", [](std::vector<uint8_t>& b) { Put16(b, NextAt(1), 0x8001); }},
        {"branch page with a next", [](std::vector<uint8_t>& b) { Put16(b, NextAt(2), 3); }},
        {"jump on the like question", [](std::vector<uint8_t>& b) { b[GotoAt(1, 0)] = 5; }},
        {"series next is itself", [](std::vector<uint8_t>& b) { Put16(b, 70, 901); }},
        {"series index past count", [](std::vector<uint8_t>& b) { Put16(b, 66, 3); }},
        {"series last book with a next", [](std::vector<uint8_t>& b) { Put16(b, 66, 2); }},
        {"series bytes after the fields", [](std::vector<uint8_t>& b) { b[72] = 1; }},
        {"unknown header flag", [](std::vector<uint8_t>& b) { Put16(b, 10, 3); }},
        {"v2 with links", [](std::vector<uint8_t>& b) { Put16(b, 4, 2); }},
        {"first option restarts", [](std::vector<uint8_t>& b) {
             b[GotoAt(0, 0)] = 0x81; b[GotoAt(0, 1)] = 3; Put16(b, NextAt(3), 0);
             Put16(b, NextAt(4), 5); Put16(b, NextAt(5), 6); }},
        {"flag without links", [](std::vector<uint8_t>& b) {
             Put16(b, NextAt(3), 0); Put16(b, NextAt(4), 0); Put16(b, NextAt(5), 0);
             b[GotoAt(0, 0)] = 3; b[GotoAt(0, 1)] = 4; Put16(b, 10, 0); }},
        {"page reserved after next", [](std::vector<uint8_t>& b) { b[NextAt(1) + 2] = 1; }},
    };
    for (const Case& c : bad) {
        std::vector<uint8_t> b = good;
        c.patch(b);
        Reseal(b);
        if (Opens(b)) {
            std::fprintf(stderr, "accepted bad branch book: %s\n", c.name);
            std::exit(1);
        }
        ++g_checks;
    }
    // Damage without resealing is a CRC failure like any other.
    std::vector<uint8_t> b = good;
    b[GotoAt(0, 0)] = 4;
    CHECK(!Opens(b));
    // A restart ("read it again") is the one allowed way back: page 2's right
    // option restarts, the rest is chained 3 -> 4 -> 5 -> 6 so all is reachable.
    b = good;
    b[GotoAt(0, 1)] = 0x81;
    Put16(b, NextAt(3), 0);
    Put16(b, NextAt(4), 5);
    Put16(b, NextAt(5), 6);
    Reseal(b);
    CHECK(Opens(b));
    // The same book as KPB2 version 2 with no links reads like V4 did.
}

void TestSceneFlags() {
    BookView v = View(5);
    v.bedtime = true;
    v.next_hint = true;
    const SceneModel end = BookScene(v, true);
    CHECK(end.emotion == Emotion::Sleepy);
    const BookView back = BookViewOf(end);
    CHECK(back == v && back.bedtime && back.next_hint);
    BookView q = View(2, 1, 1);
    q.bedtime = true;
    const BookView q_back = BookViewOf(BookScene(q, false));
    CHECK(q_back == q && q_back.bedtime && !q_back.next_hint);
    // Plain view: same word as before the flags existed.
    CHECK(BookScene(View(3), false).book_page == 3);
    CHECK(BookScene(View(2, 1, 1), false).book_page == ((1 + 1) << 8 | 2));
    CHECK(BookScene(View(7), true).emotion == Emotion::Happy);
    // Flags never change the identity the panel acknowledges.
    BookView plain = View(3);
    BookView dim = plain;
    dim.bedtime = true;
    CHECK(plain == dim);
}

void TestShelf() {
    // Python's shelf_key gives: 5 (alone), series 7: 901, 902, then 20 (alone).
    ShelfEntry book[] = {{20, 0, 0}, {902, 7, 2}, {5, 0, 0}, {901, 7, 1}, {7, 0, 0}};
    std::vector<ShelfEntry> v(std::begin(book), std::end(book));
    std::sort(v.begin(), v.end(), ShelfBefore);
    const uint16_t order[] = {5, 7, 901, 902, 20};  // standalone 7 sorts before series 7
    for (size_t i = 0; i < v.size(); ++i) CHECK(v[i].content_id == order[i]);
}

void TestBedtimeTone() {
    uint8_t grey[2] = {0x1B, 0xE4};  // 0,1,2,3 and 3,2,1,0
    BedtimeTone(grey, 2);
    CHECK(grey[0] == 0x5A && grey[1] == 0xA5);  // 1,1,2,2 and 2,2,1,1
    BedtimeTone(nullptr, 0);
    CHECK(BedtimeAvailable(DayPhase::Evening) && !BedtimeAvailable(DayPhase::Day) &&
          !BedtimeAvailable(DayPhase::Morning) && !BedtimeAvailable(DayPhase::Night));
}

// Page numbers the reader visits going "down" with the given left/right
// choices; checks every step of the history-based paging.
void TestBranchWalk(const BookOutline& plain) {
    BookOutline o = plain;
    o.next_book_present = true;  // the board found /books/c902.kpb on the card
    Walk w;
    w.Open(&o);
    CHECK(w.r.View().page == 1 && !w.r.Bedtime());
    CHECK(w.last.clip.kind == BookClipKind::Page && w.last.clip.page == 1);
    CHECK(w.In(InputKind::DialUp).publish == false);  // nothing before page 1
    CHECK(w.In(InputKind::DialDown).publish && w.r.View() == View(2));
    w.Shown();
    // Down on a branch page asks the question at once (it cannot go on without).
    CHECK(w.In(InputKind::DialDown).publish && w.r.View() == View(2, 1, 0) && w.r.Asking());
    CHECK(w.Shown().clip.kind == BookClipKind::Prompt);
    CHECK(w.In(InputKind::DialDown).publish && w.r.View() == View(2, 1, 1));  // right
    BookStep pick = w.In(InputKind::DialPress);
    CHECK(pick.answer == BookAnswer::Choice && pick.chosen == 1 &&
          pick.memory.kind == BookMemoryKind::None);
    CHECK(w.In(InputKind::DialDown).publish == false);  // reaction: wait
    CHECK(!w.Wait(2900).publish);
    CHECK(w.Wait(200).publish && w.r.View() == View(4));  // the right-hand road
    w.Shown();
    // "Previous" is the page we came from, not page - 1.
    CHECK(w.In(InputKind::DialUp).publish && w.r.View() == View(2));
    CHECK(w.In(InputKind::DialUp).publish && w.r.View() == View(1));
    CHECK(w.In(InputKind::DialDown).publish && w.r.View() == View(2));
    w.In(InputKind::DialDown);  // ask again
    w.In(InputKind::DialPress);  // left (index 0)
    CHECK(w.Wait(3100).publish && w.r.View() == View(3));  // the left-hand road
    w.Shown();
    // Page 3 asks its like question after the narration (1000 ms + 1500 ms).
    CHECK(!w.Wait(2000).publish);
    CHECK(w.Wait(600).publish && w.r.View() == View(3, 1, 0));
    w.Shown();
    pick = w.In(InputKind::DialPress);  // red
    CHECK(pick.memory.kind == BookMemoryKind::Like);
    MemoryEvent e{};
    CHECK(BookMemoryEvent(o, pick.memory, 1, 0, &e) && e.source == MemorySource::DialChoice);
    CHECK(w.Wait(3100).publish && w.r.View() == View(3) && !w.r.Asking());
    // Page 3 links to the ending page 5, not to page 4.
    CHECK(w.In(InputKind::DialDown).publish && w.r.View() == View(5));
    BookStep end = w.In(InputKind::DialDown);
    CHECK(w.r.AtEnd() && end.memory.kind == BookMemoryKind::Finished);
    CHECK(BookMemoryEvent(o, end.memory, 2, 0, &e) && e.kind == MemoryEventKind::BookFinished &&
          e.option == 901 && e.source == MemorySource::Device);
    CHECK(w.r.View().next_hint && w.r.NextBookId() == 902 && !w.r.View().bedtime);
    CHECK(BookViewOf(w.r.Scene()).next_hint);
    {
        // The next book is not on the card: no arrow, no next book.
        Walk x;
        x.Open(&plain);
        for (int i = 0; i < 3; ++i) x.In(InputKind::DialDown);
        x.In(InputKind::DialPress);
        x.Wait(3100);
        x.In(InputKind::DialDown);
        x.In(InputKind::DialDown);
        CHECK(x.r.AtEnd() && !x.r.View().next_hint && x.r.NextBookId() == 0);
        CHECK(!BookViewOf(x.r.Scene()).next_hint);
    }
    // Up from the end goes back along the way we came: 5, 3, 2, 1.
    for (uint16_t page : {5, 3, 2, 1}) {
        CHECK(w.In(InputKind::DialUp).publish && w.r.View() == View(page));
        CHECK(!w.r.AtEnd());
    }
    CHECK(!w.In(InputKind::DialUp).publish);
}

void TestRightEnding(const BookOutline& o) {
    Walk w;
    w.Open(&o);
    w.In(InputKind::DialDown);
    w.In(InputKind::DialDown);
    w.In(InputKind::DialDown);  // highlight right
    w.In(InputKind::DialPress);
    w.Wait(3100);
    CHECK(w.r.View() == View(4));
    CHECK(w.In(InputKind::DialDown).publish && w.r.View() == View(6));  // 4 links to 6
    BookStep end = w.In(InputKind::DialDown);
    CHECK(w.r.AtEnd() && end.memory.kind == BookMemoryKind::Finished);
    // Every ending counts as finished, once per open; a re-read starts at 1.
    CHECK(w.In(InputKind::DialPress).publish && w.r.View() == View(1) && !w.r.AtEnd());
    CHECK(!w.In(InputKind::DialUp).publish);  // history is gone
    CHECK(w.In(InputKind::DialLong).home);
}

void TestRestart(const std::vector<uint8_t>& good) {
    std::vector<uint8_t> b = good;
    b[GotoAt(0, 1)] = 0x81;
    Put16(b, NextAt(3), 0);
    Put16(b, NextAt(4), 5);
    Put16(b, NextAt(5), 6);
    Reseal(b);
    Loaded book(b);
    Walk w;
    w.Open(&book.outline);
    w.In(InputKind::DialDown);
    w.In(InputKind::DialDown);
    w.In(InputKind::DialDown);  // "again" option
    w.In(InputKind::DialPress);
    BookStep s = w.Wait(3100);
    CHECK(s.publish && w.r.View() == View(1) && !w.r.AtEnd());
    CHECK(s.memory.kind == BookMemoryKind::None);  // reading again is not finishing
    CHECK(!w.In(InputKind::DialUp).publish);       // history cleared by the restart
}

void TestBedtime(const BookOutline& o) {
    Walk w;
    w.Open(&o, true);
    CHECK(w.r.Bedtime() && w.r.View().bedtime && w.r.View().page == 1);
    CHECK(w.last.clip.kind == BookClipKind::Page);
    CHECK(BookViewOf(w.r.Scene()).bedtime);
    // Page 1: narration 1000 ms + 2500 ms pause, then the page turns by itself.
    CHECK(!w.Wait(3400).publish);
    BookStep s = w.Wait(200);
    CHECK(s.publish && w.r.View().page == 2 && w.r.View().bedtime);
    CHECK(s.clip.kind == BookClipKind::Stop);
    // Page 2 is a branch page: nobody is asked, the story takes the first road.
    w.Shown();
    CHECK(w.last.clip.kind == BookClipKind::Page);
    CHECK(!w.Wait(3400).publish);
    s = w.Wait(200);
    CHECK(s.publish && w.r.View() == View(3) && !w.r.Asking());
    // Page 3 has a like question, which bedtime never asks (and never remembers).
    w.Shown();
    s = w.Wait(10000);
    CHECK(s.publish && w.r.View().page == 5 && s.memory.kind == BookMemoryKind::None);
    w.Shown();
    s = w.Wait(3600);
    // Read to the end by itself in bedtime mode: not "finished" (no memory).
    CHECK(s.publish && w.r.AtEnd() && s.memory.kind == BookMemoryKind::None);
    CHECK(w.r.View().bedtime && BookScene(w.r.View(), true).emotion == Emotion::Sleepy);
    // TuanTuan yawns for 6 s, then the normal sleep flow is asked for, once.
    CHECK(!w.Wait(5900).sleep);
    CHECK(w.Wait(200).sleep);
    CHECK(!w.Wait(60000).sleep);
}

void TestBedtimeExit(const BookOutline& o) {
    // A dial turn at any time means normal reading, and the turn still counts.
    {
        Walk w;
        w.Open(&o, true);
        w.Wait(3600);  // -> page 2
        w.Shown();
        BookStep s = w.In(InputKind::DialDown);
        CHECK(s.bedtime_off && !w.r.Bedtime() && !w.r.View().bedtime);
        CHECK(w.r.Asking() && w.r.View() == View(2, 1, 0));  // asked, as in normal mode
        w.Wait(120000);  // no auto-turning any more
        CHECK(w.r.View().page == 2);
    }
    {
        Walk w;
        w.Open(&o, true);
        w.Wait(3600);  // -> page 2
        w.Shown();
        BookStep s = w.In(InputKind::DialUp);
        CHECK(s.bedtime_off && s.publish && w.r.View() == View(1) && !w.r.View().bedtime);
        CHECK(!w.Wait(200000).sleep);
    }
    {
        // Up on page 1 turns nothing, but leaves bedtime; no auto turn follows.
        Walk w;
        w.Open(&o, true);
        BookStep s = w.In(InputKind::DialUp);
        CHECK(s.bedtime_off && !s.publish && !w.r.Bedtime());
        w.Wait(60000);
        CHECK(w.r.View().page == 1);
    }
    {
        // A press only reads the page again; bedtime stays and the turn is re-armed.
        Walk w;
        w.Open(&o, true);
        w.Wait(2000);
        BookStep s = w.In(InputKind::DialPress);
        CHECK(!s.bedtime_off && w.r.Bedtime() && s.clip.kind == BookClipKind::Page);
        CHECK(!w.Wait(3400).publish);
        CHECK(w.Wait(200).publish && w.r.View().page == 2);
    }
    {
        // Leaving bedtime on the end picture cancels the sleep.
        Walk w;
        w.Open(&o, true);
        for (int i = 0; i < 5; ++i) {
            w.Wait(10000);
            w.Shown();
        }
        CHECK(w.r.AtEnd());
        BookStep s = w.In(InputKind::DialUp);
        CHECK(s.bedtime_off && !w.r.AtEnd());
        CHECK(!w.Wait(120000).sleep);
    }
    {
        // Back in normal mode, reading to the end does count as finished.
        Walk w;
        w.Open(&o, true);
        w.Wait(3600);  // -> page 2
        w.Shown();
        w.In(InputKind::DialUp);  // normal mode, page 1
        BookStep s = w.In(InputKind::DialDown);
        CHECK(w.r.View().page == 2 && !w.r.Bedtime());
        w.In(InputKind::DialDown);  // asked
        w.In(InputKind::DialPress);  // left
        w.Wait(3100);
        w.In(InputKind::DialDown);  // page 3 -> 5
        s = w.In(InputKind::DialDown);
        CHECK(w.r.AtEnd() && s.memory.kind == BookMemoryKind::Finished);
    }
    {
        // Long press still goes home from bedtime.
        Walk w;
        w.Open(&o, true);
        CHECK(w.In(InputKind::DialLong).home);
    }
}

void TestLinearBooksUnchanged(const std::string& fixtures) {
    StoryPackFilePort port;
    StoryPackReader story;
    BookOutline o;
    CHECK(port.OpenBook(fixtures.c_str(), "golden_book") && story.Open(port) &&
          BuildBookOutline(story.Content(), &o));
    CHECK(story.Content().Version() == 2 && !story.Content().Branching() && !o.branching);
    CHECK(story.Content().SeriesId() == 0 && o.series_next == 0);
    // Bedtime on a linear book: pages turn by themselves, nobody is asked.
    Walk w;
    w.Open(&o, true);
    BookStep s = w.Wait(100 + 2500 - 1 + 1);
    CHECK(s.publish && w.r.View().page == 2);
    w.Shown();
    s = w.Wait(100 + 2500);
    CHECK(s.publish && w.r.AtEnd() && s.memory.kind == BookMemoryKind::None);
    CHECK(w.r.NextBookId() == 0 && !w.r.View().next_hint);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: test_branch_book <branch-test/books> <fixtures/kpb2>\n");
        return 2;
    }
    const std::string dir = argv[1];
    std::ifstream in(dir + "/branch_test.kpb", std::ios::binary);
    CHECK(in.good());
    const std::vector<uint8_t> good((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    TestFormat(good);
    TestRejects(good);
    TestSceneFlags();
    TestBedtimeTone();
    TestShelf();
    Loaded book(good);
    TestBranchWalk(book.outline);
    TestRightEnding(book.outline);
    TestRestart(good);
    TestBedtime(book.outline);
    TestBedtimeExit(book.outline);
    TestLinearBooksUnchanged(argv[2]);
    std::printf("PASS: branching books, bedtime story mode (%d checks)\n", g_checks);
    return 0;
}
