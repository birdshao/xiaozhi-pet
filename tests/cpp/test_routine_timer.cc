#include "kid_app/routine_timer.h"

#include <cassert>
#include <iostream>

using namespace kid_app;

namespace {
struct Run {
    int redraws = 0;
    int cues = 0;
    int middle = 0;
    int completed = 0;
    uint32_t last_redraw = 0;
    uint32_t min_gap = 0xFFFFFFFFu;
    void Add(const RoutineOutput& o, uint32_t t) {
        if (o.redraw) {
            ++redraws;
            if (last_redraw && t - last_redraw < min_gap) min_gap = t - last_redraw;
            last_redraw = t;
        }
        if (o.cue != RoutineCue::None) ++cues;
        if (o.cue == RoutineCue::Middle) ++middle;
        if (o.completed) ++completed;
    }
};

// Open at t=0, press at t=1000, tick once a second for `until_ms`.
Run Play(RoutineTimer& r, RoutineKind kind, bool silent, uint32_t until_ms) {
    Run run;
    RoutineOutput o;
    assert(r.Open(kind, 0, silent, &o) && o.redraw && r.View(0).phase == RoutinePhase::Ready);
    o = r.Input(GameInput::Press, 1000);
    run.Add(o, 1000);
    for (uint32_t t = 2000; t <= until_ms; t += 1000) run.Add(r.Tick(t), t);
    return run;
}
}  // namespace

