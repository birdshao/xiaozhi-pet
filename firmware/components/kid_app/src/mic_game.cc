#include "kid_app/mic_game.h"

#include "kid_app/selftest_clock.h"

#include <algorithm>

namespace kid_app {

const char* MicGameKindName(MicGameKind kind) {
    switch (kind) {
        case MicGameKind::Candle: return "candle";
        case MicGameKind::ClapRhythm: return "clap";
    }
    return "unknown";
}

bool MicGameSession::Start(MicGameKind kind, uint32_t now_ms, GameOutput* out) {
    if (!out || (kind != MicGameKind::Candle && kind != MicGameKind::ClapRhythm)) return false;
    *out = {};
    kind_ = kind;
    result_ = {};
    feedback_ = GameFeedback::None;
    round_ = 0;
    tries_ = 0;
    claps_ = 0;
    blow_ms_ = 0;
    guard_until_ms_ = now_ms + kEchoGuardMs;  // the start tone is not a blow
    last_tick_ms_ = now_ms;
    last_activity_ms_ = now_ms;
    if (kind == MicGameKind::Candle) {
        result_.game_id = kMicCandleGameId;
        result_.rounds = 1;
        lit_ = kCandles;
        phase_ = Phase::Asking;
        out->redraw = true;
        out->clip = kMicClipStart;
    } else {
        result_.game_id = kMicClapGameId;
        result_.rounds = kMicClapRounds;
        lit_ = 0;
        BeginClapRound(now_ms, out);
    }
    return true;
}

void MicGameSession::BeginClapRound(uint32_t now_ms, GameOutput* out) {
    phase_ = Phase::Intro;
    feedback_ = GameFeedback::None;
    claps_ = 0;
    beat_ = 0;
    next_ms_ = now_ms + kIntroMs;
    last_activity_ms_ = now_ms;
    out->redraw = true;
    out->clip = kMicClipStart;
}

void MicGameSession::Finish(GameEnd end, GameOutput* out) {
    phase_ = Phase::Done;
    result_.end = end;
    out->finished = true;
    out->clip = kGameClipBye;
}

void MicGameSession::Interrupt() {
    if (!Active()) return;
    phase_ = Phase::Done;
    result_.end = GameEnd::Interrupted;
}

GameOutput MicGameSession::Input(GameInput input, uint32_t now_ms) {
    GameOutput out{};
    if (!Active()) return out;
    last_activity_ms_ = now_ms;
    if (input == GameInput::Long) Finish(GameEnd::Home, &out);
    return out;  // the dial does nothing else: these games listen
}

GameOutput MicGameSession::Tick(uint32_t now_ms, const MicDecision& mic) {
    if (!Active()) return {};
    return kind_ == MicGameKind::Candle ? TickCandle(now_ms, mic) : TickClap(now_ms, mic);
}

GameOutput MicGameSession::TickCandle(uint32_t now_ms, const MicDecision& mic) {
    GameOutput out{};
    if (phase_ == Phase::Right) {
        if (Reached(now_ms, next_ms_)) {
            result_.played = 1;
            Finish(GameEnd::Completed, &out);
        }
        return out;
    }
    const uint32_t dt = std::min<uint32_t>(now_ms - last_tick_ms_, 100);
    last_tick_ms_ = now_ms;
    if (Reached(now_ms, last_activity_ms_ + SelfTestClock::Scaled(kIdleExitMs))) {
        Finish(GameEnd::Idle, &out);
        return out;
    }
    if (!Reached(now_ms, guard_until_ms_)) return out;  // deaf while our own tone plays
    if (!mic.blowing) {
        blow_ms_ = blow_ms_ > 2 * dt ? blow_ms_ - 2 * dt : 0;  // a gasp for air costs a little
        return out;
    }
    last_activity_ms_ = now_ms;
    blow_ms_ += dt;
    if (blow_ms_ < kBlowMs) return out;
    blow_ms_ = 0;
    --lit_;
    out.redraw = true;
    guard_until_ms_ = now_ms + kEchoGuardMs;
    if (lit_ == 0) {  // the last flame: the one reward
        ++result_.correct;
        phase_ = Phase::Right;
        feedback_ = GameFeedback::Right;
        next_ms_ = now_ms + kRightMs;
        out.right = true;
        out.clip = kGameClipRight;
    } else {
        out.clip = kMicClipOut;
    }
    return out;
}

void MicGameSession::ClapFailed(uint32_t now_ms, GameOutput* out) {
    ++tries_;
    phase_ = Phase::Retry;
    next_ms_ = now_ms + kRetryMs;
    out->redraw = true;
    if (tries_ >= kMaxTries) {  // shown once more, no reward, on to the next
        feedback_ = GameFeedback::Reveal;
        out->clip = kGameClipReveal;
    } else {
        feedback_ = GameFeedback::TryAgain;
        out->clip = kGameClipTryAgain;
    }
}

GameOutput MicGameSession::TickClap(uint32_t now_ms, const MicDecision& mic) {
    GameOutput out{};
    switch (phase_) {
        case Phase::Intro:
        case Phase::Demo: {
            if (!Reached(now_ms, next_ms_)) break;
            const uint32_t at = next_ms_;  // scheduled time: the beats do not drift
            phase_ = Phase::Demo;
            out.clip = kMicClipBeat;
            if (++beat_ < kMicClapsPerRound) {
                next_ms_ = at + kMicClapGaps[round_][beat_ - 1];
            } else {
                phase_ = Phase::Turn;
                next_ms_ = at + kTurnGapMs;
            }
            break;
        }
        case Phase::Turn:
            if (!Reached(now_ms, next_ms_)) break;
            out.clip = kMicClipYourTurn;
            phase_ = Phase::Quiet;
            next_ms_ += kTurnToneMs;
            break;
        case Phase::Quiet:  // the tone plays: the microphone is still off
            if (!Reached(now_ms, next_ms_)) break;
            phase_ = Phase::Arm;
            next_ms_ += kWarmMs;
            break;
        case Phase::Arm:  // open, learning the room: claps are not counted yet
            if (!Reached(now_ms, next_ms_)) break;
            phase_ = Phase::Listen;
            listen_ms_ = now_ms;
            claps_ = 0;
            out.redraw = true;
            break;
        case Phase::Listen: {
            if (mic.clap && claps_ < kMicClapsPerRound) {
                clap_at_[claps_++] = now_ms;
                out.redraw = true;
                if (claps_ == kMicClapsPerRound) {
                    bool match = true;
                    for (uint8_t i = 0; i + 1 < kMicClapsPerRound; ++i) {
                        const int32_t gap = static_cast<int32_t>(clap_at_[i + 1] - clap_at_[i]);
                        const int32_t want = kMicClapGaps[round_][i];
                        match = match && std::abs(gap - want) <= static_cast<int32_t>(kClapToleranceMs);
                    }
                    if (match) {
                        ++result_.correct;
                        phase_ = Phase::Right;
                        feedback_ = GameFeedback::Right;
                        next_ms_ = now_ms + kRightMs;
                        out.right = true;
                        out.clip = kGameClipRight;
                    } else {
                        ClapFailed(now_ms, &out);
                    }
                }
            } else if (Reached(now_ms, listen_ms_ + kListenMs) ||
                       (claps_ > 0 && Reached(now_ms, clap_at_[claps_ - 1] + kStallMs))) {
                ClapFailed(now_ms, &out);
            }
            break;
        }
        case Phase::Right:
        case Phase::Retry: {
            if (!Reached(now_ms, next_ms_)) break;
            if (phase_ == Phase::Retry && tries_ < kMaxTries) {  // the same pattern again
                BeginClapRound(now_ms, &out);
                break;
            }
            ++result_.played;
            if (result_.played >= result_.rounds) {
                Finish(GameEnd::Completed, &out);
                break;
            }
            round_ = result_.played;
            tries_ = 0;
            BeginClapRound(now_ms, &out);
            break;
        }
        default:
            break;
    }
    return out;
}

GameView MicGameSession::View() const {
    GameView v{};
    if (phase_ == Phase::Idle) return v;
    v.rounds = result_.rounds;
    v.round = static_cast<uint8_t>(std::min<int>(result_.played + 1, result_.rounds));
    v.option_count = 3;
    v.highlight = 0xFF;  // nothing under the dial: these games listen
    v.feedback = feedback_;
    if (kind_ == MicGameKind::Candle) {
        v.question_art = static_cast<uint16_t>(MicArt::Blow);
        for (uint8_t i = 0; i < kCandles; ++i)
            v.options[i] = static_cast<uint16_t>(i < kCandles - lit_ ? MicArt::CandleOut
                                                                       : MicArt::CandleLit);
    } else {
        v.question_art = static_cast<uint16_t>(MicArt::Hand);
        const bool answering = phase_ == Phase::Listen || phase_ == Phase::Right;
        for (uint8_t i = 0; i < kMicClapsPerRound; ++i)
            v.options[i] = static_cast<uint16_t>(answering && i < claps_ ? MicArt::HandHit
                                                                         : MicArt::Hand);
    }
    return v;
}

}  // namespace kid_app
