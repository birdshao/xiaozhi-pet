#include "kid_app/play_game.h"

#include "kid_app/selftest_clock.h"

#include <algorithm>

namespace kid_app {
namespace {
constexpr uint16_t U(PlayArt art) { return static_cast<uint16_t>(art); }
constexpr uint16_t A(GameArt art) { return static_cast<uint16_t>(art); }

// Built-in tones (mini_game.cc generates a tone for every picture id).
constexpr uint16_t kClipPrompt = kGameTonePrompt;
constexpr uint16_t kClipStep = GameArtTone(A(GameArt::Circle));
constexpr uint16_t kClipShake = GameArtTone(U(PlayArt::Dots6));
constexpr uint16_t kClipNight = GameArtTone(U(PlayArt::Moon));
constexpr uint16_t kClipDay = GameArtTone(U(PlayArt::Sun));
constexpr uint16_t kClipFreeze = GameArtTone(U(PlayArt::Statue));

constexpr uint8_t kUp = 0x80, kMatched = 0x40, kFaceMask = 0x1F;

// Dice-style dots: GameArt Dots1..5, PlayArt Dots6.
uint16_t DotsArt(uint8_t n) {
    return n >= 6 ? U(PlayArt::Dots6) : static_cast<uint16_t>(A(GameArt::Dots1) + n - 1);
}
}  // namespace

uint32_t PlaySession::Next() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return rng_;
}

bool PlaySession::Start(PlayGame game, uint32_t seed, uint32_t now_ms, GameOutput* out) {
    if (!out) return false;
    const PlayGameInfo* info = FindPlayGame(game);
    if (!info || IsCardGame(game)) return false;
    *out = {};
    game_ = game;
    rng_ = seed ? seed : 0x9E3779B9u;
    result_ = {};
    result_.game_id = info->id;
    result_.rounds = game == PlayGame::Memory4   ? 2
                     : game == PlayGame::Memory6 ? 3
                     : game == PlayGame::Maze    ? kMazeRounds
                                                 : kDefaultRounds;
    BeginRound(now_ms, out);
    return true;
}

