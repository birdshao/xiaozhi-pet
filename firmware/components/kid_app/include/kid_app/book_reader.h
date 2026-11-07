#pragma once

#include "kid_app/content_pack_reader.h"
#include "kid_app/icon_choice.h"
#include "kid_app/input_hub.h"
#include "kid_app/rhythm_scheduler.h"
#include "kid_app/scene_model.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace kid_app {

struct MemoryEvent;

// V4-05 picture-book reader. Everything the dial needs about one book,
// read once from the validated pack (no file IO afterwards), so the input
// task never touches the SD card the ePaper task reads pages from.
struct BookQuestionInfo {
    uint8_t option_count = 0;  // 2..3
    uint8_t correct = 0;       // 0 = no right/wrong, else 1-based option
    uint16_t like_slot = 0;    // 0 = never writes memory (ADR-012)
    std::array<uint16_t, 3> option_ids{};
    uint8_t like_mask = 0;     // bit n: option n counts as a like
    // KPB2 v3 branch question: page each option jumps to (kGotoRestart =
    // back to page 1). Never set unless every option jumps.
    bool branch = false;
    std::array<uint8_t, 3> goto_page{};
};

struct BookPageInfo {
    bool grey = false;          // KPB2 4-level picture present
    uint8_t question_count = 0;
    uint32_t narration_ms = 0;  // 24 kHz mono PCM16 length
    uint16_t next = 0;          // v3 link after this page (see ContentPage::next)
    uint8_t branch_q = 0xFF;    // 0-based branch question, 0xFF = none
    std::array<BookQuestionInfo, ContentPackReader::kMaxQuestionsPerPage> questions{};
};

struct BookOutline {
    uint16_t pages = 0;       // 0 = no readable book (placeholder page)
    uint16_t content_id = 0;  // KPB2 numeric id; 0 for KPB1
    bool kpb2 = false;
    bool branching = false;   // KPB2 v3 with links: history-based paging
    uint16_t series_id = 0;   // 0 = standalone
    uint16_t series_index = 0, series_count = 0;
    uint16_t series_next = 0;  // content id of the next book, 0 = none
    // Set by the board: the next book is really on the card (the end picture
    // only points at it then).
    bool next_book_present = false;
    std::array<BookPageInfo, ContentPackReader::kMaxPages> page{};

    uint16_t QuestionTotal() const;
    uint16_t GreyPages() const;
};

// False (and an empty outline) unless the reader is open and every page and
// question record reads back.
bool BuildBookOutline(ContentPackReader& reader, BookOutline* out);

// What the panel should show. page = pages + 1 is "the end" picture.
struct BookView {
    uint16_t page = 1;
    uint8_t question = 0;   // 0 = plain page, else 1-based question on it
    uint8_t highlight = 0;  // 0-based option with the dial highlight
    bool marked = false;    // the answer is shown (reaction)
    // Not part of the identity (OnShown / refresh compare page + overlay):
    // bedtime = low-contrast page, next_hint = the end picture points to the
    // next book of the series.
    bool bedtime = false;
    bool next_hint = false;

    bool operator==(const BookView& o) const {
        return page == o.page && question == o.question &&
               highlight == o.highlight && marked == o.marked;
    }
    bool operator!=(const BookView& o) const { return !(*this == o); }
};

// A Reader SceneModel carries the view in book_page: bits 0-7 the page,
// bits 8-13 the question overlay (0 none, else 1 + q*6 + marked*3 + option),
// bit 14 = next-book hint, bit 15 = bedtime mode, so the gate and the packed
// scene word see every change. Pages are
// <= 33, and the legacy durable active_page (always 1 now) decodes as a
// plain page.
SceneModel BookScene(const BookView& view, bool end);
BookView BookViewOf(const SceneModel& scene);

// Audio the board starts now, all from the same pack (24 kHz PCM16).
enum class BookClipKind : uint8_t { None, Stop, Page, Prompt, Option };
struct BookClip {
    BookClipKind kind = BookClipKind::None;
    uint16_t page = 0;
    uint8_t question = 0;  // 0-based
    uint8_t option = 0;    // 0-based
};

