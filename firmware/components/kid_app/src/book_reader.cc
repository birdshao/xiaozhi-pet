#include "kid_app/book_reader.h"

#include "kid_app/memory_store.h"

namespace kid_app {
namespace {

constexpr uint32_t kPcmBytesPerMs = 48;  // 24 kHz mono PCM16
constexpr uint8_t kOverlayStride = 6;    // per question: 2 x 3 options

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
    for (uint16_t number = 1; number <= outline.pages; ++number) {
        ContentPage page{};
        if (!reader.Page(number, &page) ||
            page.question_count > ContentPackReader::kMaxQuestionsPerPage)
            return false;
        BookPageInfo& info = outline.page[number - 1];
        info.grey = page.has_grey;
        info.question_count = page.question_count;
        info.narration_ms = page.pcm.bytes / kPcmBytesPerMs;
        for (uint8_t q = 0; q < page.question_count; ++q) {
            ContentQuestion question{};
            if (!reader.Question(number, q, &question) || question.option_count < 2 ||
                question.option_count > ContentPackReader::kMaxOptions)
                return false;
            BookQuestionInfo& qi = info.questions[q];
            qi.option_count = question.option_count;
            qi.correct = question.correct;
            qi.like_slot = question.like_slot;
            for (uint8_t o = 0; o < question.option_count; ++o) {
                qi.option_ids[o] = question.options[o].id;
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
    scene.emotion = end ? Emotion::Happy : Emotion::Neutral;
    uint16_t overlay = 0;
    if (view.question)
        overlay = static_cast<uint16_t>(1 + (view.question - 1) * kOverlayStride +
                                        (view.marked ? 3 : 0) + view.highlight);
    scene.book_page = static_cast<uint16_t>((overlay << 8) | (view.page & 0xFF));
    return scene;
}

BookView BookViewOf(const SceneModel& scene) {
    BookView view{};
    view.page = static_cast<uint16_t>(scene.book_page & 0xFF);
    const uint16_t overlay = static_cast<uint16_t>(scene.book_page >> 8);
    if (overlay) {
        const uint16_t code = overlay - 1;
        view.question = static_cast<uint8_t>(1 + code / kOverlayStride);
        view.marked = (code % kOverlayStride) >= 3;
        view.highlight = static_cast<uint8_t>(code % 3);
    }
    return view;
}

void BookReader::Open(const BookOutline* outline, uint32_t now_ms, BookMissing missing) {
    (void)now_ms;
    *this = BookReader{};
    outline_ = outline;
    phase_ = Phase::Page;
    view_ = BookView{};
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

BookStep BookReader::OnInput(InputKind kind, uint32_t now_ms) {
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
            if (kind == InputKind::DialUp && pages) return GoTo(pages);
            if (kind == InputKind::DialPress && pages) return GoTo(1);
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
        if (view_.page < pages) return GoTo(static_cast<uint16_t>(view_.page + 1));
        if (!pages) return step;
        step = GoTo(static_cast<uint16_t>(pages + 1));
        phase_ = Phase::End;
        if (!finished_ && outline_->kpb2 && outline_->content_id) {
            finished_ = true;
            step.memory.kind = BookMemoryKind::Finished;
        }
        return step;
    }
    if (kind == InputKind::DialUp) {
        if (view_.page > 1) return GoTo(static_cast<uint16_t>(view_.page - 1));
        return step;
    }
    if (kind == InputKind::DialPress && narrated_ && PageInfo()) {
        // "Read again": restart this page's narration; an unanswered
        // question waits for the new reading to finish.
        step.clip = {BookClipKind::Page, view_.page, 0, 0};
        ArmQuestion(now_ms);
    }
    return step;
}

BookStep BookReader::OnShown(const BookView& view, uint32_t now_ms) {
    BookStep step{};
    if (!Active() || view != view_) return step;  // a newer view is on its way
    if (phase_ == Phase::Page && !narrated_ && PageInfo()) {
        narrated_ = true;
        step.clip = {BookClipKind::Page, view_.page, 0, 0};
        ArmQuestion(now_ms);
    } else if (phase_ == Phase::Question && !prompted_) {
        prompted_ = true;
        step.clip = {BookClipKind::Prompt, view_.page,
                     static_cast<uint8_t>(view_.question - 1), 0};
    }
    return step;
}

BookStep BookReader::Tick(uint32_t now_ms) {
    BookStep step{};
    if (phase_ == Phase::Page && question_due_ &&
        static_cast<int32_t>(now_ms - question_at_ms_) >= 0)
        return Ask(next_question_);
    if (phase_ == Phase::Reaction &&
        static_cast<int32_t>(now_ms - reaction_until_ms_) >= 0) {
        ++next_question_;
        const BookPageInfo* info = PageInfo();
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
