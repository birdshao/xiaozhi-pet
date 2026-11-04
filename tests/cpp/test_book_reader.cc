// V4-05 picture-book reader: outline from the golden KPB2/KPB1 packs, the
// dial state machine (pages, narration, read again, questions, end, home),
// the ADR-012 like-write guard, overlay frames and 4-grey RAM planes.
// Usage: test_book_reader <dir with golden_book.kpb, golden_kpb1.kpb>
#include "kid_app/book_reader.h"
#include "kid_app/icon_choice.h"
#include "kid_app/memory_store.h"
#include "kid_app/scene_model.h"
#include "kid_app/story_pack_file_port.h"
#include "kid_app/story_pack_reader.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
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

struct OpenBook {
    StoryPackFilePort port;
    StoryPackReader story;
    BookOutline outline;
    bool Open(const std::string& dir, const char* id) {
        return port.OpenBook(dir.c_str(), id) && story.Open(port) &&
               BuildBookOutline(story.Content(), &outline);
    }
};

bool Clip(const BookStep& s, BookClipKind kind, uint16_t page, uint8_t q = 0,
          uint8_t option = 0) {
    return s.clip.kind == kind && s.clip.page == page && s.clip.question == q &&
           s.clip.option == option;
}
bool NoMemory(const BookStep& s) { return s.memory.kind == BookMemoryKind::None; }
BookView View(uint16_t page, uint8_t question = 0, uint8_t highlight = 0,
              bool marked = false) {
    BookView v{};
    v.page = page;
    v.question = question;
    v.highlight = highlight;
    v.marked = marked;
    return v;
}

void TestOutline(const OpenBook& book, const OpenBook& kpb1) {
    const BookOutline& o = book.outline;
    CHECK(o.pages == 2 && o.kpb2 && o.content_id == 101);
    CHECK(o.QuestionTotal() == 2 && o.GreyPages() == 1);
    CHECK(o.page[0].grey && o.page[0].question_count == 2);
    CHECK(o.page[0].narration_ms == 100);  // 2,400 samples at 24 kHz
    const BookQuestionInfo& like = o.page[0].questions[0];
    CHECK(like.option_count == 3 && like.correct == 0 && like.like_slot == 1);
    CHECK(like.option_ids[0] == 3 && like.option_ids[1] == 4 && like.option_ids[2] == 0xFF03);
    CHECK(like.like_mask == 0x03);
    const BookQuestionInfo& quiz = o.page[0].questions[1];
    CHECK(quiz.option_count == 2 && quiz.correct == 1 && quiz.like_slot == 0 &&
          quiz.like_mask == 0);
    CHECK(!o.page[1].grey && o.page[1].question_count == 0);

    CHECK(kpb1.outline.pages == 1 && !kpb1.outline.kpb2 && kpb1.outline.content_id == 0);
    CHECK(kpb1.outline.QuestionTotal() == 0 && kpb1.outline.GreyPages() == 0);

    ContentPackReader closed;
    BookOutline out;
    out.pages = 9;
    CHECK(!BuildBookOutline(closed, &out) && out.pages == 0);
    CHECK(!BuildBookOutline(closed, nullptr));
}

void TestSceneCodec() {
    for (uint16_t page = 1; page <= 33; ++page)
        for (uint8_t q = 0; q <= 2; ++q)
            for (uint8_t h = 0; h < 3; ++h)
                for (bool marked : {false, true}) {
                    const BookView v = q ? View(page, q, h, marked) : View(page);
                    const SceneModel s = BookScene(v, false);
                    CHECK(s.kind == SceneKind::Reader && s.emotion == Emotion::Neutral);
                    CHECK(BookViewOf(s) == v);
                    CHECK(BookViewOf(UnpackScene(PackScene(s))) == v);
                }
    CHECK(BookScene(View(3), true).emotion == Emotion::Happy);
    // The durable StartStory scene (active_page 1) is a plain first page.
    SceneModel legacy{};
    legacy.kind = SceneKind::Reader;
    legacy.book_page = 1;
    legacy.home_selection = 1;
    CHECK(BookViewOf(legacy) == View(1));
    CHECK(BookScene(View(1), false).book_page == 1);  // no overlay: plain page number
}

