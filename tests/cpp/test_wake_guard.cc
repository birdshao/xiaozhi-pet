#include "kid_app/wake_guard.h"

#include <cassert>
#include <iostream>

using namespace kid_app;

namespace {

CueConditions Ok() {
    CueConditions c;
    c.idle = true;
    c.phase_known = true;
    return c;
}

void RepairNeedsSettledIdleAndNotRunning() {
    WakeRepairGate g;
    // Never repairs while running or while not idle (Connecting after a wake).
    for (uint32_t t = 0; t < 5000; t += 100) {
        assert(!g.Observe(true, true, false, t));
        assert(!g.Observe(false, false, false, t));
    }
    // First idle-and-off sample only starts the clock.
    assert(!g.Observe(true, false, false, 10000));
    assert(!g.Observe(true, false, false, 10000 + WakeRepairGate::kSettleMs - 1));
    assert(g.Observe(true, false, false, 10000 + WakeRepairGate::kSettleMs));
    assert(g.Tries() == 1);
}

void RepairClockRestartsWhenInterrupted() {
    WakeRepairGate g;
    assert(!g.Observe(true, false, false, 0));
    assert(!g.Observe(false, false, false, 900));  // left idle: wake word
    assert(!g.Observe(true, false, false, 1000));  // clock restarts here
    assert(!g.Observe(true, false, false, 1999));
    assert(g.Observe(true, false, false, 2000));
    WakeRepairGate h;
    assert(!h.Observe(true, false, false, 0));
    assert(!h.Observe(true, true, false, 900));  // it came back by itself
    assert(!h.Observe(true, false, false, 1000));
    assert(!h.Observe(true, false, false, 1500));
}

void RepairIsBlockedBySleepAndRateLimited() {
    WakeRepairGate g;
    assert(!g.Observe(true, false, true, 0));
    assert(!g.Observe(true, false, true, 5000));  // blocked: never
    assert(g.Observe(true, false, false, 5001));
    assert(!g.Observe(true, false, false, 5001 + WakeRepairGate::kRetryMs - 1));
    assert(g.Observe(true, false, false, 5001 + WakeRepairGate::kRetryMs));
    uint32_t t = 5001 + WakeRepairGate::kRetryMs;
    for (int i = 2; i < WakeRepairGate::kMaxTries; ++i) {
        t += WakeRepairGate::kRetryMs;
        assert(g.Observe(true, false, false, t));
    }
    assert(g.Tries() == WakeRepairGate::kMaxTries);
    t += WakeRepairGate::kRetryMs * 4;
    assert(!g.Observe(true, false, false, t));  // gave up
    assert(!g.Observe(true, true, false, t));   // runs again: tries reset
    assert(g.Tries() == 0);
}

void CueConditionsEachBlock() {
    assert(ChatEndCueAllowed(Ok()));
    CueConditions c;
    c = Ok(); c.idle = false; assert(!ChatEndCueAllowed(c));
    c = Ok(); c.phase_known = false; assert(!ChatEndCueAllowed(c));
    c = Ok(); c.night = true; assert(!ChatEndCueAllowed(c));
    c = Ok(); c.sleeping = true; assert(!ChatEndCueAllowed(c));
    c = Ok(); c.quiet_hours = true; assert(!ChatEndCueAllowed(c));
    c = Ok(); c.power_off = true; assert(!ChatEndCueAllowed(c));
    c = Ok(); c.command_pending = true; assert(!ChatEndCueAllowed(c));
    c = Ok(); c.volume_zero = true; assert(!ChatEndCueAllowed(c));
    c = Ok(); c.audio_busy = true; assert(!ChatEndCueAllowed(c));
}

void CueFiresOnceAfterDelay() {
    ChatEndCue cue;
    cue.OnState(false, true, 0);  // plain idle: nothing armed
    assert(!cue.Armed());
    cue.OnState(true, true, 1000);  // chat -> idle
    assert(cue.Armed());
    assert(!cue.Poll(Ok(), 1000 + ChatEndCue::kDelayMs - 1));
    assert(cue.Poll(Ok(), 1000 + ChatEndCue::kDelayMs));
    assert(!cue.Armed());
    assert(!cue.Poll(Ok(), 1000 + ChatEndCue::kDelayMs + 10));  // once
}

void CueIsCancelledNotDeferred() {
    ChatEndCue cue;
    cue.OnState(true, true, 0);
    CueConditions busy = Ok();
    busy.audio_busy = true;  // the error alert's sound
    assert(!cue.Poll(busy, 100));
    assert(!cue.Armed());
    assert(!cue.Poll(Ok(), 5000));  // does not come back later
    cue.OnState(true, true, 6000);
    cue.OnState(false, false, 6100);  // chat resumed: cancelled
    assert(!cue.Armed());
    cue.OnState(true, true, 7000);
    CueConditions cmd = Ok();
    cmd.command_pending = true;  // "sleep" is about to run
    assert(!cue.Poll(cmd, 7000 + ChatEndCue::kDelayMs));
    assert(!cue.Poll(Ok(), 9000));
}

void CueClockWraps() {
    ChatEndCue cue;
    const uint32_t start = 0xFFFFFF00u;
    cue.OnState(true, true, start);
    assert(!cue.Poll(Ok(), start + 100));
    assert(cue.Poll(Ok(), start + ChatEndCue::kDelayMs));
}

}  // namespace

int main() {
    RepairNeedsSettledIdleAndNotRunning();
    RepairClockRestartsWhenInterrupted();
    RepairIsBlockedBySleepAndRateLimited();
    CueConditionsEachBlock();
    CueFiresOnceAfterDelay();
    CueIsCancelledNotDeferred();
    CueClockWraps();
    std::cout << "wake_guard ok\n";
    return 0;
}