enum class BookMemoryKind : uint8_t { None, Like, Finished };
struct BookMemoryRequest {
    BookMemoryKind kind = BookMemoryKind::None;
    uint16_t page = 0;
    uint8_t question = 0;  // 0-based
    uint8_t option = 0;    // 0-based
};

enum class BookAnswer : uint8_t { None, Choice, Right, Wrong };

// What the board does after one reader call. Nothing here touches NVS:
// memory goes through BookMemoryEvent + the ledger, "home" through the
// durable StopStory action.
struct BookStep {
    bool publish = false;  // show Scene()
    BookClip clip{};
    BookMemoryRequest memory{};
    BookAnswer answer = BookAnswer::None;
    uint8_t chosen = 0;    // 0-based option the child pressed
    bool home = false;     // long press: leave the book
    bool sleep = false;    // bedtime story done: start the normal sleep flow
    bool bedtime_off = false;  // the dial turned: back to normal reading
};

// Bedtime story mode (docs/V8_BOOKS.md): offered in the evening day phase.
inline bool BedtimeAvailable(DayPhase phase) { return phase == DayPhase::Evening; }

// Bookshelf order (same key as shelf_key in tools/kpb2.py): a series stays
// together in reading order, books are placed by series id / content id.
struct ShelfEntry {
    uint16_t content_id = 0;
    uint16_t series_id = 0;  // 0 = standalone
    uint16_t series_index = 0;
};
inline bool ShelfBefore(const ShelfEntry& a, const ShelfEntry& b) {
    const uint32_t ka = a.series_id ? a.series_id : a.content_id;
    const uint32_t kb = b.series_id ? b.series_id : b.content_id;
    if (ka != kb) return ka < kb;
    if ((a.series_id != 0) != (b.series_id != 0)) return a.series_id == 0;
    if (a.series_index != b.series_index) return a.series_index < b.series_index;
    return a.content_id < b.content_id;
}

// Bedtime picture: pulls the 4-grey levels towards the middle (black and
// white become dark / light grey: 0,1,2,3 -> 1,1,2,2) in place; `bytes` of
// 4 px/byte grey data as ReadGrey returns them.
void BedtimeTone(uint8_t* grey, size_t bytes);

// Deterministic reader state machine (PRD §4/§5.2/§7), owned by the input
// task. Dial down/up = next/previous page, press = read the page again,
// long press = home. On a page with questions the first question appears
// once the narration is over: dial moves the highlight across the 2-3
// icons (each says its name), press answers (the answer's name again and a
// "chosen" ring: the child's pick, or the right one for a quiz, never a
// "wrong" sound), then the next question or the plain page comes back.
// Turning the page skips the questions; coming back asks them again. Past
// Past the last page is the end picture (press starts again, up goes back).
// Without a book (empty outline) it shows the "no book" picture of
// `missing`: press or long press goes home, the dial does nothing, and it
// never speaks or asks for a memory.
class BookReader {
public:
    static constexpr uint32_t kQuestionDelayMs = 1500;  // after narration
    static constexpr uint32_t kReactionMs = 3000;
    static constexpr uint32_t kBedtimePauseMs = 2500;  // after narration, then turn
    static constexpr uint32_t kBedtimeYawnMs = 6000;   // end picture, then sleep

    // missing: why there is no book when the outline is empty (None counts
    // as NoBook); ignored for a readable book.
    // bedtime: start in the low-contrast, auto-turning, question-free mode.
    void Open(const BookOutline* outline, uint32_t now_ms,
              BookMissing missing = BookMissing::None, bool bedtime = false);
    void Close();
    bool Active() const { return phase_ != Phase::Closed; }

    BookStep OnInput(InputKind kind, uint32_t now_ms);
    // The ePaper task drew `view` from the card (only then may it speak).
    BookStep OnShown(const BookView& view, uint32_t now_ms);
    BookStep Tick(uint32_t now_ms);