void TestWalk(const BookOutline& o) {
    BookReader r;
    CHECK(!r.Active());
    CHECK(!r.OnInput(InputKind::DialDown, 0).publish);
    r.Open(&o, 0);
    CHECK(r.Active() && r.View() == View(1) && !r.Asking());

    // Nothing speaks before the page is on the panel; stale views are ignored.
    CHECK(r.Tick(5000).clip.kind == BookClipKind::None);
    CHECK(r.OnInput(InputKind::DialPress, 10).clip.kind == BookClipKind::None);
    CHECK(r.OnShown(View(2), 20).clip.kind == BookClipKind::None);
    BookStep s = r.OnShown(View(1), 1000);
    CHECK(Clip(s, BookClipKind::Page, 1) && !s.publish);
    CHECK(r.OnShown(View(1), 1001).clip.kind == BookClipKind::None);  // once per visit

    // The first question comes once the 100 ms narration + 1.5 s are over.
    CHECK(!r.Tick(1000 + 100 + BookReader::kQuestionDelayMs - 1).publish);
    s = r.Tick(1000 + 100 + BookReader::kQuestionDelayMs);
    CHECK(s.publish && r.Asking() && r.View() == View(1, 1, 0));
    CHECK(s.clip.kind == BookClipKind::Stop);
    s = r.OnShown(View(1, 1, 0), 3000);
    CHECK(Clip(s, BookClipKind::Prompt, 1, 0));
    CHECK(r.OnShown(View(1, 1, 0), 3001).clip.kind == BookClipKind::None);

    // Dial moves the highlight (each move says the option), clamped.
    s = r.OnInput(InputKind::DialDown, 3100);
    CHECK(s.publish && Clip(s, BookClipKind::Option, 1, 0, 1) && r.View() == View(1, 1, 1));
    s = r.OnInput(InputKind::DialDown, 3200);
    CHECK(Clip(s, BookClipKind::Option, 1, 0, 2) && r.View().highlight == 2);
    s = r.OnInput(InputKind::DialDown, 3300);
    CHECK(!s.publish && s.clip.kind == BookClipKind::None);
    s = r.OnInput(InputKind::DialUp, 3400);
    CHECK(Clip(s, BookClipKind::Option, 1, 0, 1) && NoMemory(s));

    // Press answers the like question: bird (slot 1 option 4) is a like.
    s = r.OnInput(InputKind::DialPress, 4000);
    CHECK(s.publish && s.answer == BookAnswer::Choice && s.chosen == 1);
    CHECK(s.memory.kind == BookMemoryKind::Like && s.memory.page == 1 &&
          s.memory.question == 0 && s.memory.option == 1);
    CHECK(Clip(s, BookClipKind::Option, 1, 0, 1) && r.View() == View(1, 1, 1, true));
    // While the answer is shown the dial does nothing (and never writes).
    for (InputKind k : {InputKind::DialUp, InputKind::DialDown, InputKind::DialPress}) {
        s = r.OnInput(k, 4100);
        CHECK(!s.publish && NoMemory(s) && s.answer == BookAnswer::None);
    }
    CHECK(!r.Tick(4000 + BookReader::kReactionMs - 1).publish);
    s = r.Tick(4000 + BookReader::kReactionMs);
    CHECK(s.publish && r.View() == View(1, 2, 0) && r.Asking());
    CHECK(Clip(r.OnShown(View(1, 2, 0), 7100), BookClipKind::Prompt, 1, 1));

    // Quiz, wrong pick: the right one is marked and named, no memory.
    r.OnInput(InputKind::DialDown, 7200);
    s = r.OnInput(InputKind::DialPress, 7300);
    CHECK(s.answer == BookAnswer::Wrong && s.chosen == 1 && NoMemory(s));
    CHECK(Clip(s, BookClipKind::Option, 1, 1, 0) && r.View() == View(1, 2, 0, true));
    // Then the plain page returns quietly (it was read already).
    s = r.Tick(7300 + BookReader::kReactionMs);
    CHECK(s.publish && s.clip.kind == BookClipKind::None && r.View() == View(1) && !r.Asking());
    CHECK(r.OnShown(View(1), 11000).clip.kind == BookClipKind::None);
    CHECK(!r.Tick(60000).publish);  // all questions of this visit asked

    // Press = read again: narration restarts, no question this visit.
    s = r.OnInput(InputKind::DialPress, 61000);
    CHECK(Clip(s, BookClipKind::Page, 1) && !s.publish && NoMemory(s));
    CHECK(!r.Tick(70000).publish);

    // Next page, then the end picture (book finished once).
    s = r.OnInput(InputKind::DialDown, 71000);
    CHECK(s.publish && s.clip.kind == BookClipKind::Stop && r.View() == View(2));
    CHECK(Clip(r.OnShown(View(2), 72000), BookClipKind::Page, 2));
    CHECK(!r.Tick(90000).publish);  // page 2 has no question
    s = r.OnInput(InputKind::DialDown, 91000);
    CHECK(s.publish && r.AtEnd() && r.View() == View(3) && s.clip.kind == BookClipKind::Stop);
    CHECK(s.memory.kind == BookMemoryKind::Finished);
    CHECK(r.Scene().emotion == Emotion::Happy && BookViewOf(r.Scene()) == View(3));
    CHECK(!r.OnInput(InputKind::DialDown, 91500).publish);
    CHECK(r.OnShown(View(3), 92000).clip.kind == BookClipKind::None);
    s = r.OnInput(InputKind::DialUp, 93000);
    CHECK(s.publish && r.View() == View(2) && !r.AtEnd());
    s = r.OnInput(InputKind::DialDown, 94000);
    CHECK(r.AtEnd() && NoMemory(s));  // finished only once per opening
    s = r.OnInput(InputKind::DialPress, 95000);  // start again
    CHECK(s.publish && r.View() == View(1));
    // Up on the first page does nothing.
    CHECK(!r.OnInput(InputKind::DialUp, 95500).publish);

    // Long press goes home from anywhere, also mid-question.
    r.OnShown(View(1), 96000);
    r.Tick(96000 + 100 + BookReader::kQuestionDelayMs);
    CHECK(r.Asking());
    s = r.OnInput(InputKind::DialLong, 98000);
    CHECK(s.home && s.clip.kind == BookClipKind::Stop && NoMemory(s) && !s.publish);
    // Other kinds the board owns are ignored.
    CHECK(!r.OnInput(InputKind::Talk, 98100).publish);
    r.Close();
    CHECK(!r.Active() && !r.OnInput(InputKind::DialLong, 99000).home);
}

