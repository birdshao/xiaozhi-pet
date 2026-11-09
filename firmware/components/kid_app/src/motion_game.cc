#include "kid_app/motion_game.h"

#include "kid_app/selftest_clock.h"

#include <algorithm>

namespace kid_app {
namespace {
constexpr uint16_t A(GameArt art) { return static_cast<uint16_t>(art); }
// Built-in tones from mini_game.cc: nothing to record for these games.
constexpr uint16_t kClipPrompt = kGameTonePrompt;
constexpr uint16_t kClipHide = GameArtTone(static_cast<uint16_t>(GameArt::Square));
// Holes at the ends: always a few cells of rolling, never at the start.
constexpr uint8_t kHoles[] = {0, 1, 5, 6};
// A short tone for a menu card: the picture's own tone, or (the V4-08 cards,
// which have no tone of their own) a neighbour's from mini_game.cc.
uint16_t ChoiceTone(uint16_t art) {
    if (CareArtValid(art) || MenuArtValid(art)) return GameArtTone(A(GameArt::Heart));
    if (MicArtValid(art)) return art == static_cast<uint16_t>(MicArt::Hand) ? GameArtTone(A(GameArt::Square))
                                                                              : GameArtTone(A(GameArt::Star));
    return GameArtTone(art);
}
}  // namespace

const char* MotionKindName(MotionKind kind) {
    return kind == MotionKind::Ball ? "ball" : "peekaboo";
}

const char* GameChoiceName(GameChoice choice) {
    if (choice == GameChoice::None) return "none";
    const Activity* a = FindActivity(choice);
    return a ? a->name : "unknown";
}

// ---- TiltTracker ----

Gesture TiltTracker::OnWindow(const MotionStats::Summary& w) {
    if (w.samples == 0) return Gesture::None;
    if (w.mean_z_mg > config_.min_up_z_mg || w.max_dev_mg >= config_.max_dev_mg) {
        level_ = true;  // lying the other way up or shaking: nothing to report
        return Gesture::None;
    }
    if (config_.four_way) {
        const int h = (config_.use_y ? w.mean_y_mg : w.mean_x_mg) * config_.sign;
        const int v = (config_.use_y ? w.mean_x_mg : w.mean_y_mg) * config_.sign_v;
        const int ah = h < 0 ? -h : h, av = v < 0 ? -v : v;
        if (std::max(ah, av) > config_.tilt_mg) {
            level_ = false;
            if (ah >= av) return h > 0 ? Gesture::TiltRight : Gesture::TiltLeft;
            return v > 0 ? Gesture::TiltDown : Gesture::TiltUp;
        }
        if (ah <= config_.level_mg && av <= config_.level_mg && !level_) {
            level_ = true;
            return Gesture::Level;
        }
        return Gesture::None;
    }
    const int axis = (config_.use_y ? w.mean_y_mg : w.mean_x_mg) * config_.sign;
    if (axis > config_.tilt_mg) {
        level_ = false;
        return Gesture::TiltRight;
    }
    if (axis < -config_.tilt_mg) {
        level_ = false;
        return Gesture::TiltLeft;
    }
    if (axis >= -config_.level_mg && axis <= config_.level_mg && !level_) {
        level_ = true;
        return Gesture::Level;
    }
    return Gesture::None;
}

// ---- MotionGameSession ----

uint32_t MotionGameSession::Next() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return rng_;
}

bool MotionGameSession::Start(MotionKind kind, uint32_t seed, uint8_t rounds, uint32_t now_ms,
                              GameOutput* out) {
    if (!out || (kind != MotionKind::Ball && kind != MotionKind::Peekaboo)) return false;
    *out = {};
    kind_ = kind;
    rng_ = seed ? seed : 0x9E3779B9u;
    result_ = {};
    result_.game_id = kind == MotionKind::Ball ? kBallGameId : kPeekabooGameId;
    result_.rounds = std::max<uint8_t>(1, std::min<uint8_t>(rounds, GameSession::kMaxRounds));
    BeginRound(now_ms, out);
    return true;
}

void MotionGameSession::BeginRound(uint32_t now_ms, GameOutput* out) {
    phase_ = Phase::Playing;
    feedback_ = GameFeedback::None;
    ball_ = kBallStart;
    if (kind_ == MotionKind::Ball) hole_ = kHoles[Next() % (sizeof(kHoles) / sizeof(kHoles[0]))];
    repeated_ = false;
    last_input_ms_ = now_ms;
    out->redraw = true;
    out->clip = kClipPrompt;
}

