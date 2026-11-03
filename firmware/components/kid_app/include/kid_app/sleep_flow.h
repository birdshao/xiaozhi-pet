#pragma once

#include "kid_app/input_hub.h"

#include <cstdint>

namespace kid_app {

// "Put the pet to sleep" (PRD v0.2 §4, ADR-009/010): show the sleeping
// scene, give the child 3 s after it is on screen to change their mind,
// then power off through the PMIC (the ePaper keeps the image).
enum class SleepPhase : uint8_t { Awake, Showing, Countdown, PoweringOff };
enum class SleepAction : uint8_t { None, ShowSleepScene, RestoreScene, PowerOff };

// Single owner (board input task); uint32 ms clock, wrap-safe.
class SleepFlow {
public:
    static constexpr uint32_t kCancelWindowMs = 3000;
    // A full refresh takes ~4.3 s; if the scene never appears, give up.
    static constexpr uint32_t kShowTimeoutMs = 15000;

    SleepAction Request(uint32_t now_ms);
    // Dial or BOOT actions cancel while showing or counting down.
    SleepAction OnInput(InputKind kind, uint32_t now_ms);
    // The ePaper worker reports the sleep scene is physically displayed.
    void OnSceneShown(uint32_t now_ms);
    SleepAction Tick(uint32_t now_ms);
    // PowerOff() returned (power was not cut, e.g. on USB).
    void PowerOffReturned() { phase_ = SleepPhase::Awake; }
    // Abort before power-off (Pet mode left mid-flow). True if it aborted.
    bool Cancel() {
        if (phase_ != SleepPhase::Showing && phase_ != SleepPhase::Countdown) return false;
        phase_ = SleepPhase::Awake;
        return true;
    }

    SleepPhase Phase() const { return phase_; }
    bool Active() const { return phase_ != SleepPhase::Awake; }

private:
    SleepPhase phase_ = SleepPhase::Awake;
    uint32_t since_ms_ = 0;
};

}  // namespace kid_app