void TestSkipAndRevisit(const BookOutline& o) {
    BookReader r;
    r.Open(&o, 0);
    r.OnShown(View(1), 0);
    // Turning the page during narration skips the questions.
    BookStep s = r.OnInput(InputKind::DialDown, 50);
    CHECK(s.publish && r.View() == View(2));
    CHECK(!r.Tick(10000).publish && !r.Asking());
    // Coming back is a new visit: narration again, questions again.
    r.OnInput(InputKind::DialUp, 11000);
    CHECK(Clip(r.OnShown(View(1), 12000), BookClipKind::Page, 1));
    // Read again before the question pushes it back.
    CHECK(Clip(r.OnInput(InputKind::DialPress, 12500), BookClipKind::Page, 1));
    CHECK(!r.Tick(12000 + 100 + BookReader::kQuestionDelayMs).publish);
    CHECK(r.Tick(12500 + 100 + BookReader::kQuestionDelayMs).publish && r.Asking());
    // The right quiz answer: second question, highlight on the circle.
    r.OnInput(InputKind::DialPress, 20000);  // like question: rabbit
    r.Tick(20000 + BookReader::kReactionMs);
    s = r.OnInput(InputKind::DialPress, 30000);
    CHECK(s.answer == BookAnswer::Right && s.chosen == 0 && NoMemory(s));
    CHECK(r.View() == View(1, 2, 0, true));
}

