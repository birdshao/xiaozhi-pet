#pragma once

#include "kid_app/mini_game.h"
#include "kid_app/personality.h"

#include <cstdint>

namespace kid_app {

// V8-05: TuanTuan keeps the child company while brushing teeth (2 minutes)
// or washing hands (20 seconds). Display-led and low refresh: an hourglass
// whose sand moves one step every FrameMs() (a partial refresh each, well
// inside the refresh gate, docs/P1_REFRESH_GATE.md), three short spoken
// cues, then praise. Pure and deterministic: no clock of its own (uint32 ms
// passed in, wrap-safe), no randomness, no IO.
//
// Why the child confirms first (Ready): the routine may be OPENED by voice
// ("刷牙") or the home rail, but it only RUNS after a physical dial press,
// so a finished routine is always a Device-originated interaction. Voice
// and Network never cause a care record (ADR-012, V5-03).
//
// Not punishing: stopping early (dial hold = home, BOOT, PWR, a voice
// session) ends quietly: no completion, no record, no scolding.
enum class RoutineKind : uint8_t { Brush = 1, Wash = 2 };
const char* RoutineKindName(RoutineKind kind);

enum class RoutinePhase : uint8_t { Idle, Ready, Running, Praise, Done };

// A spoken line. The wording is reviewed (docs/V8_SOUND_AND_ROUTINES.md);
// RoutineClipId() is the KPS1 clip id inside the `routines` pack.
enum class RoutineCue : uint8_t {
    None,
    Start,   // "我们开始啦" when the dial is pressed
    Middle,  // brush: "刷刷里面"; wash: "搓搓手指缝"
    Done,    // praise at the end
};

constexpr uint16_t RoutineClipId(RoutineKind kind, RoutineCue cue) {
    // 1..3 brush start/middle/done, 11..13 wash.
    return static_cast<uint16_t>((kind == RoutineKind::Brush ? 0 : 10) +
                                 static_cast<uint16_t>(cue));
}
// What the board plays without a `routines` pack: the built-in chime of an
// existing phrase context (nothing new in flash).
constexpr PhraseContext RoutineFallbackChime(RoutineCue cue) {
    return cue == RoutineCue::Start    ? PhraseContext::Greeting
           : cue == RoutineCue::Middle ? PhraseContext::Giggle
                                       : PhraseContext::Feed;
}

struct RoutineOutput {
    bool redraw = false;                // draw View() now (one partial refresh)
    RoutineCue cue = RoutineCue::None;  // speak this (never set when silent)
    bool completed = false;             // true once: ran the full time
    bool leave = false;                 // go back home
};

struct RoutineView {
    RoutineKind kind = RoutineKind::Brush;
    RoutinePhase phase = RoutinePhase::Idle;
    uint8_t step = 0;   // sand moved so far, 0..steps
    uint8_t steps = 0;  // total frames after the first
    uint16_t seconds_left = 0;
};

// What the care store (V8-01) receives. The proposed interface (PR notes):
// one Device-source event per completion, never from Voice/Network.
struct RoutineCareRecord {
    RoutineKind kind = RoutineKind::Brush;
    uint32_t seconds = 0;  // the planned length that was fully served
};

class RoutineTimer {
public:
    static constexpr uint32_t kBrushMs = 120000;
    static constexpr uint32_t kWashMs = 20000;
    static constexpr uint32_t kBrushFrameMs = 12000;    // 10 frames in 2 minutes
    static constexpr uint32_t kWashFrameMs = 10000;     // 2 frames in 20 seconds
    static constexpr uint32_t kReadyTimeoutMs = 60000;  // nobody pressed: quietly home
    static constexpr uint32_t kPraiseMs = 5000;

    // False when a routine is already open. `silent`: quiet hours (display
    // only, no cue sounds).
    bool Open(RoutineKind kind, uint32_t now_ms, bool silent, RoutineOutput* out);
    // Dial: Press starts a Ready routine; Long = home. Up/Down do nothing
    // (nothing to choose; the child cannot lose a routine by fidgeting).
    RoutineOutput Input(GameInput input, uint32_t now_ms);
    // Call about once a second; cheap.
    RoutineOutput Tick(uint32_t now_ms);
    // BOOT talk, PWR sleep, a voice session, Pet mode off, low battery: the
    // routine just ends, no completion.
    RoutineOutput Interrupt();

    bool Active() const {
        return phase_ == RoutinePhase::Ready || phase_ == RoutinePhase::Running ||
               phase_ == RoutinePhase::Praise;
    }
    RoutineView View(uint32_t now_ms) const;
    // The care record of the last completion; true exactly once.
    bool TakeRecord(RoutineCareRecord* out);

    static uint32_t TotalMs(RoutineKind kind) { return kind == RoutineKind::Brush ? kBrushMs : kWashMs; }
    static uint32_t FrameMs(RoutineKind kind) {
        return kind == RoutineKind::Brush ? kBrushFrameMs : kWashFrameMs;
    }

private:
    uint8_t StepAt(uint32_t now_ms) const;
    RoutineOutput End(bool leave);

    RoutineKind kind_ = RoutineKind::Brush;
    RoutinePhase phase_ = RoutinePhase::Idle;
    bool silent_ = false;
    bool middle_said_ = false;
    bool record_ready_ = false;
    uint32_t opened_ms_ = 0;
    uint32_t started_ms_ = 0;
    uint32_t praise_ms_ = 0;
    uint8_t drawn_step_ = 0;
};

}  // namespace kid_app