int main() {
    // Lengths and frame rate: 2 minutes / 20 seconds; a frame every 10-15 s.
    static_assert(RoutineTimer::kBrushMs == 120000 && RoutineTimer::kWashMs == 20000, "lengths");
    static_assert(RoutineTimer::kBrushFrameMs >= 10000 && RoutineTimer::kBrushFrameMs <= 15000, "gate");
    static_assert(RoutineTimer::kWashFrameMs >= 10000 && RoutineTimer::kWashFrameMs <= 15000, "gate");

    // Brushing: Ready -> press -> 120 s -> praise once; "刷刷里面" once in the middle.
    {
        RoutineTimer r;
        const Run run = Play(r, RoutineKind::Brush, false, 1000 + RoutineTimer::kBrushMs + 100);
        assert(run.completed == 1);
        assert(run.middle == 1);
        assert(run.cues == 3);  // start, middle, done
        // press redraw + 10 sand frames (the last one is the completion frame) -> never faster than 12 s.
        assert(run.min_gap >= RoutineTimer::kBrushFrameMs);
        assert(run.redraws == 1 + 10);
        RoutineCareRecord rec;
        assert(r.TakeRecord(&rec) && rec.kind == RoutineKind::Brush && rec.seconds == 120);
        assert(!r.TakeRecord(&rec));  // exactly once
        assert(r.View(200000).phase == RoutinePhase::Praise && r.View(200000).step == r.View(200000).steps);
    }

    // The middle cue is the half-way point, not earlier.
    {
        RoutineTimer r;
        r.Open(RoutineKind::Brush, 0, false, nullptr);
        r.Input(GameInput::Press, 0);
        assert(r.Tick(59999).cue == RoutineCue::None);
        assert(r.Tick(60000).cue == RoutineCue::Middle);
        assert(r.Tick(60001).cue == RoutineCue::None);
    }

    // Washing: 20 s, frame every 10 s.
    {
        RoutineTimer r;
        const Run run = Play(r, RoutineKind::Wash, false, 1000 + RoutineTimer::kWashMs + 100);
        assert(run.completed == 1 && run.cues == 3 && run.middle == 1);
        assert(run.min_gap >= RoutineTimer::kWashFrameMs);
        RoutineCareRecord rec;
        assert(r.TakeRecord(&rec) && rec.kind == RoutineKind::Wash && rec.seconds == 20);
    }

    // Quiet hours: the picture runs, nothing is spoken, the care record still counts.
    {
        RoutineTimer r;
        const Run run = Play(r, RoutineKind::Brush, true, 1000 + RoutineTimer::kBrushMs + 100);
        assert(run.cues == 0 && run.completed == 1 && run.redraws == 11);
        assert(r.TakeRecord(nullptr));
    }

    // It only RUNS after a physical dial press (voice/rail only open the Ready screen):
    // without a press there is no completion and no record, and Ready times out quietly.
    {
        RoutineTimer r;
        r.Open(RoutineKind::Brush, 0, false, nullptr);
        for (uint32_t t = 1000; t < RoutineTimer::kReadyTimeoutMs; t += 1000) {
            const RoutineOutput o = r.Tick(t);
            assert(!o.completed && !o.leave && !o.redraw);
            assert(r.Input(GameInput::Up, t).redraw == false && r.Input(GameInput::Down, t).cue == RoutineCue::None);
        }
        const RoutineOutput o = r.Tick(RoutineTimer::kReadyTimeoutMs);
        assert(o.leave && !o.completed && !r.Active());
        assert(!r.TakeRecord(nullptr));
    }

    // Stopping early: no record, no completion, nothing scolds. Up/Down never cancel.
    {
        RoutineTimer r;
        r.Open(RoutineKind::Brush, 0, false, nullptr);
        r.Input(GameInput::Press, 0);
        assert(!r.Input(GameInput::Up, 5000).leave && !r.Input(GameInput::Down, 5000).leave);
        assert(!r.Input(GameInput::Press, 5000).redraw);  // a second press does not restart
        RoutineOutput o = r.Input(GameInput::Long, 30000);  // dial hold = home
        assert(o.leave && !o.completed && !r.Active());
        assert(!r.TakeRecord(nullptr));
        assert(!r.Tick(200000).completed);  // and nothing completes later by itself
    }

    // Interrupt (BOOT talk / PWR sleep / voice / low battery): same, from every phase.
    {
        RoutineTimer r;
        assert(!r.Interrupt().leave);  // idle
        r.Open(RoutineKind::Wash, 0, false, nullptr);
        assert(r.Interrupt().leave && !r.Active());  // ready
        r.Open(RoutineKind::Wash, 0, false, nullptr);
        r.Input(GameInput::Press, 0);
        assert(r.Interrupt().leave && !r.TakeRecord(nullptr));  // running: no record
        // A praise already earned stays earned even if BOOT is pressed during it.
        r.Open(RoutineKind::Wash, 0, false, nullptr);
        r.Input(GameInput::Press, 0);
        assert(r.Tick(20000).completed);
        assert(r.Interrupt().leave);
        assert(r.TakeRecord(nullptr));
    }

    // Praise lasts kPraiseMs, then home; a second Open is refused while active.
    {
        RoutineTimer r;
        r.Open(RoutineKind::Wash, 0, false, nullptr);
        assert(!r.Open(RoutineKind::Brush, 0, false, nullptr));
        r.Input(GameInput::Press, 0);
        assert(r.Tick(20000).completed);
        assert(!r.Tick(20000 + RoutineTimer::kPraiseMs - 1).leave);
        assert(r.Tick(20000 + RoutineTimer::kPraiseMs).leave && !r.Active());
        assert(r.Open(RoutineKind::Brush, 30000, false, nullptr));  // free again
    }

    // A late tick (e.g. the display was busy) coalesces into one redraw and does not skip the end.
    {
        RoutineTimer r;
        r.Open(RoutineKind::Brush, 0, false, nullptr);
        r.Input(GameInput::Press, 0);
        RoutineOutput o = r.Tick(50000);  // 4 frames late
        assert(o.redraw && r.View(50000).step == 4);
        assert(!r.Tick(50500).redraw);
        o = r.Tick(500000);  // hours later: completes once
        assert(o.completed);
        assert(!r.Tick(500001).completed);
    }

    // uint32 wrap-around of the millisecond clock.
    {
        RoutineTimer r;
        const uint32_t t0 = 0xFFFFFFFFu - 30000u;
        r.Open(RoutineKind::Brush, t0, false, nullptr);
        r.Input(GameInput::Press, t0);
        assert(!r.Tick(t0 + 119999u).completed);
        assert(r.Tick(t0 + 120000u).completed);
    }

    // Clip ids: brush 1..3, wash 11..13 (inside the reviewed `routines` pack).
    assert(RoutineClipId(RoutineKind::Brush, RoutineCue::Start) == 1);
    assert(RoutineClipId(RoutineKind::Brush, RoutineCue::Done) == 3);
    assert(RoutineClipId(RoutineKind::Wash, RoutineCue::Middle) == 12);

    std::cout << "routine timer tests passed\n";
    return 0;
}