void MotionGameSession::Finish(GameEnd end, GameOutput* out) {
    phase_ = Phase::Done;
    result_.end = end;
    out->finished = true;
    out->clip = kGameClipBye;
}

void MotionGameSession::Interrupt() {
    if (!Active()) return;
    phase_ = Phase::Done;
    result_.end = GameEnd::Interrupted;
}

void MotionGameSession::Win(uint32_t now_ms, GameOutput* out) {
    ++result_.correct;
    phase_ = Phase::Right;
    feedback_ = GameFeedback::Right;
    phase_until_ms_ = now_ms + kRightMs;
    out->redraw = true;
    out->right = true;
    out->clip = kGameClipRight;
}

GameOutput MotionGameSession::Input(GameInput input, uint32_t now_ms) {
    GameOutput out{};
    if (!Active()) return out;
    last_input_ms_ = now_ms;
    if (input == GameInput::Long) Finish(GameEnd::Home, &out);
    return out;
}

GameOutput MotionGameSession::OnGesture(Gesture gesture, uint32_t now_ms) {
    GameOutput out{};
    if (!Active()) return out;
    // Shaking and the gestures of the other game are not part of this one.
    if (kind_ == MotionKind::Ball) {
        if (gesture == Gesture::TiltLeft || gesture == Gesture::TiltRight ||
            gesture == Gesture::Level)
            last_input_ms_ = now_ms;
        if (phase_ != Phase::Playing) return out;  // TuanTuan is reacting
        if (gesture == Gesture::TiltLeft && ball_ > 0) {
            --ball_;
        } else if (gesture == Gesture::TiltRight && ball_ + 1 < kBallCells) {
            ++ball_;
        } else {
            return out;
        }
        out.redraw = true;
        if (ball_ == hole_) Win(now_ms, &out);
        return out;
    }
    if (gesture == Gesture::FaceDown && phase_ == Phase::Playing) {
        phase_ = Phase::Hidden;
        feedback_ = GameFeedback::None;
        hidden_since_ms_ = now_ms;
        last_input_ms_ = now_ms;
        out.redraw = true;
        out.clip = kClipHide;
    } else if (gesture == Gesture::FaceUp && phase_ == Phase::Hidden) {
        last_input_ms_ = now_ms;
        if (static_cast<uint32_t>(now_ms - hidden_since_ms_) >= kMinHideMs) {
            Win(now_ms, &out);  // "found you!"
        } else {
            phase_ = Phase::Playing;  // too early: gently, once more
            feedback_ = GameFeedback::TryAgain;
            out.redraw = true;
            out.clip = kGameClipTryAgain;
        }
    }
    return out;
}

GameOutput MotionGameSession::Tick(uint32_t now_ms) {
    GameOutput out{};
    if (phase_ == Phase::Playing) {
        if (Reached(now_ms, last_input_ms_ + SelfTestClock::Scaled(kIdleExitMs))) {
            Finish(GameEnd::Idle, &out);
        } else if (!repeated_ &&
                   Reached(now_ms, last_input_ms_ + SelfTestClock::Scaled(kRepeatPromptMs))) {
            repeated_ = true;
            out.clip = kClipPrompt;
        }
    } else if (phase_ == Phase::Hidden) {
        if (Reached(now_ms, hidden_since_ms_ + SelfTestClock::Scaled(kHiddenMaxMs)))
            Finish(GameEnd::Idle, &out);
    } else if (phase_ == Phase::Right && Reached(now_ms, phase_until_ms_)) {
        ++result_.played;
        if (result_.played >= result_.rounds) Finish(GameEnd::Completed, &out);
        else BeginRound(now_ms, &out);
    }
    return out;
}

GameView MotionGameSession::View() const {
    GameView v{};
    if (phase_ == Phase::Idle) return v;
    v.rounds = result_.rounds;
    v.round = static_cast<uint8_t>(std::min<int>(result_.played + 1, result_.rounds));
    v.feedback = feedback_;
    if (kind_ == MotionKind::Ball) {
        v.motion = 1;
        v.ball = ball_;
        v.hole = hole_;
        v.question_art = A(GameArt::BallBig);
    } else {
        v.motion = 2;
        v.hidden = phase_ == Phase::Hidden;
        v.question_art = A(GameArt::Cat);
    }
    return v;
}