void TestNoBookAndKpb1(const BookOutline& kpb1) {
    BookOutline none{};
    BookReader r;
    r.Open(&none, 0);
    for (InputKind k : {InputKind::DialUp, InputKind::DialDown, InputKind::DialPress}) {
        const BookStep s = r.OnInput(k, 10);
        CHECK(!s.publish && s.clip.kind == BookClipKind::None && NoMemory(s));
    }
    CHECK(r.OnShown(View(1), 20).clip.kind == BookClipKind::None);
    CHECK(r.OnInput(InputKind::DialLong, 30).home);

    r.Open(&kpb1, 0);
    CHECK(Clip(r.OnShown(View(1), 0), BookClipKind::Page, 1));
    const BookStep s = r.OnInput(InputKind::DialDown, 100);
    CHECK(r.AtEnd() && NoMemory(s));  // KPB1 has no numeric id: nothing to remember
}

void TestMemoryGuard(const BookOutline& o) {
    MemoryEvent e{};
    BookMemoryRequest like{BookMemoryKind::Like, 1, 0, 0};
    CHECK(BookMemoryEvent(o, like, 77, 9, &e));
    CHECK(e.id == 77 && e.day == 9 && e.kind == MemoryEventKind::PreferenceChosen &&
          e.source == MemorySource::DialChoice && e.slot == 1 && e.option == 3);
    MemoryStore memory;
    CHECK(memory.Apply(e) == MemoryResult::Applied);
    uint16_t option = 0;
    CHECK(memory.FindPreference(1, &option) && option == 3);
    // The same pick from voice is refused by the store (ADR-012).
    MemoryEvent voice = e;
    voice.source = MemorySource::Voice;
    CHECK(memory.Apply(voice) == MemoryResult::Rejected);

    MemoryEvent untouched{};
    untouched.id = 5;
    auto rejects = [&](BookMemoryRequest r, uint64_t id = 1) {
        MemoryEvent out = untouched;
        return !BookMemoryEvent(o, r, id, 0, &out) && out.id == 5;
    };
    CHECK(rejects({BookMemoryKind::Like, 1, 0, 2}));     // "both": not a like
    CHECK(rejects({BookMemoryKind::Like, 1, 1, 0}));     // quiz never writes
    CHECK(rejects({BookMemoryKind::Like, 1, 0, 3}));     // no such option
    CHECK(rejects({BookMemoryKind::Like, 2, 0, 0}));     // page without questions
    CHECK(rejects({BookMemoryKind::Like, 3, 0, 0}));     // past the book
    CHECK(rejects({BookMemoryKind::Like, 0, 0, 0}));
    CHECK(rejects({BookMemoryKind::None, 1, 0, 0}));
    CHECK(rejects(like, 0));                             // no event id
    CHECK(!BookMemoryEvent(o, like, 1, 0, nullptr));

    CHECK(BookMemoryEvent(o, {BookMemoryKind::Finished, 0, 0, 0}, 8, 3, &e));
    CHECK(e.kind == MemoryEventKind::BookFinished && e.source == MemorySource::Device &&
          e.option == 101 && e.day == 3);
    BookOutline kpb1 = o;
    kpb1.kpb2 = false;
    CHECK(rejects({BookMemoryKind::Finished, 0, 0, 0}) == false);
    MemoryEvent out = untouched;
    CHECK(!BookMemoryEvent(kpb1, {BookMemoryKind::Finished, 0, 0, 0}, 8, 3, &out));

    // Walking the whole book with every dial gesture except a press on a
    // like option never asks for a like.
    BookReader r;
    r.Open(&o, 0);
    r.OnShown(View(1), 0);
    r.Tick(100 + BookReader::kQuestionDelayMs);
    r.OnInput(InputKind::DialDown, 2000);
    r.OnInput(InputKind::DialDown, 2100);  // "both"
    const BookStep s = r.OnInput(InputKind::DialPress, 2200);
    CHECK(s.answer == BookAnswer::Choice && s.chosen == 2 && NoMemory(s));
}

