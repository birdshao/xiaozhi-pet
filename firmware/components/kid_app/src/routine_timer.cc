#include "kid_app/routine_timer.h"

namespace kid_app {

const char* RoutineKindName(RoutineKind kind) {
    return kind == RoutineKind::Brush ? "brush" : "wash";
}

uint8_t RoutineTimer::StepAt(uint32_t now_ms) const {
    const uint32_t total = TotalMs(kind_);
    uint32_t elapsed = now_ms - started_ms_;
    if (elapsed > total) elapsed = total;
    return static_cast<uint8_t>(elapsed / FrameMs(kind_));
}

bool RoutineTimer::Open(RoutineKind kind, uint32_t now_ms, bool silent, RoutineOutput* out) {
    if (Active()) return false;
    kind_ = kind;
    phase_ = RoutinePhase::Ready;
    silent_ = silent;
    middle_said_ = false;
    record_ready_ = false;
    opened_ms_ = now_ms;
    drawn_step_ = 0;
    if (out) {
        *out = RoutineOutput{};
        out->redraw = true;
    }
    return true;
}

RoutineOutput RoutineTimer::End(bool leave) {
    phase_ = RoutinePhase::Done;
    RoutineOutput out;
    out.leave = leave;
    return out;
}

RoutineOutput RoutineTimer::Input(GameInput input, uint32_t now_ms) {
    RoutineOutput out;
    if (input == GameInput::Long) return Active() ? End(true) : out;
    if (input != GameInput::Press || phase_ != RoutinePhase::Ready) return out;
    phase_ = RoutinePhase::Running;
    started_ms_ = now_ms;
    drawn_step_ = 0;
    out.redraw = true;
    if (!silent_) out.cue = RoutineCue::Start;
    return out;
}

RoutineOutput RoutineTimer::Tick(uint32_t now_ms) {
    RoutineOutput out;
    switch (phase_) {
        case RoutinePhase::Ready:
            if (static_cast<uint32_t>(now_ms - opened_ms_) >= kReadyTimeoutMs) return End(true);
            break;
        case RoutinePhase::Running: {
            const uint32_t elapsed = now_ms - started_ms_;
            if (elapsed >= TotalMs(kind_)) {
                phase_ = RoutinePhase::Praise;
                praise_ms_ = now_ms;
                record_ready_ = true;
                out.completed = true;
                out.redraw = true;
                if (!silent_) out.cue = RoutineCue::Done;
                break;
            }
            if (!middle_said_ && elapsed >= TotalMs(kind_) / 2) {
                middle_said_ = true;
                if (!silent_) out.cue = RoutineCue::Middle;
            }
            // One frame per interval; a late tick coalesces into one redraw.
            const uint8_t step = StepAt(now_ms);
            if (step != drawn_step_) {
                drawn_step_ = step;
                out.redraw = true;
            }
            break;
        }
        case RoutinePhase::Praise:
            if (static_cast<uint32_t>(now_ms - praise_ms_) >= kPraiseMs) return End(true);
            break;
        case RoutinePhase::Idle:
        case RoutinePhase::Done:
            break;
    }
    return out;
}

RoutineOutput RoutineTimer::Interrupt() {
    if (!Active()) return RoutineOutput{};
    // A praise already earned stays earned; a half-done routine leaves no trace.
    return End(true);
}

RoutineView RoutineTimer::View(uint32_t now_ms) const {
    RoutineView v;
    v.kind = kind_;
    v.phase = phase_;
    v.steps = static_cast<uint8_t>(TotalMs(kind_) / FrameMs(kind_));
    if (phase_ == RoutinePhase::Running) {
        v.step = drawn_step_;
        const uint32_t elapsed = now_ms - started_ms_;
        const uint32_t total = TotalMs(kind_);
        v.seconds_left = static_cast<uint16_t>(elapsed >= total ? 0 : (total - elapsed + 999) / 1000);
    } else if (phase_ == RoutinePhase::Praise) {
        v.step = v.steps;
    } else if (phase_ == RoutinePhase::Ready) {
        v.seconds_left = static_cast<uint16_t>(TotalMs(kind_) / 1000);
    }
    return v;
}

bool RoutineTimer::TakeRecord(RoutineCareRecord* out) {
    if (!record_ready_) return false;
    record_ready_ = false;
    if (out) {
        out->kind = kind_;
        out->seconds = TotalMs(kind_) / 1000;
    }
    return true;
}

}  // namespace kid_app