// ---- GameMenu ----

namespace {
// Level one, in card order (today's pick first, when there is one).
constexpr ActivityCategory kTopPages[] = {ActivityCategory::Today, ActivityCategory::Brain,
                                          ActivityCategory::Move, ActivityCategory::Sound,
                                          ActivityCategory::Album};
constexpr uint8_t kTopPageCount = sizeof(kTopPages) / sizeof(kTopPages[0]);

uint16_t PageArt(ActivityCategory page) {
    switch (page) {
        case ActivityCategory::Today: return static_cast<uint16_t>(MenuArt::Today);
        case ActivityCategory::Brain: return static_cast<uint16_t>(MenuArt::Brain);
        case ActivityCategory::Move: return static_cast<uint16_t>(MenuArt::Move);
        case ActivityCategory::Sound: return static_cast<uint16_t>(MenuArt::Sound);
        case ActivityCategory::Album: {
            const Activity* a = ActivityInCategory(ActivityCategory::Album, 0);
            return a ? a->art : 0;
        }
        case ActivityCategory::None: break;
    }
    return 0;
}
}  // namespace

bool GameMenu::HasPick() const {
    for (GameChoice c : pick_)
        if (c != GameChoice::None && FindActivity(c)) return true;
    return false;
}

ActivityCategory GameMenu::TopCategoryAt(uint8_t i) const {
    for (uint8_t k = 0; k < kTopPageCount; ++k) {
        const ActivityCategory page = kTopPages[k];
        if (page == ActivityCategory::Today && !HasPick()) continue;
        if (page != ActivityCategory::Today && ActivityCountIn(page) == 0) continue;
        if (i == 0) return page;
        --i;
    }
    return ActivityCategory::None;
}

uint8_t GameMenu::TopIndexOf(ActivityCategory page) const {
    for (uint8_t i = 0; TopCategoryAt(i) != ActivityCategory::None; ++i)
        if (TopCategoryAt(i) == page) return i;
    return 0;
}

uint8_t GameMenu::Count() const {
    if (group_ != ActivityGroup::Play) return static_cast<uint8_t>(ActivityCount(group_));
    if (level_ == MenuLevel::Top) {
        uint8_t n = 0;
        while (TopCategoryAt(n) != ActivityCategory::None) ++n;
        return n;
    }
    if (page_ == ActivityCategory::Today) {
        uint8_t n = 0;
        for (GameChoice c : pick_)
            if (c != GameChoice::None && FindActivity(c)) ++n;
        return n;
    }
    return static_cast<uint8_t>(ActivityCountIn(page_));
}

GameChoice GameMenu::ChoiceAt(uint8_t i) const {
    if (group_ != ActivityGroup::Play) {
        const Activity* a = ActivityAt(group_, i);
        return a ? a->id : GameChoice::None;
    }
    if (level_ == MenuLevel::Top) {
        // The album is a leaf: its card is the game.
        const ActivityCategory page = TopCategoryAt(i);
        const Activity* a = page == ActivityCategory::Album ? ActivityInCategory(page, 0) : nullptr;
        return a ? a->id : GameChoice::None;
    }
    if (page_ == ActivityCategory::Today) {
        for (GameChoice c : pick_) {
            if (c == GameChoice::None || !FindActivity(c)) continue;
            if (i == 0) return c;
            --i;
        }
        return GameChoice::None;
    }
    const Activity* a = ActivityInCategory(page_, i);
    return a ? a->id : GameChoice::None;
}

uint16_t GameMenu::ArtAt(uint8_t i) const {
    if (group_ == ActivityGroup::Play && level_ == MenuLevel::Top) return PageArt(TopCategoryAt(i));
    const Activity* a = FindActivity(ChoiceAt(i));
    return a ? a->art : 0;
}

void GameMenu::OpenPage(ActivityCategory page, uint8_t index) {
    level_ = MenuLevel::Games;
    page_ = page;
    index_ = std::min<uint8_t>(index, static_cast<uint8_t>(Count() - 1));
}

