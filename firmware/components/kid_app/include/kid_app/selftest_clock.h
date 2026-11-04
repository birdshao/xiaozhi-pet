#pragma once

#include <atomic>
#include <cstdint>

namespace kid_app {

// TEST ONLY: time scale for scripted hardware self-tests (PETSELFTEST1
// "timescale <percent>"). 100 % is real time: the value at every boot and
// the only one a child's device ever sees. Only the board's
// MaybeRunSelfTest sets it (on ota_1, with a valid script; a contract test
// in tests/test_p1_integration_contract.py keeps it that way) and puts it
// back to 100 % when the script ends. RAM only: a reboot is real time.
//
// Feature timers that only exist to wait for a person (idle blink, cleanup
// after idle, power idle/nap, the morning window, the parent menu idle
// exit, the mini game repeat/idle exit, battery sampling and reaction
// periods) wrap their duration in
// SelfTestClock::Scaled(). Never wrap refresh physics (EPD busy waits,
// animation frame/settle times), audio playback, the sleep cancel window
// (a child's chance to change their mind, counted from the panel) or
// wall-clock schedules (status band minute, RTC alarms).
//
//   if (now_ms - since_ms >= SelfTestClock::Scaled(kIdleExitMs)) ...
//
// Lock-free, callable from any task.
class SelfTestClock {
public:
    static constexpr uint32_t kRealPct = 100;
    static constexpr uint32_t kMinPct = 10;

    static uint32_t Pct() { return pct_.load(std::memory_order_relaxed); }
    static bool Scaling() { return Pct() != kRealPct; }

    // `ms` at the current scale; never 0 for a non-zero duration.
    static uint32_t Scaled(uint32_t ms) {
        const uint32_t pct = Pct();
        if (pct == kRealPct || ms == 0) return ms;
        const uint64_t scaled = static_cast<uint64_t>(ms) * pct / kRealPct;
        return scaled ? static_cast<uint32_t>(scaled) : 1u;
    }

    // MaybeRunSelfTest only. Out of kMinPct..kRealPct: refused, unchanged.
    static bool SetForSelfTest(uint32_t pct) {
        if (pct < kMinPct || pct > kRealPct) return false;
        pct_.store(static_cast<uint8_t>(pct), std::memory_order_relaxed);
        return true;
    }

private:
    static inline std::atomic<uint8_t> pct_{kRealPct};
};

}  // namespace kid_app
