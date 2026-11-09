#include "kid_app/care_session.h"

#include "kid_app/selftest_clock.h"

#include <algorithm>

namespace kid_app {
namespace {
constexpr uint16_t kTick = GameArtTone(static_cast<uint16_t>(GameArt::Heart));
constexpr uint32_t kDiaryExitMs = 45000;  // up to 7 lines are read first
constexpr uint32_t kDiaryGrowMs = 8000;     // V9 C3: one more line to read
constexpr uint32_t kBedsongExitMs = 30000;  // V9 S4: no answer -> straight to bed

// 32-bit integer mix (lowbias32): a fixed, portable hash for the daily
// variation. Never a random source: the same day and temperament always
// give the same bath, the same knots and the same itches.
uint32_t Mix(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// Growth memory id (GAME_COMPLETED option) -> menu card. 1..5 are the V4
// games (mini_game.h / mic_game.h / motion_game.h), 21..39 the V8 games
// (play_content.cc PlayGameInfo::id; test_care_v9 checks the two agree).
struct GameToy {
    uint8_t game_id;
    GameChoice card;
};
constexpr GameToy kGameToys[] = {
    {1, GameChoice::Quiz},       {2, GameChoice::Candle},     {3, GameChoice::Clap},
    {4, GameChoice::Ball},       {5, GameChoice::Peekaboo},   {21, GameChoice::Memory4},
    {22, GameChoice::Memory6},   {23, GameChoice::Maze},      {24, GameChoice::Dice},
    {25, GameChoice::Tree},      {26, GameChoice::Statue},    {27, GameChoice::DayNight},
    {28, GameChoice::Shapes},    {29, GameChoice::Counting},  {31, GameChoice::SpotDiff},
    {32, GameChoice::MamaBaby},  {33, GameChoice::SizeOrder}, {34, GameChoice::Pattern},
    {35, GameChoice::Classify},  {36, GameChoice::Shadow},    {37, GameChoice::LeftRight},
    {38, GameChoice::Clock},     {39, GameChoice::Weather},
};
}  // namespace

uint32_t CareSeed(uint32_t day, Temperament t) {
    const uint32_t s = Mix(day * 3u + static_cast<uint32_t>(t) + 0x9e3779b9u);
    return s ? s : 1u;
}

uint8_t CheerArg(uint32_t day) { return day ? static_cast<uint8_t>(day % 3 + 1) : 0; }

uint8_t BathSpots(uint8_t dirty_level) {
    constexpr uint8_t kSpots[4] = {2, 2, 3, 5};
    return kSpots[dirty_level > 3 ? 3 : dirty_level];
}

uint8_t SpotMask(uint32_t seed, uint8_t count) {
    if (count > kCareSpotSlots) count = kCareSpotSlots;
    uint8_t mask = 0, have = 0;
    uint32_t r = seed;
    while (have < count) {
        r = Mix(r + 0x6d2b79f5u);
        const uint8_t bit = static_cast<uint8_t>(1u << (r % kCareSpotSlots));
        if (mask & bit) continue;
        mask = static_cast<uint8_t>(mask | bit);
        ++have;
    }
    return mask;
}

uint8_t BrushKnots(uint32_t seed) { return static_cast<uint8_t>(2 + Mix(seed ^ 0xb5u) % 3); }

uint8_t TickleTarget(uint32_t seed, uint8_t round) {
    constexpr uint8_t kOrders[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2},
                                       {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
    return kOrders[Mix(seed ^ 0x71u) % 6][round % 3];
}

uint8_t TicklishSpot(Temperament t) {
    switch (t) {
        case Temperament::Curious: return kTickleFeet;
        case Temperament::Gentle: return kTickleBelly;
        case Temperament::Lively: return kTickleHead;  // the ears
    }
    return kTickleBelly;
}

Food TodayWish(uint32_t day, Temperament t, const Season& season) {
    if (!day) return Food::None;
    Food menu[kAllFoodCount];
    uint8_t n = 0;
    for (uint8_t f = 1; f <= kFoodCount; ++f) menu[n++] = static_cast<Food>(f);
    if (season.dumpling) menu[n++] = Food::Dumpling;
    if (season.treat != Treat::None) menu[n++] = Food::FestiveTreat;
    return menu[Mix(CareSeed(day, t) ^ 0xf00du) % n];
}

uint8_t WishPicture(Food food, Treat treat) {
    if (food == Food::FestiveTreat) return treat == Treat::Mooncake ? 9 : 8;
    const uint8_t f = static_cast<uint8_t>(food);
    return f <= static_cast<uint8_t>(Food::Dumpling) ? f : 0;
}

uint16_t GameToyArt(uint16_t game_id) {
    for (const GameToy& g : kGameToys) {
        if (g.game_id != game_id) continue;
        const Activity* a = FindActivity(g.card);
        return a ? a->art : 0;
    }
    return 0;
}

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
        case CareKind::Bedsong: return "bedsong";
    }
    return "none";
}

uint16_t FoodArt(Food food) {
    const uint8_t f = static_cast<uint8_t>(food);
    return f >= 1 && f <= kAllFoodCount
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
    // V9 C1: a dated session varies by day; no date keeps the V8 flow
    // (self-test scripts and a board whose clock is not set).
    seed_ = ctx_.day ? CareSeed(ctx_.day, t) : 0;
    goal_ = 0;
    variant_ = 0;
    feather_ = 0;
    laugh_ = false;
    switch (kind) {
        case CareKind::Feed:
            // V9 C4: the seasonal foods first (out of season: the V8 six).
            menu_count_ = 0;
            if (ctx_.season.dumpling) menu_[menu_count_++] = static_cast<uint8_t>(Food::Dumpling);
            if (ctx_.season.treat != Treat::None)
                menu_[menu_count_++] = static_cast<uint8_t>(Food::FestiveTreat);
            for (uint8_t f = 1; f <= kFoodCount; ++f) menu_[menu_count_++] = f;
            out->clip = CarePhraseId(CareLine::FeedPrompt, 0, t);
            break;
        case CareKind::Bath:
            goal_ = seed_ ? BathSpots(ctx_.dirty) : kBathBubbles;
            if (seed_) variant_ = SpotMask(seed_, goal_);
            out->clip = CarePhraseId(CareLine::BathStart, 0, t);
            break;
        case CareKind::Brush:
            if (seed_) {
                const uint8_t knots = BrushKnots(seed_);
                variant_ = SpotMask(seed_ ^ 0x5au, knots);
                goal_ = static_cast<uint8_t>(knots * 2);
            } else {
                goal_ = kBrushStrokes;
            }
            out->clip = CarePhraseId(CareLine::BrushStart, 0, t);
            break;
        case CareKind::Tickle:
            if (seed_) {
                // The feather waits on the belly (or, when the first itch is
                // there, on the head or the feet): one dial step away.
                const uint8_t first = TickleTarget(seed_, 0);
                feather_ = first != kTickleBelly ? kTickleBelly
                                                 : ((seed_ & 1u) ? kTickleHead : kTickleFeet);
                out->clip = CarePhraseId(CareLine::TickleSeek, 0, t);
            } else {
                out->clip = CarePhraseId(CareLine::TickleStart, 0, t);
            }
            break;
        case CareKind::Bedsong: out->clip = CarePhraseId(CareLine::SongAsk, 0, t); break;
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
        case CareKind::Feed: return menu_count_;
        case CareKind::Dress: return dress_count_ ? dress_count_ : 1;
        case CareKind::Habit: return 1;
        case CareKind::Bedsong: return 2;
        default: return 0;
    }
}

uint16_t CareSession::CardArt(uint8_t i) const {
    switch (kind_) {
        case CareKind::Feed: return FoodArt(static_cast<Food>(menu_[i]));
        case CareKind::Dress:
            return dress_count_ ? WardrobeArt(dress_[i]) : static_cast<uint16_t>(CareArt::Lock);
        case CareKind::Habit: return HabitArt(ctx_.habit);
        case CareKind::Bedsong:  // the moon with a note, or TuanTuan asleep
            return i == 0 ? static_cast<uint16_t>(SoundArt::Lullaby)
                          : static_cast<uint16_t>(PlayArt::Sleep);
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
        if (++progress_ < goal_) {
            out.redraw = true;
            out.clip = kTick;
            return out;
        }
        reacting_ = true;
        react_until_ms_ = now_ms + SelfTestClock::Scaled(kReactMs);
        hearts_ = 3;
        out = Record(CareEventKind::Brushed, MemorySource::Device, 0, 0);
        out.clip = CarePhraseId(CareLine::BrushDone, CheerArg(ctx_.day), t);
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
        return AfterDiary(now_ms);  // bedtime ritual: a lullaby, then to sleep
    }
    if (kind_ == CareKind::Tickle && seed_ &&
        (input == GameInput::Up || input == GameInput::Down)) {
        // V9: the dial carries the feather, head (0) .. feet (2).
        const bool up = input == GameInput::Up;
        if (up ? feather_ == kTickleHead : feather_ == kTickleFeet) return out;
        feather_ = static_cast<uint8_t>(up ? feather_ - 1 : feather_ + 1);
        laugh_ = false;
        out.redraw = true;
        out.clip = kTick;
        return out;
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
    if (kind_ == CareKind::Bedsong) {
        if (index_ != 0) {  // "no song": TuanTuan goes to bed (the lull step)
            CareOutput done = Finish(0, now_ms);
            done.next = CareKind::Lull;
            return done;
        }
        // No spoken answer: the lullaby itself is the answer (a line here
        // would talk over the first bars).
        CareOutput done = Finish(0, now_ms);
        done.song = true;  // the board plays the lullaby, then the sleep flow
        return done;
    }
    if (kind_ == CareKind::Feed) {
        const Food food = static_cast<Food>(menu_[index_]);
        const Taste taste = TasteOf(t, food);
        const bool wished = food == ctx_.wish;  // V9 C4: "how did you know!"
        hearts_ = wished ? 3 : static_cast<uint8_t>(static_cast<uint8_t>(taste) + 1);
        phase_ = 1;
        reacting_ = true;
        react_until_ms_ = now_ms + SelfTestClock::Scaled(kReactMs);
        out = Record(CareEventKind::Fed, MemorySource::DialChoice, 0, static_cast<uint16_t>(food));
        out.clip = wished ? CarePhraseId(CareLine::WishHit, 0, t)
                          : CarePhraseId(CareLine::FoodReact, static_cast<uint8_t>(taste), t);
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
    auto react = [&](CareEventKind kind, CareLine line, uint8_t arg) {
        hearts_ = 3;
        reacting_ = true;
        react_until_ms_ = now_ms + SelfTestClock::Scaled(kReactMs);
        CareOutput r = Record(kind, MemorySource::Device, 0, 0);
        r.clip = CarePhraseId(line, arg, t);
        return r;
    };
    switch (kind_) {
        case CareKind::Bath:
            if (phase_ == 0 && gesture == Gesture::GentleShake) {
                last_input_ms_ = now_ms;
                out.redraw = true;
                if (++progress_ >= goal_) {
                    phase_ = 1;  // enough bubbles: turn TuanTuan over to rinse
                    out.clip = CarePhraseId(CareLine::BathRinse, 0, t);
                } else {
                    out.clip = kTick;
                }
            } else if (phase_ == 1 && gesture == Gesture::FaceDown) {
                last_input_ms_ = now_ms;
                return react(CareEventKind::Bathed, CareLine::BathDone, CheerArg(ctx_.day));
            }
            break;
        case CareKind::Tickle:
            if (gesture == Gesture::GentleShake && seed_) {  // V9: the feather on the itch
                last_input_ms_ = now_ms;
                const uint8_t target = TickleTarget(seed_, progress_);
                out.redraw = true;
                out.clip = kTick;
                if (feather_ != target) {  // a wiggle towards the itch, no tickle yet
                    feather_ = static_cast<uint8_t>(feather_ < target ? feather_ + 1 : feather_ - 1);
                    laugh_ = false;
                    return out;
                }
                laugh_ = target == TicklishSpot(t);  // the big laugh, a frame of its own
                if (++progress_ >= kTickles) {
                    return laugh_ ? react(CareEventKind::Tickled, CareLine::TickleLaugh, 0)
                                  : react(CareEventKind::Tickled, CareLine::TickleDone,
                                          CheerArg(ctx_.day));
                }
                if (laugh_) out.clip = CarePhraseId(CareLine::TickleLaugh, 0, t);
                return out;
            }
            if (gesture == Gesture::GentleShake) {
                last_input_ms_ = now_ms;
                if (++progress_ >= kTickles)
                    return react(CareEventKind::Tickled, CareLine::TickleDone, 0);
                out.redraw = true;
                out.clip = kTick;
            }
            break;
        case CareKind::Lull:
            if (gesture == Gesture::FaceDown) {
                last_input_ms_ = now_ms;
                progress_ = kLullStillWindows;
                return react(CareEventKind::Lulled, CareLine::LullDone, 0);
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
    // V9: the growth line makes the diary one line longer.
    const uint32_t diary_ms = kDiaryExitMs + (ctx_.diary.grew ? kDiaryGrowMs : 0);
    const uint32_t limit = kind_ == CareKind::Diary     ? diary_ms
                           : kind_ == CareKind::Bedsong ? kBedsongExitMs
                                                        : kIdleExitMs;
    if (static_cast<int32_t>(now_ms - (last_input_ms_ + SelfTestClock::Scaled(limit))) >= 0) {
        // Bedtime: the diary goes on (the song question or the lull); no
        // answer to the song question is a quiet "no"; a lullaby nobody
        // finished still ends in bed (the child put TuanTuan down).
        if (kind_ == CareKind::Diary) return AfterDiary(now_ms);
        out = Finish(0, now_ms);
        if (kind_ == CareKind::Bedsong) out.next = CareKind::Lull;
        out.sleep = kind_ == CareKind::Lull;
        return out;
    }
    return out;
}

CareOutput CareSession::AfterDiary(uint32_t now_ms) {
    CareOutput done = Finish(0, now_ms);
    // V9 S4: with a lullaby on the card, ask first; otherwise the V8 lull.
    done.next = ctx_.lullaby ? CareKind::Bedsong : CareKind::Lull;
    return done;
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
        case CareKind::Bath:
        case CareKind::Brush:
            v.goal = goal_;
            v.variant = variant_;
            break;
        case CareKind::Tickle:
            v.goal = kTickles;
            if (seed_) {
                const uint8_t round = progress_ < kTickles ? progress_ : kTickles - 1;
                v.variant = TickleVariant(TickleTarget(seed_, round), feather_, laugh_);
            }
            break;
        case CareKind::Lull: v.goal = kLullStillWindows; break;
        case CareKind::Tidy:
            v.goal = kToys;
            v.toy = ctx_.toy_art ? ctx_.toy_art : static_cast<uint16_t>(CareArt::ToyBear);
            break;
        case CareKind::Album:
            v.page = page_;
            v.pages = kAlbumPages;
            v.stickers = ctx_.stickers;
            break;
        case CareKind::Feed: v.variant = TreatVariant(ctx_.season.treat); break;
        case CareKind::Diary:
            v.food = WishPicture(static_cast<Food>(ctx_.diary.last_food), ctx_.diary.treat);
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