void TestOverlayFrames(OpenBook& book) {
    ContentPackReader& content = book.story.Content();
    ContentQuestion q{};
    CHECK(content.Question(1, 0, &q));
    auto base = std::make_unique<MonoFrame>();
    CHECK(book.story.ReadFrame(1, *base));

    auto frame = std::make_unique<MonoFrame>(*base);
    CHECK(ComposeBookOverlay(content, View(1, 1, 1), *frame));
    for (uint8_t o = 0; o < 3; ++o) {
        const PageRect ring = HighlightRect(q.options[o]);
        const bool lit = o == 1;
        // Outer 4 px black only on the highlighted card, rest of margin white.
        CHECK(frame->IsBlack(ring.x, ring.y) == lit);
        CHECK(frame->IsBlack(ring.x + 3, ring.y + ring.h / 2) == lit);
        CHECK(!frame->IsBlack(ring.x + 4, ring.y + ring.h / 2));
        CHECK(!frame->IsBlack(ring.x + ring.w - 5, ring.y + 5));
    }
    // Icons are the pack's icons.
    auto icons = std::make_unique<MonoFrame>(*base);
    for (uint8_t o = 0; o < 3; ++o) CHECK(content.BlitOptionIcon(q, o, *icons));
    for (uint8_t o = 0; o < 3; ++o) {
        const PageRect b = q.options[o].box;
        for (int y = b.y; y < b.y + b.h; ++y)
            for (int x = b.x; x < b.x + b.w; ++x)
                CHECK(frame->IsBlack(x, y) == icons->IsBlack(x, y));
    }
    // Away from the cards the page picture is untouched.
    CHECK(frame->Data()[0] == base->Data()[0]);
    CHECK(frame->IsBlack(799, 0) == base->IsBlack(799, 0));

    auto marked = std::make_unique<MonoFrame>(*base);
    CHECK(ComposeBookOverlay(content, View(1, 1, 1, true), *marked));
    const PageRect ring = HighlightRect(q.options[1]);
    CHECK(marked->IsBlack(ring.x + 7, ring.y + ring.h / 2));
    CHECK(marked->IsBlack(ring.x + ring.w / 2, ring.y + 7));

    // Forced "previous image" from the plain page: every card pixel differs
    // from the new picture, everything else is what the panel shows.
    auto forced = std::make_unique<MonoFrame>(*base);
    CHECK(ForceBookOverlay(content, View(1, 1, 1), *frame, *forced));
    for (uint8_t o = 0; o < 3; ++o) {
        const PageRect r = HighlightRect(q.options[o]);
        for (int y = r.y; y < r.y + r.h; ++y)
            for (int x = r.x; x < r.x + r.w; ++x)
                CHECK(forced->IsBlack(x, y) != frame->IsBlack(x, y));
    }
    CHECK(forced->Data()[0] == base->Data()[0]);
    // From question 1 to question 2: the panel still shows question 1's
    // cards; outside question 2's cards they stay as shown, so the partial
    // refresh clears them (they differ from the new picture).
    ContentQuestion q2{};
    CHECK(content.Question(1, 1, &q2));
    auto second = std::make_unique<MonoFrame>(*base);
    CHECK(ComposeBookOverlay(content, View(1, 2, 0), *second));
    auto previous = std::make_unique<MonoFrame>(*marked);
    CHECK(ForceBookOverlay(content, View(1, 2, 0), *second, *previous));
    const PageRect old_ring = HighlightRect(q.options[1]);
    CHECK(previous->IsBlack(old_ring.x + 6, old_ring.y + 40));
    CHECK(previous->IsBlack(old_ring.x + 6, old_ring.y + 40) !=
          second->IsBlack(old_ring.x + 6, old_ring.y + 40));
    for (uint8_t o = 0; o < 2; ++o) {
        const PageRect r = HighlightRect(q2.options[o]);
        CHECK(previous->IsBlack(r.x + 1, r.y + 1) != second->IsBlack(r.x + 1, r.y + 1));
    }

    // Bad views draw nothing.
    auto bad = std::make_unique<MonoFrame>(*base);
    CHECK(!ComposeBookOverlay(content, View(1), *bad));
    CHECK(!ComposeBookOverlay(content, View(1, 1, 3), *bad));
    CHECK(!ComposeBookOverlay(content, View(2, 1, 0), *bad));
    CHECK(!ForceBookOverlay(content, View(2, 1, 0), *base, *bad));
    CHECK(bad->Data() == base->Data());
}

