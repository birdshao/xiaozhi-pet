#include "kid_app/book_reader.h"

#include "kid_app/memory_store.h"

namespace kid_app {
namespace {

constexpr uint32_t kPcmBytesPerMs = 48;  // 24 kHz mono PCM16
constexpr uint8_t kOverlayStride = 6;    // per question: 2 x 3 options
constexpr uint16_t kHintBit = 0x40;      // overlay byte: next-book hint
constexpr uint16_t kBedtimeBit = 0x80;   // overlay byte: bedtime mode
constexpr uint16_t kOverlayMask = 0x3F;
constexpr uint8_t kNoQuestion = 0xFF;

}  // namespace

uint16_t BookOutline::QuestionTotal() const {
    uint16_t n = 0;
    for (uint16_t p = 0; p < pages && p < page.size(); ++p)
        n = static_cast<uint16_t>(n + page[p].question_count);
    return n;
}

uint16_t BookOutline::GreyPages() const {
    uint16_t n = 0;
    for (uint16_t p = 0; p < pages && p < page.size(); ++p)
        n = static_cast<uint16_t>(n + (page[p].grey ? 1 : 0));
    return n;
}

bool BuildBookOutline(ContentPackReader& reader, BookOutline* out) {
    if (!out) return false;
    *out = BookOutline{};
    if (!reader.Ready() || reader.PageCount() == 0 ||
        reader.PageCount() > ContentPackReader::kMaxPages)
        return false;
    BookOutline outline{};
    outline.pages = reader.PageCount();
    outline.kpb2 = reader.Format() == PackFormat::Kpb2;
    outline.content_id = reader.ContentId();
    outline.branching = reader.Branching();
    outline.series_id = reader.SeriesId();
    outline.series_index = reader.SeriesIndex();
    outline.series_count = reader.SeriesCount();
    outline.series_next = reader.SeriesNext();
    for (uint16_t number = 1; number <= outline.pages; ++number) {
        ContentPage page{};
        if (!reader.Page(number, &page) ||
            page.question_count > ContentPackReader::kMaxQuestionsPerPage)
            return false;
        BookPageInfo& info = outline.page[number - 1];
        info.grey = page.has_grey;
        info.question_count = page.question_count;
        info.narration_ms = page.pcm.bytes / kPcmBytesPerMs;
        info.next = page.next;
        for (uint8_t q = 0; q < page.question_count; ++q) {
            ContentQuestion question{};
            if (!reader.Question(number, q, &question) || question.option_count < 2 ||
                question.option_count > ContentPackReader::kMaxOptions)
                return false;
            BookQuestionInfo& qi = info.questions[q];
            qi.option_count = question.option_count;
            qi.correct = question.correct;
            qi.like_slot = question.like_slot;
            if (question.IsBranch()) {
                qi.branch = true;
                info.branch_q = q;
            }
            for (uint8_t o = 0; o < question.option_count; ++o) {
                qi.option_ids[o] = question.options[o].id;
                qi.goto_page[o] = question.options[o].goto_page;
                if (question.options[o].counts_as_like)
                    qi.like_mask = static_cast<uint8_t>(qi.like_mask | (1u << o));
            }
        }
    }
    *out = outline;
    return true;
}

SceneModel BookScene(const BookView& view, bool end) {
    SceneModel scene{};
    scene.kind = SceneKind::Reader;
    // The end picture is the reader placeholder with a happy TuanTuan.
    scene.emotion = end ? (view.bedtime ? Emotion::Sleepy : Emotion::Happy) : Emotion::Neutral;
    uint16_t overlay = 0;
    if (view.question)
        overlay = static_cast<uint16_t>(1 + (view.question - 1) * kOverlayStride +
                                        (view.marked ? 3 : 0) + view.highlight);
    if (view.next_hint) overlay = static_cast<uint16_t>(overlay | kHintBit);
    if (view.bedtime) overlay = static_cast<uint16_t>(overlay | kBedtimeBit);
    scene.book_page = static_cast<uint16_t>((overlay << 8) | (view.page & 0xFF));
    return scene;
}

BookView BookViewOf(const SceneModel& scene) {
    BookView view{};
    view.page = static_cast<uint16_t>(scene.book_page & 0xFF);
    const uint16_t flags = static_cast<uint16_t>(scene.book_page >> 8);
    view.bedtime = (flags & kBedtimeBit) != 0;
    view.next_hint = (flags & kHintBit) != 0;
    const uint16_t overlay = flags & kOverlayMask;
    if (overlay) {
        const uint16_t code = overlay - 1;
        view.question = static_cast<uint8_t>(1 + code / kOverlayStride);
        view.marked = (code % kOverlayStride) >= 3;
        view.highlight = static_cast<uint8_t>(code % 3);
    }
    return view;
}

void BedtimeTone(uint8_t* grey, size_t bytes) {
    if (!grey) return;
    for (size_t i = 0; i < bytes; ++i) {
        uint8_t out = 0;
        for (unsigned shift = 0; shift < 8; shift += 2) {
            uint8_t level = static_cast<uint8_t>((grey[i] >> shift) & 3u);
            level = level < 1 ? 1 : (level > 2 ? 2 : level);
            out = static_cast<uint8_t>(out | (level << shift));
        }
        grey[i] = out;
    }
}

uint16_t BookReader::NextBookId() const {
    return phase_ == Phase::End && outline_ ? outline_->series_next : 0;
}

void BookReader::Open(const BookOutline* outline, uint32_t now_ms, BookMissing missing,
                      bool bedtime) {
    (void)now_ms;
    *this = BookReader{};
    outline_ = outline;
    phase_ = Phase::Page;
    view_ = BookView{};
    bedtime_ = bedtime && outline && outline->pages != 0;
    view_.bedtime = bedtime_;
    if (!outline || outline->pages == 0) {
        phase_ = Phase::Missing;
        missing_ = missing == BookMissing::None ? BookMissing::NoBook : missing;
        view_.page = 0;
    }
}

const char* BookMissingName(BookMissing reason) {
    switch (reason) {
        case BookMissing::NoCard: return "no_card";
        case BookMissing::NoBook: return "no_book";
        case BookMissing::BadBook: return "bad_book";
        case BookMissing::None: break;
    }
    return "none";
}

void BookReader::Close() { *this = BookReader{}; }

const BookPageInfo* BookReader::PageInfo() const {
    if (!outline_ || view_.page == 0 || view_.page > outline_->pages) return nullptr;
    return &outline_->page[view_.page - 1];
}

BookStep BookReader::GoTo(uint16_t page) {
    phase_ = Phase::Page;
    view_ = BookView{};
    view_.page = page;
    view_.bedtime = bedtime_;
    auto_due_ = false;
    end_sleep_armed_ = false;
    narrated_ = false;
    prompted_ = false;
    next_question_ = 0;
    question_due_ = false;
    BookStep step{};
    step.publish = true;
    step.clip.kind = BookClipKind::Stop;  // the old page stops at once
    return step;
}

BookStep BookReader::Ask(uint8_t question) {
    phase_ = Phase::Question;
    question_due_ = false;
    prompted_ = false;
    view_.question = static_cast<uint8_t>(question + 1);
    view_.highlight = 0;
    view_.marked = false;
    choice_.Reset(PageInfo()->questions[question].option_count, 0);
    BookStep step{};
    step.publish = true;
    step.clip.kind = BookClipKind::Stop;  // a "read again" must not talk over it
    return step;
}

void BookReader::ArmQuestion(uint32_t now_ms) {
    const BookPageInfo* info = PageInfo();
    question_due_ = info && next_question_ < info->question_count;
    if (question_due_) question_at_ms_ = now_ms + info->narration_ms + kQuestionDelayMs;
}

void BookReader::PushHistory(uint16_t page) {
    if (history_n_ == history_.size()) {  // cannot happen in a valid book
        for (size_t i = 1; i < history_.size(); ++i) history_[i - 1] = history_[i];
        --history_n_;
    }
    history_[history_n_++] = static_cast<uint8_t>(page);
}

BookReader::Target BookReader::NextTarget() const {
    const uint16_t pages = outline_ ? outline_->pages : 0;
    const BookPageInfo* info = PageInfo();
    const uint16_t raw = info ? info->next : 0;
    if (raw == ContentPackReader::kNextEnd) return {0, false};
    if (raw == ContentPackReader::kNextRestart) return {1, true};
    if (raw != 0) return {raw, false};
    return {view_.page < pages ? static_cast<uint16_t>(view_.page + 1) : uint16_t{0}, false};
}

BookReader::Target BookReader::OptionTarget(const BookQuestionInfo& info,
                                            uint8_t option) const {
    const uint8_t go = option < info.goto_page.size() ? info.goto_page[option] : 0;
    if (go == ContentPackReader::kGotoRestart) return {1, true};
    return {go, false};
}

BookStep BookReader::Advance(Target target, uint32_t now_ms) {
    if (target.restart) {
        history_n_ = 0;
        return GoTo(1);
    }
    if (target.page == 0) return Finish(now_ms);
    PushHistory(view_.page);
    return GoTo(target.page);
}

BookStep BookReader::Retreat() {
    BookStep none{};
    if (history_n_ == 0) return none;
    return GoTo(history_[--history_n_]);
}

BookStep BookReader::Finish(uint32_t now_ms) {
    PushHistory(view_.page);
    BookStep step = GoTo(static_cast<uint16_t>(outline_->pages + 1));
    phase_ = Phase::End;
    view_.next_hint = outline_->series_next != 0;
    if (bedtime_) {
        end_sleep_armed_ = true;
        end_sleep_at_ms_ = now_ms + kBedtimeYawnMs;
    }
    if (!finished_ && outline_->kpb2 && outline_->content_id) {
        finished_ = true;
        step.memory.kind = BookMemoryKind::Finished;
    }
    return step;
}

// "Next" from a plain page. On a branch page the dial asks the choice at
// once (the story cannot go on without it); in bedtime mode nobody is
// asked, the story follows the first option.
BookStep BookReader::Forward(uint32_t now_ms) {
    const BookPageInfo* info = PageInfo();
    if (info && info->branch_q != kNoQuestion) {
        if (!bedtime_ && next_question_ < info->question_count) return Ask(next_question_);
        return Advance(OptionTarget(info->questions[info->branch_q], 0), now_ms);
    }
    return Advance(NextTarget(), now_ms);
}

BookStep BookReader::OnInput(InputKind kind, uint32_t now_ms) {
    // Turning the dial during a bedtime story means "normal mode, please":
    // the turn still does what it always does.
    bool left = false;
    if (bedtime_ && Active() && phase_ != Phase::Missing &&
        (kind == InputKind::DialUp || kind == InputKind::DialDown)) {
        bedtime_ = false;
        view_.bedtime = false;
        auto_due_ = false;
        end_sleep_armed_ = false;
        left = true;
    }
    BookStep step = OnInputCore(kind, now_ms);
    if (left) {
        step.bedtime_off = true;
        // Same page, still narrating: its questions are asked after all.
        if (phase_ == Phase::Page && narrated_ && !step.publish) ArmQuestion(now_ms);
    }
    return step;
}

BookStep BookReader::OnInputCore(InputKind kind, uint32_t now_ms) {
    BookStep step{};
    if (!Active()) return step;
    if (kind == InputKind::DialLong) {
        step.home = true;
        step.clip.kind = BookClipKind::Stop;
        return step;
    }
    const uint16_t pages = outline_ ? outline_->pages : 0;
    switch (phase_) {
        case Phase::Closed:
        case Phase::Reaction:  // the answer is being shown: wait for it
            return step;
        case Phase::Missing:  // nothing to read: a press also goes home
            if (kind == InputKind::DialPress) {
                step.home = true;
                step.clip.kind = BookClipKind::Stop;
            }
            return step;
        case Phase::End:
            if (kind == InputKind::DialUp && pages) return Retreat();
            if (kind == InputKind::DialPress && pages) {
                history_n_ = 0;
                return GoTo(1);
            }
            return step;
        case Phase::Question: {
            if (kind == InputKind::DialUp || kind == InputKind::DialDown) {
                if (!choice_.Move(kind == InputKind::DialUp ? -1 : +1)) return step;
                view_.highlight = choice_.Index();
                step.publish = true;
                step.clip = {BookClipKind::Option, view_.page,
                             static_cast<uint8_t>(view_.question - 1), view_.highlight};
                return step;
            }
            if (kind != InputKind::DialPress) return step;
            const uint8_t q = static_cast<uint8_t>(view_.question - 1);
            const BookQuestionInfo& info = PageInfo()->questions[q];
            const uint8_t chosen = choice_.Index();
            step.chosen = chosen;
            uint8_t shown = chosen;
            if (info.correct == 0) {
                step.answer = BookAnswer::Choice;
            } else if (info.correct == chosen + 1u) {
                step.answer = BookAnswer::Right;
            } else {
                // Gentle: show and say the right one, no "wrong" sound.
                step.answer = BookAnswer::Wrong;
                shown = static_cast<uint8_t>(info.correct - 1);
            }
            // ADR-012: the only like write, from this dial press.
            if (info.like_slot != 0 && (info.like_mask & (1u << chosen)))
                step.memory = {BookMemoryKind::Like, view_.page, q, chosen};
            // A branch question: the story follows the pick once it is shown.
            if (info.branch) branch_target_ = OptionTarget(info, chosen);
            view_.highlight = shown;
            view_.marked = true;
            phase_ = Phase::Reaction;
            reaction_until_ms_ = now_ms + kReactionMs;
            step.publish = true;
            step.clip = {BookClipKind::Option, view_.page, q, shown};
            return step;
        }
        case Phase::Page:
            break;
    }
    if (kind == InputKind::DialDown) {
        if (!pages) return step;
        return Forward(now_ms);
    }
    if (kind == InputKind::DialUp) return Retreat();
    if (kind == InputKind::DialPress && narrated_ && PageInfo()) {
        // "Read again": restart this page's narration; an unanswered
        // question waits for the new reading to finish (bedtime: the page
        // turns after it).
        step.clip = {BookClipKind::Page, view_.page, 0, 0};
        if (bedtime_) {
            auto_due_ = true;
            auto_at_ms_ = now_ms + PageInfo()->narration_ms + kBedtimePauseMs;
        } else {
            ArmQuestion(now_ms);
        }
    }
    return step;
}

BookStep BookReader::OnShown(const BookView& view, uint32_t now_ms) {
    BookStep step{};
    if (!Active() || view != view_) return step;  // a newer view is on its way
    if (phase_ == Phase::Page && !narrated_ && PageInfo()) {
        narrated_ = true;
        step.clip = {BookClipKind::Page, view_.page, 0, 0};
        if (bedtime_) {
            auto_due_ = true;
            auto_at_ms_ = now_ms + PageInfo()->narration_ms + kBedtimePauseMs;
        } else {
            ArmQuestion(now_ms);
        }
    } else if (phase_ == Phase::Question && !prompted_) {
        prompted_ = true;
        step.clip = {BookClipKind::Prompt, view_.page,
                     static_cast<uint8_t>(view_.question - 1), 0};
    }
    return step;
}

BookStep BookReader::Tick(uint32_t now_ms) {
    BookStep step{};
    if (bedtime_) {
        if (phase_ == Phase::Page && auto_due_ &&
            static_cast<int32_t>(now_ms - auto_at_ms_) >= 0) {
            auto_due_ = false;
            return Forward(now_ms);
        }
        if (phase_ == Phase::End && end_sleep_armed_ &&
            static_cast<int32_t>(now_ms - end_sleep_at_ms_) >= 0) {
            end_sleep_armed_ = false;
            step.sleep = true;  // TuanTuan yawned: the normal sleep flow takes over
            return step;
        }
    }
    if (phase_ == Phase::Page && question_due_ &&
        static_cast<int32_t>(now_ms - question_at_ms_) >= 0)
        return Ask(next_question_);
    if (phase_ == Phase::Reaction &&
        static_cast<int32_t>(now_ms - reaction_until_ms_) >= 0) {
        const BookPageInfo* info = PageInfo();
        if (info && info->branch_q != kNoQuestion && view_.question - 1 == info->branch_q)
            return Advance(branch_target_, now_ms);
        ++next_question_;
        if (info && next_question_ < info->question_count) return Ask(next_question_);
        // Back to the plain page; it was read already, so it stays quiet.
        phase_ = Phase::Page;
        view_.question = 0;
        view_.highlight = 0;
        view_.marked = false;
        step.publish = true;
    }
    return step;
}

bool BookMemoryEvent(const BookOutline& outline, const BookMemoryRequest& request,
                     uint64_t event_id, uint32_t day, MemoryEvent* out) {
    if (!out || event_id == 0) return false;
    if (request.kind == BookMemoryKind::Finished) {
        if (!outline.kpb2 || outline.content_id == 0) return false;
        MemoryEvent e{};
        e.id = event_id;
        e.kind = MemoryEventKind::BookFinished;
        e.source = MemorySource::Device;
        e.day = day;
        e.option = outline.content_id;
        *out = e;
        return true;
    }
    if (request.kind != BookMemoryKind::Like || request.page == 0 ||
        request.page > outline.pages)
        return false;
    const BookPageInfo& page = outline.page[request.page - 1];
    if (request.question >= page.question_count) return false;
    const BookQuestionInfo& info = page.questions[request.question];
    ContentQuestion q{};
    q.page = request.page;
    q.index = request.question;
    q.option_count = info.option_count;
    q.correct = info.correct;
    q.like_slot = info.like_slot;
    for (uint8_t o = 0; o < info.option_count && o < q.options.size(); ++o) {
        q.options[o].id = info.option_ids[o];
        q.options[o].counts_as_like = (info.like_mask & (1u << o)) != 0;
    }
    return ChoiceMemoryEvent(q, request.option, event_id, day, out);
}

bool ComposeBookOverlay(ContentPackReader& reader, const BookView& view,
                        MonoFrame& frame) {
    ContentQuestion q{};
    if (!view.question || !reader.Question(view.page, view.question - 1, &q) ||
        view.highlight >= q.option_count)
        return false;
    for (uint8_t o = 0; o < q.option_count; ++o) {
        if (!reader.BlitOptionIcon(q, o, frame)) return false;
        const ChoiceRing style = o != view.highlight ? ChoiceRing::Plain
                                 : view.marked       ? ChoiceRing::Chosen
                                                     : ChoiceRing::Highlight;
        DrawChoiceRing(frame, HighlightRect(q.options[o]),
                       static_cast<uint8_t>(ContentPackReader::kHighlightMargin), style);
    }
    return true;
}

bool ForceBookOverlay(ContentPackReader& reader, const BookView& view,
                      const MonoFrame& next, MonoFrame& previous) {
    ContentQuestion q{};
    if (!view.question || !reader.Question(view.page, view.question - 1, &q)) return false;
    for (uint8_t o = 0; o < q.option_count; ++o)
        OpposeRect(previous, next, HighlightRect(q.options[o]));
    return true;
}

bool EpdGreyPlanes(const uint8_t* grey, size_t grey_bytes, uint8_t* ram24,
                   uint8_t* ram26) {
    if (!SplitGreyPlanes(grey, grey_bytes, ram26, ram24)) return false;
    for (size_t i = 0; i < grey_bytes / 2; ++i) {
        ram24[i] = static_cast<uint8_t>(~ram24[i]);
        ram26[i] = static_cast<uint8_t>(~ram26[i]);
    }
    return true;
}

}  // namespace kid_app