GameMenuOutput GameMenu::Start(uint32_t now_ms, uint8_t start, ActivityGroup group) {
    GameMenuOutput out{};
    group_ = group;
    level_ = group == ActivityGroup::Play ? MenuLevel::Top : MenuLevel::Games;
    page_ = ActivityCategory::None;
    active_ = Count() > 0;
    end_ = GameEnd::None;
    if (!active_) {  // a group without rows: nothing to pick, straight home
        end_ = GameEnd::Home;
        out.finished = true;
        return out;
    }
    index_ = std::min<uint8_t>(start, static_cast<uint8_t>(Count() - 1));
    last_input_ms_ = now_ms;
    out.redraw = true;
    out.clip = kClipPrompt;
    return out;
}

GameMenuOutput GameMenu::StartOn(uint32_t now_ms, GameChoice game) {
    GameMenuOutput out{};
    const Activity* row = FindActivity(game);
    group_ = ActivityGroup::Play;
    end_ = GameEnd::None;
    if (!row || row->group != ActivityGroup::Play) {
        active_ = false;
        end_ = GameEnd::Home;
        out.finished = true;
        return out;
    }
    active_ = true;
    if (row->category == ActivityCategory::Album) {  // no page: level one, on its card
        level_ = MenuLevel::Top;
        page_ = ActivityCategory::None;
        index_ = TopIndexOf(ActivityCategory::Album);
    } else {
        uint8_t at = 0;
        while (ActivityInCategory(row->category, at) != row) ++at;
        OpenPage(row->category, at);
    }
    last_input_ms_ = now_ms;
    out.redraw = true;
    out.clip = kClipPrompt;
    return out;
}

GameMenuOutput GameMenu::Input(GameInput input, uint32_t now_ms) {
    GameMenuOutput out{};
    if (!active_) return out;
    last_input_ms_ = now_ms;
    switch (input) {
        case GameInput::Up:
        case GameInput::Down:
            if (input == GameInput::Up ? index_ > 0 : index_ + 1 < Count()) {
                index_ = static_cast<uint8_t>(index_ + (input == GameInput::Up ? -1 : 1));
                out.redraw = true;
                const GameView v = View();
                out.clip = ChoiceTone(v.options[v.highlight]);
            }
            break;
        case GameInput::Press:
            if (group_ == ActivityGroup::Play && level_ == MenuLevel::Top &&
                ChoiceAt(index_) == GameChoice::None) {  // a page: open it on its first card
                OpenPage(TopCategoryAt(index_), 0);
                out.redraw = true;
                out.level_changed = true;
                out.clip = kClipPrompt;
                break;
            }
            active_ = false;
            out.chosen = ChoiceAt(index_);
            break;
        case GameInput::Long:
            if (group_ == ActivityGroup::Play && level_ == MenuLevel::Games) {  // back one level
                const ActivityCategory page = page_;
                level_ = MenuLevel::Top;
                page_ = ActivityCategory::None;
                index_ = TopIndexOf(page);
                out.redraw = true;
                out.level_changed = true;
                out.clip = kClipPrompt;
                break;
            }
            active_ = false;
            end_ = GameEnd::Home;
            out.finished = true;
            break;
    }
    return out;
}

GameMenuOutput GameMenu::Home() {
    GameMenuOutput out{};
    if (!active_) return out;
    active_ = false;
    end_ = GameEnd::Home;
    out.finished = true;
    return out;
}

GameMenuOutput GameMenu::Tick(uint32_t now_ms) {
    GameMenuOutput out{};
    if (active_ &&
        static_cast<int32_t>(now_ms - (last_input_ms_ + SelfTestClock::Scaled(kIdleExitMs))) >= 0) {
        active_ = false;
        end_ = GameEnd::Idle;
        out.finished = true;
    }
    return out;
}

void GameMenu::Interrupt() {
    if (!active_) return;
    active_ = false;
    end_ = GameEnd::Interrupted;
}

GameView GameMenu::View() const {
    GameView v{};
    if (!active_) return v;
    // Three cards at a time (fewer if the list is shorter), the window
    // follows the highlight.
    const uint8_t n = Count();
    const uint8_t shown = std::min<uint8_t>(n, 3);
    v.option_count = shown;
    v.question_art = A(GameArt::Heart);
    const uint8_t first = index_ == 0 ? 0 : std::min<uint8_t>(index_ - 1, n - shown);
    v.highlight = static_cast<uint8_t>(index_ - first);
    for (uint8_t i = 0; i < shown; ++i) v.options[i] = ArtAt(first + i);
    return v;
}

}  // namespace kid_app