void TestGreyPlanes(OpenBook& book) {
    std::vector<uint8_t> grey(ContentPackReader::kGreyBytes);
    CHECK(book.story.Content().ReadGrey(1, grey.data(), grey.size()));
    std::vector<uint8_t> ram24(ContentPackReader::kMonoBytes), ram26(ram24.size());
    CHECK(EpdGreyPlanes(grey.data(), grey.size(), ram24.data(), ram26.data()));
    // Golden page 1: four bands, x / 200 = level 0 (black) .. 3 (white).
    auto bit = [](const std::vector<uint8_t>& p, int x, int y) {
        return (p[y * 100 + x / 8] >> (7 - x % 8)) & 1;
    };
    const int expect24[] = {1, 0, 1, 0};  // official: black and light grey
    const int expect26[] = {1, 1, 0, 0};  // official: black and dark grey
    for (int band = 0; band < 4; ++band)
        for (int y : {0, 239, 479}) {
            CHECK(bit(ram24, band * 200 + 13, y) == expect24[band]);
            CHECK(bit(ram26, band * 200 + 187, y) == expect26[band]);
        }
    CHECK(!EpdGreyPlanes(grey.data(), 3, ram24.data(), ram26.data()));
    CHECK(!EpdGreyPlanes(nullptr, 2, ram24.data(), ram26.data()));
}

void TestIconChoice() {
    IconChoice c;
    CHECK(!c.Move(1) && c.Count() == 0);
    c.Reset(3);
    CHECK(c.Index() == 0 && !c.Move(-1) && c.Move(+1) && c.Move(+5) && c.Index() == 2);
    CHECK(!c.Move(+1) && !c.Move(0) && c.Move(-1) && c.Index() == 1);
    c.Reset(2, 5);
    CHECK(c.Index() == 0 && c.Count() == 2);
    c.Reset(9, 3);
    CHECK(c.Count() == IconChoice::kMaxOptions && c.Index() == 3);

    auto f = std::make_unique<MonoFrame>();
    const PageRect ring{100, 50, 80, 64};
    DrawChoiceRing(*f, ring, 8, ChoiceRing::Highlight);
    CHECK(f->IsBlack(100, 50) && f->IsBlack(103, 80) && !f->IsBlack(104, 80));
    CHECK(f->IsBlack(179, 113) && !f->IsBlack(175, 80) && f->IsBlack(176, 80));
    CHECK(!f->IsBlack(140, 58) && f->IsBlack(140, 53) && !f->IsBlack(140, 54));
    CHECK(f->IsBlack(100, 60) && !f->IsBlack(99, 60) && !f->IsBlack(180, 60));
    f->SetPixel(140, 80, true);  // inside the icon area: never touched
    DrawChoiceRing(*f, ring, 8, ChoiceRing::Plain);
    CHECK(!f->IsBlack(100, 50) && !f->IsBlack(103, 80) && f->IsBlack(140, 80));
    DrawChoiceRing(*f, ring, 8, ChoiceRing::Chosen);
    CHECK(f->IsBlack(107, 80) && !f->IsBlack(108, 80) && f->IsBlack(140, 57));
    const MonoFrame white;
    OpposeRect(*f, white, PageRect{796, 476, 8, 8});  // clipped at the frame edge
    CHECK(f->IsBlack(799, 479) && !f->IsBlack(795, 479));
}