    const BookView& View() const { return view_; }
    bool AtEnd() const { return phase_ == Phase::End; }
    bool Asking() const { return phase_ == Phase::Question; }
    bool Bedtime() const { return bedtime_; }
    // Content id of the next book of the series at the end picture, else 0
    // (also 0 when that book is not on the card).
    uint16_t NextBookId() const;
    // None while a readable book is open (or closed).
    BookMissing Missing() const { return missing_; }
    SceneModel Scene() const {
        return phase_ == Phase::Missing ? BookMissingScene(missing_) : BookScene(view_, AtEnd());
    }

private:
    enum class Phase : uint8_t { Closed, Page, Question, Reaction, End, Missing };

    struct Target {
        uint16_t page = 0;  // 0 = the end of the book
        bool restart = false;
    };
    const BookPageInfo* PageInfo() const;
    BookStep OnInputCore(InputKind kind, uint32_t now_ms);
    Target NextTarget() const;
    Target OptionTarget(const BookQuestionInfo& info, uint8_t option) const;
    BookStep Forward(uint32_t now_ms);
    BookStep Advance(Target target, uint32_t now_ms);
    BookStep Retreat();
    BookStep Finish(uint32_t now_ms);
    void PushHistory(uint16_t page);
    BookStep GoTo(uint16_t page);
    BookStep Ask(uint8_t question);
    void ArmQuestion(uint32_t now_ms);

    const BookOutline* outline_ = nullptr;
    Phase phase_ = Phase::Closed;
    BookView view_{};
    IconChoice choice_{};
    bool narrated_ = false;       // this page visit has started its narration
    bool prompted_ = false;       // this question has played its prompt
    uint8_t next_question_ = 0;   // 0-based, == count: all asked this visit
    bool question_due_ = false;
    uint32_t question_at_ms_ = 0;
    uint32_t reaction_until_ms_ = 0;
    bool finished_ = false;       // BookFinished asked for once per Open
    // Pages visited on the way here: "previous page" is the one we came from
    // (equals page - 1 in a linear book). The graph is acyclic apart from
    // restarts, which clear it, so it never holds more than pages + 1.
    std::array<uint8_t, ContentPackReader::kMaxPages + 2> history_{};
    uint8_t history_n_ = 0;
    Target branch_target_{};      // where the answered branch question leads
    bool bedtime_ = false;
    bool auto_due_ = false;       // bedtime: turn the page at auto_at_ms_
    uint32_t auto_at_ms_ = 0;
    bool end_sleep_armed_ = false;
    uint32_t end_sleep_at_ms_ = 0;
    BookMissing missing_ = BookMissing::None;
};

// Log name of a missing-book reason: none, no_card, no_book, bad_book.
const char* BookMissingName(BookMissing reason);

// ADR-012 / V5-03: turns a reader memory request into the ledger event. A
// like goes ONLY through ChoiceMemoryEvent (source DialChoice), and only for
// an option the pack marks as a like on a like question; BookReader asks for
// it only from a dial press. Finished needs a KPB2 content id.
bool BookMemoryEvent(const BookOutline& outline, const BookMemoryRequest& request,
                     uint64_t event_id, uint32_t day, MemoryEvent* out);

// The picture a question overlay shows: page picture (already in frame),
// every option icon on a white card, the highlight / chosen ring.
bool ComposeBookOverlay(ContentPackReader& reader, const BookView& view,
                        MonoFrame& frame);
// The "previous image" for a question's first partial refresh: `previous`
// holds what the panel shows (1-bit), and every card of the view is set to
// the opposite of `next`, so all card pixels are driven (the page under a
// card may be grey) and an earlier question's cards elsewhere are cleared.
bool ForceBookOverlay(ContentPackReader& reader, const BookView& view,
                      const MonoFrame& next, MonoFrame& previous);

// Waveshare 3.97 4-grey RAM planes (official EPD_3IN97_Display_4Gray): with
// level 0 black .. 3 white, RAM 0x24 bit = 1 for levels 0 and 2 (= NOT lo),
// RAM 0x26 bit = 1 for levels 0 and 1 (= NOT hi). grey_bytes even; writes
// grey_bytes / 2 bytes to each plane.
bool EpdGreyPlanes(const uint8_t* grey, size_t grey_bytes, uint8_t* ram24,
                   uint8_t* ram26);

}  // namespace kid_app