void PlaySession::BeginRound(uint32_t now_ms, GameOutput* out) {
    feedback_ = GameFeedback::None;
    last_input_ms_ = now_ms;
    repeated_ = false;
    out->redraw = true;
    out->clip = kClipPrompt;
    switch (game_) {
        case PlayGame::Memory4:
        case PlayGame::Memory6: {
            const uint8_t pairs = result_.rounds;
            cols_ = pairs == 2 ? 2 : 3;
            rows_ = 2;
            // `pairs` distinct faces out of nine (partial Fisher-Yates), twice, shuffled.
            uint8_t faces[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
            for (uint8_t i = 0; i < pairs; ++i)
                std::swap(faces[i], faces[i + Next() % (9 - i)]);
            cells_.fill(0);
            const uint8_t total = static_cast<uint8_t>(pairs * 2);
            for (uint8_t i = 0; i < total; ++i) cells_[i] = faces[i / 2];
            for (uint8_t i = total; i > 1; --i) std::swap(cells_[i - 1], cells_[Next() % i]);
            cursor_ = 0;
            first_ = second_ = -1;
            phase_ = Phase::Playing;
            break;
        }
        case PlayGame::Maze:
            BuildMaze();
            armed_ = true;
            phase_ = Phase::Playing;
            break;
        case PlayGame::Dice:
        case PlayGame::Tree:
            die_ = 0;
            options_ = {};
            phase_ = Phase::Waiting;
            break;
        case PlayGame::Statue:
            move_windows_ = still_windows_ = 0;
            window_seen_ = false;
            phase_ = Phase::Dance;
            break;
        case PlayGame::DayNight:
            phase_ = Phase::Playing;  // "it is getting dark": waiting for face down
            out->clip = kClipNight;
            break;
        default:
            phase_ = Phase::Done;
            break;
    }
}

// Recursive backtracker on a cols x rows grid (always a spanning tree, so
// every cell, the carrot included, can be reached). The carrot goes to the
// cell farthest from the start by walking distance.
void PlaySession::BuildMaze() {
    cols_ = result_.played == 0 ? 4 : 5;
    rows_ = result_.played == 0 ? 3 : 4;
    cells_.fill(0);
    const uint8_t total = static_cast<uint8_t>(cols_ * rows_);
    std::array<bool, 24> seen{};
    std::array<uint8_t, 24> stack{};
    uint8_t depth = 0;
    stack[depth++] = 0;
    seen[0] = true;
    while (depth > 0) {
        const uint8_t cell = stack[depth - 1];
        const uint8_t r = cell / cols_, c = cell % cols_;
        // Unvisited neighbours: bit, delta cell.
        uint8_t bit[4], to[4], n = 0;
        if (r > 0 && !seen[cell - cols_]) { bit[n] = 1; to[n++] = static_cast<uint8_t>(cell - cols_); }
        if (c + 1 < cols_ && !seen[cell + 1]) { bit[n] = 2; to[n++] = static_cast<uint8_t>(cell + 1); }
        if (r + 1 < rows_ && !seen[cell + cols_]) { bit[n] = 4; to[n++] = static_cast<uint8_t>(cell + cols_); }
        if (c > 0 && !seen[cell - 1]) { bit[n] = 8; to[n++] = static_cast<uint8_t>(cell - 1); }
        if (n == 0) {
            --depth;
            continue;
        }
        const uint8_t pick = static_cast<uint8_t>(Next() % n);
        cells_[cell] |= bit[pick];
        const uint8_t opposite = bit[pick] == 1 ? 4 : bit[pick] == 4 ? 1 : bit[pick] == 2 ? 8 : 2;
        cells_[to[pick]] |= opposite;
        seen[to[pick]] = true;
        stack[depth++] = to[pick];
    }
    // Breadth-first distance from the start.
    std::array<int8_t, 24> dist{};
    dist.fill(-1);
    std::array<uint8_t, 24> queue{};
    uint8_t head = 0, tail = 0;
    queue[tail++] = 0;
    dist[0] = 0;
    while (head < tail) {
        const uint8_t cell = queue[head++];
        const uint8_t r = cell / cols_, c = cell % cols_;
        const uint8_t w = cells_[cell];
        auto go = [&](bool open, int next) {
            if (open && dist[next] < 0) {
                dist[next] = static_cast<int8_t>(dist[cell] + 1);
                queue[tail++] = static_cast<uint8_t>(next);
            }
        };
        go((w & 1) && r > 0, cell - cols_);
        go((w & 2) && c + 1 < cols_, cell + 1);
        go((w & 4) && r + 1 < rows_, cell + cols_);
        go((w & 8) && c > 0, cell - 1);
    }
    goal_ = 0;
    for (uint8_t i = 0; i < total; ++i)
        if (dist[i] > dist[goal_]) goal_ = i;
    pos_ = 0;
}

void PlaySession::Finish(GameEnd end, GameOutput* out) {
    phase_ = Phase::Done;
    result_.end = end;
    out->finished = true;
    out->clip = kGameClipBye;
}

void PlaySession::Interrupt() {
    if (!Active()) return;
    phase_ = Phase::Done;
    result_.end = GameEnd::Interrupted;
}

void PlaySession::Win(uint32_t now_ms, GameOutput* out) {
    ++result_.correct;
    phase_ = Phase::Right;
    feedback_ = GameFeedback::Right;
    phase_until_ms_ = now_ms + kRightMs;
    out->redraw = true;
    out->right = true;
    out->clip = kGameClipRight;
}

void PlaySession::NextRound(uint32_t now_ms, GameOutput* out) {
    ++result_.played;
    if (result_.played >= result_.rounds) Finish(GameEnd::Completed, out);
    else BeginRound(now_ms, out);
}

void PlaySession::BeginAsk(uint8_t count, uint32_t now_ms, GameOutput* out) {
    const bool dice = game_ == PlayGame::Dice;
    die_ = count;
    const uint8_t top = dice ? 6 : 5;
    // The right count and two others from 1..top.
    uint8_t counts[3] = {count, 0, 0};
    for (uint8_t k = 1; k < 3; ++k) {
        uint8_t c;
        do {
            c = static_cast<uint8_t>(1 + Next() % top);
        } while (c == counts[0] || c == counts[1]);
        counts[k] = c;
    }
    for (uint8_t i = 3; i > 1; --i) std::swap(counts[i - 1], counts[Next() % i]);
    for (uint8_t i = 0; i < 3; ++i) {
        options_[i] = DotsArt(counts[i]);
        if (counts[i] == count) answer_ = i;
    }
    choice_.Reset(3);
    tries_ = 0;
    feedback_ = GameFeedback::None;
    phase_ = Phase::Asking;
    last_input_ms_ = now_ms;
    repeated_ = false;
    out->redraw = true;
    out->clip = kClipShake;
}

bool PlaySession::Selectable(uint8_t card) const {
    return card < cols_ * rows_ && !(cells_[card] & (kUp | kMatched));
}

GameOutput PlaySession::FlipCard(uint32_t now_ms) {
    GameOutput out{};
    const uint8_t card = cursor_;
    if (!Selectable(card)) return out;
    cells_[card] |= kUp;
    out.redraw = true;
    out.clip = GameArtTone(kMemoryFaceArt[(cells_[card] & kFaceMask) - 1]);
    if (first_ < 0) {
        first_ = static_cast<int8_t>(card);
        return out;
    }
    second_ = static_cast<int8_t>(card);
    if ((cells_[first_] & kFaceMask) == (cells_[second_] & kFaceMask)) {
        cells_[first_] |= kMatched;
        cells_[second_] |= kMatched;
        ++result_.played;
        Win(now_ms, &out);  // matched pair: the only reward, via the dial
    } else {
        phase_ = Phase::Mismatch;
        feedback_ = GameFeedback::TryAgain;
        phase_until_ms_ = now_ms + kMismatchMs;
        out.clip = kGameClipTryAgain;
    }
    return out;
}

GameOutput PlaySession::Input(GameInput input, uint32_t now_ms) {
    GameOutput out{};
    if (!Active()) return out;
    last_input_ms_ = now_ms;
    if (input == GameInput::Long) {
        Finish(GameEnd::Home, &out);
        return out;
    }
    if ((game_ == PlayGame::Memory4 || game_ == PlayGame::Memory6) && phase_ == Phase::Playing) {
        if (input == GameInput::Press) return FlipCard(now_ms);
        // Move to the previous / next card that can still be picked.
        const int step = input == GameInput::Up ? -1 : 1;
        for (int next = static_cast<int>(cursor_) + step; next >= 0 && next < cols_ * rows_;
             next += step) {
            if (Selectable(static_cast<uint8_t>(next))) {
                cursor_ = static_cast<uint8_t>(next);
                feedback_ = GameFeedback::None;
                out.redraw = true;  // the card's face is a secret: no sound
                break;
            }
        }
        return out;
    }
    if ((game_ == PlayGame::Dice || game_ == PlayGame::Tree) && phase_ == Phase::Asking) {
        if (input == GameInput::Up || input == GameInput::Down) {
            if (!choice_.Move(input == GameInput::Up ? -1 : 1)) return out;
            feedback_ = GameFeedback::None;
            out.redraw = true;
            out.clip = GameArtTone(options_[choice_.Index()]);
            return out;
        }
        out.redraw = true;
        if (choice_.Index() == answer_) {
            Win(now_ms, &out);
        } else if (++tries_ >= kMaxTries) {
            phase_ = Phase::Reveal;
            feedback_ = GameFeedback::Reveal;
            choice_.Reset(3, answer_);
            phase_until_ms_ = now_ms + kRightMs;
            out.clip = kGameClipReveal;
        } else {
            feedback_ = GameFeedback::TryAgain;
            out.clip = kGameClipTryAgain;
        }
        return out;
    }
    // Waiting for a shake (parents may switch motion off meanwhile): a press
    // rolls too. Rolling is never a reward.
    if ((game_ == PlayGame::Dice || game_ == PlayGame::Tree) && phase_ == Phase::Waiting &&
        input == GameInput::Press) {
        BeginAsk(static_cast<uint8_t>(1 + Next() % (game_ == PlayGame::Dice ? 6 : 5)), now_ms,
                 &out);
        return out;
    }
    return out;  // everything else is not asked of the dial
}

GameOutput PlaySession::OnGesture(Gesture gesture, uint32_t now_ms) {
    GameOutput out{};
    if (!Active()) return out;
    if (game_ == PlayGame::Maze && phase_ == Phase::Playing) {
        if (gesture == Gesture::Level) {
            armed_ = true;
            return out;
        }
        uint8_t bit = 0;
        switch (gesture) {
            case Gesture::TiltUp: bit = 1; break;
            case Gesture::TiltRight: bit = 2; break;
            case Gesture::TiltDown: bit = 4; break;
            case Gesture::TiltLeft: bit = 8; break;
            default: return out;  // shaking and the rest are not part of the game
        }
        if (!armed_) return out;  // held tilt: one cell per tilt
        armed_ = false;
        last_input_ms_ = now_ms;
        if (!(cells_[pos_] & bit)) return out;  // a wall
        pos_ = static_cast<uint8_t>(bit == 1   ? pos_ - cols_
                                    : bit == 2 ? pos_ + 1
                                    : bit == 4 ? pos_ + cols_
                                               : pos_ - 1);
        out.redraw = true;
        out.clip = kClipStep;
        if (pos_ == goal_) Win(now_ms, &out);
        return out;
    }
    if ((game_ == PlayGame::Dice || game_ == PlayGame::Tree) && phase_ == Phase::Waiting) {
        // Rolling is not a reward: a gentle or a hard shake both just roll.
        if (gesture != Gesture::GentleShake && gesture != Gesture::HardShake) return out;
        const uint8_t count =
            static_cast<uint8_t>(1 + Next() % (game_ == PlayGame::Dice ? 6 : 5));
        BeginAsk(count, now_ms, &out);
        return out;
    }
    if (game_ == PlayGame::DayNight) {
        if (phase_ == Phase::Playing && gesture == Gesture::FaceDown) {
            phase_ = Phase::Night;
            night_since_ms_ = now_ms;
            night_for_ms_ = kNightMinMs + Next() % (kNightMaxMs - kNightMinMs + 1);
            last_input_ms_ = now_ms;
            feedback_ = GameFeedback::None;
            out.redraw = true;
            out.clip = kClipNight;
        } else if (phase_ == Phase::Night && gesture == Gesture::FaceUp) {
            phase_ = Phase::Playing;  // flipped before it was day: once more, gently
            feedback_ = GameFeedback::TryAgain;
            last_input_ms_ = now_ms;
            out.redraw = true;
            out.clip = kGameClipTryAgain;
        } else if (phase_ == Phase::DayCue && gesture == Gesture::FaceUp) {
            Win(now_ms, &out);
        }
    }
    return out;
}

GameOutput PlaySession::OnWindow(const MotionStats::Summary& w, uint32_t now_ms) {
    GameOutput out{};
    if (!WantsWindows() || w.samples < kMinWindowSamples) return out;
    if (phase_ == Phase::Dance) {
        if (w.max_dev_mg < kMoveDevMg) {  // a pause (or a board on the table): start over
            move_windows_ = 0;
            return out;
        }
        last_input_ms_ = now_ms;
        if (++move_windows_ >= kMoveWindows) {
            phase_ = Phase::Freeze;
            still_windows_ = 0;
            window_seen_ = false;
            feedback_ = GameFeedback::None;
            out.redraw = true;
            out.clip = kClipFreeze;
        }
        return out;
    }
    if (phase_ != Phase::Freeze) return out;
    if (!window_seen_) {  // the window that held the cue is half motion: skip it
        window_seen_ = true;
        return out;
    }
    if (w.max_dev_mg <= kStillDevMg) {
        const uint8_t before = still_windows_ / 2;
        ++still_windows_;
        last_input_ms_ = now_ms;
        if (still_windows_ >= kFreezeWindows) {
            Win(now_ms, &out);
        } else if (still_windows_ / 2 != before) {
            out.redraw = true;  // one more second held
        }
    } else if (still_windows_ > 0) {
        still_windows_ = 0;  // moved: start counting again
        out.redraw = true;
    }
    return out;
}

GameOutput PlaySession::Tick(uint32_t now_ms) {
    GameOutput out{};
    switch (phase_) {
        case Phase::Playing:
        case Phase::Waiting:
        case Phase::Asking:
        case Phase::Dance:
        case Phase::Freeze:
        case Phase::DayCue:
            if (Reached(now_ms, last_input_ms_ + SelfTestClock::Scaled(kIdleExitMs))) {
                Finish(GameEnd::Idle, &out);
            } else if (!repeated_ &&
                       Reached(now_ms, last_input_ms_ + SelfTestClock::Scaled(kRepeatPromptMs))) {
                repeated_ = true;
                out.clip = phase_ == Phase::DayCue ? kClipDay : kClipPrompt;
            }
            break;
        case Phase::Night:
            if (Reached(now_ms, night_since_ms_ + night_for_ms_)) {
                phase_ = Phase::DayCue;  // "it is morning": now flip back
                last_input_ms_ = now_ms;
                repeated_ = false;
                out.redraw = true;
                out.clip = kClipDay;
            } else if (Reached(now_ms, night_since_ms_ + SelfTestClock::Scaled(kNightHiddenMaxMs))) {
                Finish(GameEnd::Idle, &out);
            }
            break;
        case Phase::Mismatch:
            if (Reached(now_ms, phase_until_ms_)) {
                cells_[first_] &= static_cast<uint8_t>(~kUp);
                cells_[second_] &= static_cast<uint8_t>(~kUp);
                cursor_ = static_cast<uint8_t>(second_);
                first_ = second_ = -1;
                feedback_ = GameFeedback::None;
                phase_ = Phase::Playing;
                last_input_ms_ = now_ms;
                out.redraw = true;
            }
            break;
        case Phase::Right:
            if (!Reached(now_ms, phase_until_ms_)) break;
            if (game_ == PlayGame::Memory4 || game_ == PlayGame::Memory6) {
                if (result_.played >= result_.rounds) {
                    Finish(GameEnd::Completed, &out);
                } else {
                    first_ = second_ = -1;
                    feedback_ = GameFeedback::None;
                    phase_ = Phase::Playing;
                    last_input_ms_ = now_ms;
                    for (uint8_t i = 0; i < cols_ * rows_; ++i)
                        if (Selectable(i)) {
                            cursor_ = i;
                            break;
                        }
                    out.redraw = true;
                }
            } else {
                NextRound(now_ms, &out);
            }
            break;
        case Phase::Reveal:
            if (Reached(now_ms, phase_until_ms_)) NextRound(now_ms, &out);
            break;
        case Phase::Idle:
        case Phase::Done:
            break;
    }
    return out;
}

GameView PlaySession::View() const {
    GameView v{};
    if (phase_ == Phase::Idle) return v;
    v.rounds = result_.rounds;
    v.round = static_cast<uint8_t>(std::min<int>(result_.played + 1, result_.rounds));
    v.feedback = feedback_;
    switch (game_) {
        case PlayGame::Memory4:
        case PlayGame::Memory6:
            v.play = 3;
            v.cols = cols_;
            v.rows = rows_;
            v.cells = cells_;
            v.cursor = cursor_;
            v.question_art = A(GameArt::Heart);
            break;
        case PlayGame::Maze:
            v.play = 4;
            v.cols = cols_;
            v.rows = rows_;
            v.cells = cells_;
            v.pos = pos_;
            v.goal = goal_;
            v.question_art = U(PlayArt::Carrot);
            break;
        case PlayGame::Dice:
        case PlayGame::Tree: {
            v.play = 5;
            const bool asked = die_ != 0;
            v.question_art = !asked ? U(PlayArt::ShakeMe)
                             : game_ == PlayGame::Dice ? PlayDiceArt(die_)
                                                       : PlayTreeArt(die_);
            if (asked) {
                v.option_count = 3;
                v.highlight = choice_.Index();
                v.options = options_;
            }
            break;
        }
        case PlayGame::Statue:
            v.play = 6;
            v.question_art = phase_ == Phase::Freeze || phase_ == Phase::Right
                                 ? U(PlayArt::Statue)
                                 : U(PlayArt::Dance);
            v.count = static_cast<uint8_t>(still_windows_ / 2);
            break;
        case PlayGame::DayNight:
            v.play = 7;
            v.night = phase_ == Phase::Night;
            v.hidden = phase_ == Phase::Night;
            v.question_art = phase_ == Phase::DayCue || phase_ == Phase::Right
                                 ? U(PlayArt::Sun)
                                 : U(PlayArt::Moon);
            v.count = phase_ == Phase::Night ? 1 : phase_ == Phase::DayCue ? 2 : 0;
            break;
        default:
            break;
    }
    return v;
}

}  // namespace kid_app
