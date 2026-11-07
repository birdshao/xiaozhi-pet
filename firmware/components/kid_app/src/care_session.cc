#include "kid_app/care_session.h"

#include "kid_app/selftest_clock.h"

#include <algorithm>

namespace kid_app {
namespace {
constexpr uint16_t kTick = GameArtTone(static_cast<uint16_t>(GameArt::Heart));
constexpr uint32_t kDiaryExitMs = 45000;  // up to 7 lines are read first
}  // namespace

const char* CareKindName(CareKind kind) {
    switch (kind) {
        case CareKind::None: return "none";
        case CareKind::Feed: return "feed";
        case CareKind::Bath: return "bath";
        case CareKind::Brush: return "brush";
        case CareKind::Tickle: return "tickle";
        case CareKind::Lull: return "lull";
        case CareKind::Dress: return "dress";
        case CareKind::Habit: return "habit";
        case CareKind::Tidy: return "tidy";
        case CareKind::Diary: return "diary";
        case CareKind::Album: return "album";
    }
    return "none";
}

uint16_t FoodArt(Food food) {
    const uint8_t f = static_cast<uint8_t>(food);
    return f >= 1 && f <= kFoodCount
               ? static_cast<uint16_t>(static_cast<uint16_t>(CareArt::Fish) + f - 1)
               : 0;
}
uint16_t WardrobeArt(size_t i) {
    return i < WardrobeCount() ? static_cast<uint16_t>(static_cast<uint16_t>(CareArt::WearFirst) + i)
                               : static_cast<uint16_t>(CareArt::Lock);
}
uint16_t HabitArt(Habit habit) {
    const uint8_t h = static_cast<uint8_t>(habit);
    return h >= 1 && h <= kHabitCount ? static_cast<uint16_t>(static_cast<uint16_t>(CareArt::HabitFirst) - 1 + h)
                                      : static_cast<uint16_t>(CareArt::Lock);
}

void CareSession::DressList() {
    dress_count_ = 0;
    for (size_t i = 0; i < WardrobeCount() && dress_count_ < dress_.size(); ++i)
        if (WardrobeUnlocked(WardrobeAt(i), ctx_.stickers))
            dress_[dress_count_++] = static_cast<uint8_t>(i);
}

bool CareSession::Start(CareKind kind, const CareContext& context, uint32_t now_ms,
                        CareOutput* out) {
    if (kind == CareKind::None || !out) return false;
    *out = CareOutput{};
    kind_ = kind;
    ctx_ = context;
    active_ = true;
    reacting_ = false;
    last_input_ms_ = now_ms;
    index_ = progress_ = phase_ = hearts_ = page_ = 0;
    awaiting_ = false;
    last_dir_ = 0;
    after_ = CareKind::None;
    const Temperament t = ctx_.temperament;
    switch (kind) {
        case CareKind::Feed: out->clip = CarePhraseId(CareLine::FeedPrompt, 0, t); break;
        case CareKind::Bath: out->clip = CarePhraseId(CareLine::BathStart, 0, t); break;
        case CareKind::Brush: out->clip = CarePhraseId(CareLine::BrushStart, 0, t); break;
        case CareKind::Tickle: out->clip = CarePhraseId(CareLine::TickleStart, 0, t); break;
        case CareKind::Lull: out->clip = CarePhraseId(CareLine::LullStart, 0, t); break;
        case CareKind::Tidy:
            out->clip = CarePhraseId(CareLine::TidyStart, 0, t);
            after_ = CareKind::Diary;
            break;
        case CareKind::Habit:
            if (ctx_.habit != Habit::None && !ctx_.habit_done_today)
                out->clip = CarePhraseId(CareLine::HabitRemind, static_cast<uint8_t>(ctx_.habit), t);
            else
                out->clip = kGameTonePrompt;
            break;
        case CareKind::Dress:
            DressList();
            out->clip = kGameTonePrompt;
            break;
        case CareKind::Diary:
        case CareKind::Album:
        case CareKind::None:
            break;  // the board reads the diary (ComposeDiary); the album is quiet
    }
    out->redraw = true;
    return true;
}

uint8_t CareSession::CardCount() const {
    switch (kind_) {
        case CareKind::Feed: return kFoodCount;
        case CareKind::Dress: return dress_count_ ? dress_count_ : 1;
        case CareKind::Habit: return 1;
        default: return 0;
    }
}

uint16_t CareSession::CardArt(uint8_t i) const {
    switch (kind_) {
        case CareKind::Feed: return FoodArt(static_cast<Food>(i + 1));
        case CareKind::Dress:
            return dress_count_ ? WardrobeArt(dress_[i]) : static_cast<uint16_t>(CareArt::Lock);
        case CareKind::Habit: return HabitArt(ctx_.habit);
        default: return 0;
    }
}

CareOutput CareSession::Finish(uint16_t clip, uint32_t) {
    CareOutput out{};
    active_ = false;
    reacting_ = false;
    out.finished = true;
    out.redraw = true;
    out.clip = clip;
    return out;
}

CareOutput CareSession::Record(CareEventKind kind, MemorySource source, uint16_t slot,
                               uint16_t option) {
    CareOutput out{};
    out.record = true;
    out.event.kind = kind;
    out.event.source = source;
    out.event.slot = slot;
    out.event.option = option;
    out.redraw = true;
    return out;
}

CareOutput CareSession::Input(GameInput input, uint32_t now_ms) {
    CareOutput out{};
    if (!active_) return out;
    last_input_ms_ = now_ms;
    if (reacting_ || awaiting_) return out;  // the reaction plays to its end
    const Temperament t = ctx_.temperament;
    if (input == GameInput::Long) return Finish(0, now_ms);  // home, nothing lost

    if (kind_ == CareKind::Album) {
        if (input == GameInput::Press) return Finish(0, now_ms);
        const bool up = input == GameInput::Up;
        if (up ? page_ == 0 : page_ + 1 >= kAlbumPages) return out;
        page_ = static_cast<uint8_t>(up ? page_ - 1 : page_ + 1);
        out.redraw = true;
        out.clip = kTick;
        return out;
    }
    if (kind_ == CareKind::Brush) {
        if (input == GameInput::Press) return out;
        const int8_t dir = input == GameInput::Up ? -1 : 1;
        // Back and forth: only a change of direction is a brush stroke, so
        // spinning the dial one way does nothing.
        if (dir == last_dir_) return out;
        last_dir_ = dir;
        if (++progress_ < kBrushStrokes) {
            out.redraw = true;
            out.clip = kTick;
            return out;
        }
        reacting_ = true;
        react_until_ms_ = now_ms + SelfTestClock::Scaled(kReactMs);
        hearts_ = 3;
        out = Record(CareEventKind::Brushed, MemorySource::Device, 0, 0);
        out.clip = CarePhraseId(CareLine::BrushDone, 0, t);
        return out;
    }
    if (kind_ == CareKind::Tidy) {
        if (input != GameInput::Press) return out;
        out.redraw = true;
        if (++progress_ < kToys) {
            out.clip = kTick;
            return out;
        }
        reacting_ = true;
        react_until_ms_ = now_ms + SelfTestClock::Scaled(kReactMs);
        hearts_ = 3;
        out.clip = CarePhraseId(CareLine::TidyDone, 0, t);
        return out;
    }
    if (kind_ == CareKind::Diary) {
        if (input != GameInput::Press) return out;
        CareOutput done = Finish(0, now_ms);
        done.next = CareKind::Lull;  // bedtime ritual: then the lullaby
        return done;
    }
    if (kind_ == CareKind::Bath || kind_ == CareKind::Tickle || kind_ == CareKind::Lull)
        return out;  // motion sessions: the dial only leaves (long press)

    // Card sessions: feed, dress, habit.
    const uint8_t n = CardCount();
    if (input == GameInput::Up || input == GameInput::Down) {
        const bool up = input == GameInput::Up;
        if (up ? index_ == 0 : index_ + 1 >= n) return out;
        index_ = static_cast<uint8_t>(up ? index_ - 1 : index_ + 1);
        out.redraw = true;
        out.clip = kTick;
        return out;
    }
    // Press.
    if (kind_ == CareKind::Feed) {
        const Food food = static_cast<Food>(index_ + 1);
        const Taste taste = TasteOf(t, food);
        hearts_ = static_cast<uint8_t>(static_cast<uint8_t>(taste) + 1);
        phase_ = 1;
        reacting_ = true;
        react_until_ms_ = now_ms + SelfTestClock::Scaled(kReactMs);
        out = Record(CareEventKind::Fed, MemorySource::DialChoice, 0, static_cast<uint16_t>(food));
        out.clip = CarePhraseId(CareLine::FoodReact, static_cast<uint8_t>(taste), t);
        return out;
    }
    if (kind_ == CareKind::Habit) {
        if (ctx_.habit == Habit::None) return Finish(0, now_ms);
        if (ctx_.habit_done_today) return Finish(CarePhraseId(CareLine::HabitAlready, 0, t), now_ms);
        // Streaks are days: without a clock the tick would be refused.
        if (!ctx_.clock_known) return Finish(kGameTonePrompt, now_ms);
        // Ask the ledger first; the check appears with its answer (Confirm).
        awaiting_ = true;
        awaiting_since_ms_ = now_ms;
        out = Record(CareEventKind::HabitDone, MemorySource::DialChoice, 0, 0);
        out.redraw = false;
        return out;
    }
    if (kind_ == CareKind::Dress) {
        if (!dress_count_) return Finish(0, now_ms);
        const WardrobeItem& w = WardrobeAt(dress_[index_]);
        const uint8_t slot = static_cast<uint8_t>(w.slot);
        // Press again on what is worn takes it off; otherwise put it on.
        const uint8_t item = ctx_.worn[slot] == w.item ? 0 : w.item;
        ctx_.worn[slot] = item;
        out = Record(CareEventKind::Dressed, MemorySource::DialChoice, slot, item);
        out.clip = item ? CarePhraseId(CareLine::DressDone, 0, t) : kTick;
        return out;  // stays open: try on the next one
    }
    return out;
}

CareOutput CareSession::OnGesture(Gesture gesture, uint32_t now_ms) {
    CareOutput out{};
    if (!active_ || reacting_ || !WantsMotion()) return out;
    const Temperament t = ctx_.temperament;
    if (gesture == Gesture::HardShake) {  // a gentle reminder, never a reward
        last_input_ms_ = now_ms;
        out.clip = CarePhraseId(CareLine::GentlePlease, 0, t);
        return out;
    }
    auto react = [&](CareEventKind kind, CareLine line) {
        hearts_ = 3;
        reacting_ = true;
        react_until_ms_ = now_ms + SelfTestClock::Scaled(kReactMs);
        CareOutput r = Record(kind, MemorySource::Device, 0, 0);
        r.clip = CarePhraseId(line, 0, t);
        return r;
    };
    switch (kind_) {
        case CareKind::Bath:
            if (phase_ == 0 && gesture == Gesture::GentleShake) {
                last_input_ms_ = now_ms;
                out.redraw = true;
                if (++progress_ >= kBathBubbles) {
                    phase_ = 1;  // enough bubbles: turn TuanTuan over to rinse
                    out.clip = CarePhraseId(CareLine::BathRinse, 0, t);
                } else {
                    out.clip = kTick;
                }
            } else if (phase_ == 1 && gesture == Gesture::FaceDown) {
                last_input_ms_ = now_ms;
                return react(CareEventKind::Bathed, CareLine::BathDone);
            }
            break;
        case CareKind::Tickle:
            if (gesture == Gesture::GentleShake) {
                last_input_ms_ = now_ms;
                if (++progress_ >= kTickles) return react(CareEventKind::Tickled, CareLine::TickleDone);
                out.redraw = true;
                out.clip = kTick;
            }
            break;
        case CareKind::Lull:
            if (gesture == Gesture::FaceDown) {
                last_input_ms_ = now_ms;
                progress_ = kLullStillWindows;
                return react(CareEventKind::Lulled, CareLine::LullDone);
            }
            break;
        default:
            break;
    }
    return out;
}

CareOutput CareSession::OnWindow(bool still, uint32_t now_ms) {
    CareOutput out{};
    if (!active_ || reacting_ || kind_ != CareKind::Lull) return out;
    if (!still) {
        if (progress_) {
            progress_ = 0;
            out.redraw = true;
        }
        return out;
    }
    last_input_ms_ = now_ms;  // resting is what the lullaby asks for
    if (++progress_ < kLullStillWindows) {
        out.redraw = (progress_ % 2) == 0;  // a dot every second, not every window
        return out;
    }
    hearts_ = 3;
    reacting_ = true;
    react_until_ms_ = now_ms + SelfTestClock::Scaled(kReactMs);
    out = Record(CareEventKind::Lulled, MemorySource::Device, 0, 0);
    out.clip = CarePhraseId(CareLine::LullDone, 0, ctx_.temperament);
    return out;
}

CareOutput CareSession::Confirm(bool committed, uint32_t now_ms) {
    CareOutput out{};
    if (!active_ || !awaiting_ || kind_ != CareKind::Habit) return out;
    awaiting_ = false;
    const Temperament t = ctx_.temperament;
    if (!committed) return Finish(CarePhraseId(CareLine::HabitAlready, 0, t), now_ms);
    ctx_.habit_done_today = true;
    phase_ = 1;
    hearts_ = 3;
    reacting_ = true;
    react_until_ms_ = now_ms + SelfTestClock::Scaled(kReactMs);
    out.redraw = true;
    out.clip = CarePhraseId(CareLine::HabitTicked, 0, t);
    return out;
}

CareOutput CareSession::Tick(uint32_t now_ms) {
    CareOutput out{};
    if (!active_) return out;
    if (awaiting_) {  // the ledger never answered: no praise, back home
        if (static_cast<int32_t>(now_ms - (awaiting_since_ms_ + kConfirmMs)) < 0) return out;
        awaiting_ = false;
        return Finish(0, now_ms);
    }
    if (reacting_) {
        if (static_cast<int32_t>(now_ms - react_until_ms_) < 0) return out;
        out = Finish(0, now_ms);
        out.sleep = kind_ == CareKind::Lull;
        out.next = after_;
        return out;
    }
    const uint32_t limit = kind_ == CareKind::Diary ? kDiaryExitMs : kIdleExitMs;
    if (static_cast<int32_t>(now_ms - (last_input_ms_ + SelfTestClock::Scaled(limit))) >= 0) {
        out = Finish(0, now_ms);
        // Bedtime: the diary goes on to the lullaby; a lullaby nobody
        // finished still ends in bed (the child put TuanTuan down).
        if (kind_ == CareKind::Diary) out.next = CareKind::Lull;
        out.sleep = kind_ == CareKind::Lull;
        return out;
    }
    return out;
}

CareView CareSession::View() const {
    CareView v{};
    if (!active_) return v;
    v.kind = kind_;
    v.progress = progress_;
    v.phase = phase_;
    v.hearts = reacting_ ? hearts_ : 0;
    v.worn = ctx_.worn;
    switch (kind_) {
        case CareKind::Bath: v.goal = kBathBubbles; break;
        case CareKind::Brush: v.goal = kBrushStrokes; break;
        case CareKind::Tickle: v.goal = kTickles; break;
        case CareKind::Lull: v.goal = kLullStillWindows; break;
        case CareKind::Tidy: v.goal = kToys; break;
        case CareKind::Album:
            v.page = page_;
            v.pages = kAlbumPages;
            v.stickers = ctx_.stickers;
            break;
        case CareKind::Diary:
            v.food = ctx_.diary.last_food;
            v.book = ctx_.diary.books != 0;
            v.game = ctx_.diary.games != 0;
            v.habit_done = ctx_.diary.habit_done;
            v.cares = CareBucket(ctx_.diary.cares);
            break;
        case CareKind::Habit: v.phase = ctx_.habit_done_today ? 1 : 0; break;
        default: break;
    }
    const uint8_t n = CardCount();
    if (n) {
        const uint8_t shown = std::min<uint8_t>(n, 3);
        const uint8_t first = index_ == 0 ? 0 : std::min<uint8_t>(index_ - 1, n - shown);
        v.option_count = shown;
        v.highlight = static_cast<uint8_t>(index_ - first);
        for (uint8_t i = 0; i < shown; ++i) v.options[i] = CardArt(static_cast<uint8_t>(first + i));
    }
    return v;
}

}  // namespace kid_app