// tools/make_reader_test_book.py output, as the board trial reads it.
void TestReaderTestBook(const std::string& dir) {
    auto book = std::make_unique<OpenBook>();
    CHECK(book->Open(dir, "reader_test"));
    const BookOutline& o = book->outline;
    CHECK(o.pages == 3 && o.kpb2 && o.content_id == 900);
    CHECK(o.GreyPages() == 2 && o.QuestionTotal() == 2);
    CHECK(o.page[0].narration_ms == 2000 && o.page[1].narration_ms == 1500 &&
          o.page[2].narration_ms == 1000 && !o.page[2].grey);
    const BookQuestionInfo& like = o.page[0].questions[0];
    CHECK(like.like_slot == 2 && like.option_count == 3 && like.like_mask == 0x03);
    CHECK(like.option_ids[0] == 1 && like.option_ids[1] == 3);  // red, blue
    CHECK(o.page[0].questions[1].correct == 1 && o.page[0].questions[1].like_slot == 0);

    // The self-test walk (tools/selftest/v4_book_reader.pettest).
    BookReader r;
    r.Open(&o, 0);
    CHECK(Clip(r.OnShown(View(1), 5000), BookClipKind::Page, 1));
    CHECK(r.Tick(5000 + 2000 + BookReader::kQuestionDelayMs).publish && r.Asking());
    CHECK(Clip(r.OnShown(View(1, 1, 0), 9000), BookClipKind::Prompt, 1, 0));
    r.OnInput(InputKind::DialDown, 10000);
    r.OnInput(InputKind::DialDown, 12500);
    r.OnInput(InputKind::DialUp, 15000);
    BookStep s = r.OnInput(InputKind::DialPress, 17500);
    MemoryEvent e{};
    CHECK(BookMemoryEvent(o, s.memory, 1, 0, &e) && e.slot == 2 && e.option == 3);
    r.Tick(17500 + BookReader::kReactionMs);
    r.OnInput(InputKind::DialDown, 23000);
    s = r.OnInput(InputKind::DialPress, 25500);
    CHECK(s.answer == BookAnswer::Wrong && NoMemory(s));
    r.Tick(25500 + BookReader::kReactionMs);
    CHECK(Clip(r.OnInput(InputKind::DialPress, 34500), BookClipKind::Page, 1));
    for (int page : {2, 3, 2, 3}) {
        r.OnInput(page == 2 && r.View().page == 3 ? InputKind::DialUp : InputKind::DialDown, 0);
        CHECK(r.View() == View(static_cast<uint16_t>(page)));
    }
    s = r.OnInput(InputKind::DialDown, 80000);
    CHECK(r.AtEnd() && BookMemoryEvent(o, s.memory, 2, 0, &e) && e.option == 900);
    CHECK(r.OnInput(InputKind::DialUp, 87000).publish && r.View() == View(3));
    CHECK(r.OnInput(InputKind::DialLong, 94000).home);

    // Overlays of both questions draw from this pack.
    auto frame = std::make_unique<MonoFrame>();
    CHECK(book->story.ReadFrame(1, *frame));
    CHECK(ComposeBookOverlay(book->story.Content(), View(1, 1, 2), *frame));
    CHECK(ComposeBookOverlay(book->story.Content(), View(1, 2, 1, true), *frame));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2 && argc != 3) {
        std::fprintf(stderr, "usage: test_book_reader <fixtures/kpb2> [reader-test/books]\n");
        return 2;
    }
    const std::string dir = argv[1];
    auto book = std::make_unique<OpenBook>();
    auto kpb1 = std::make_unique<OpenBook>();
    CHECK(book->Open(dir, "golden_book"));
    CHECK(kpb1->Open(dir, "golden_kpb1"));
    TestOutline(*book, *kpb1);
    TestSceneCodec();
    TestWalk(book->outline);
    TestSkipAndRevisit(book->outline);
    TestNoBookAndKpb1(kpb1->outline);
    TestMemoryGuard(book->outline);
    TestOverlayFrames(*book);
    TestGreyPlanes(*book);
    TestIconChoice();
    if (argc == 3) TestReaderTestBook(argv[2]);
    std::printf("PASS: book reader state machine, like guard, overlays and grey planes "
                "(%d checks)\n", g_checks);
    return 0;
}
