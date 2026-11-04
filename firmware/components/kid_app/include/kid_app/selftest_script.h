#pragma once

#include "kid_app/input_hub.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace kid_app {

// Host-authored scripted key presses for attended-free hardware trials.
// The script lives in the last 4 KiB sector of the running ota_1 slot and
// is written/erased by the host; without a valid script nothing happens.
//
//   PETSELFTEST1            header, version 1
//   # comment
//   wait <ms>               pause
//   pet                     toggle Pet mode (as a BOOT long press would)
//   tap <key> [gap_ms]      press 80 ms, release, then gap (default 1000)
//   hold <key> <ms> [gap]   press <ms>, release, then gap (default 1000)
//   combo <ms> [gap]        BOOT and dial press together for <ms> (the V3-06
//                           parent entry needs 5000), release both, then gap
//   mark <name>             log marker ([a-z0-9_], <= 15 chars)
//   alarm <seconds>         RTC wake-up 10..600 s after the next sleep power-off
//   volume <percent>        prompt volume 0..100 for the rest of this boot (RAM only)
//   gesture <name>          as if the IMU detected it: pickup, gentle, hard,
//                           facedown, faceup (tests the wiring, not the thresholds)
//   morning                 as after an RTC-alarm power-on (V3-04): Pet mode,
//                           good-morning picture, back to sleep after 60 s
//                           without input. Only after an "alarm" step, so the
//                           power-off it leads to always wakes the board again.
//   voice <phase>           as if XiaoZhi were: listen, think, speak; off hands
//                           the voice face back to XiaoZhi's real state (V6-01)
//   battery <pct> <0|1>     PMIC readings from now on (V3-05): level 0..100,
//                           1 = charging on USB, 0 = on battery; drives the
//                           status band and the battery policy. A critical
//                           level on battery (< 8 %) powers off, so it is
//                           only accepted after an "alarm" step.
//   battery off             back to the real PMIC readings
//   timescale <percent>     TEST ONLY: feature timers (idle blink, cleanup,
//                           power idle/nap, morning window, parent idle exit,
//                           battery sampling/reactions) run at 10..100 % of
//                           their real length until the script ends (RAM
//                           only, see selftest_clock.h); 100 = real time
//   twait <ms>              wait for one of those timers: <ms> as at real
//                           time, scaled by "timescale" (refreshes, sounds
//                           and animations around it go in plain "wait"s),
//                           so one scenario runs at any scale
//   waitidle <timeout_ms>   wait until XiaoZhi is idle (Wi-Fi up, activated)
//                           for 1 s, at most timeout_ms; then go on anyway
//   end                     required: a truncated script is rejected
//
// keys: up, down, press, boot, pwr. Durations are limited to 10 minutes.
enum class SelfTestOp : uint8_t {
    Wait, TogglePet, Hold, Mark, Alarm, Volume, Gesture, Morning, Voice, Battery,
    Combo, TimeScale, WaitIdle, TimerWait
};

struct SelfTestStep {
    SelfTestOp op = SelfTestOp::Wait;
    InputKey key = InputKey::DialPress;
    uint32_t ms = 0;      // wait/hold/combo ms; seconds for Alarm; percent for Volume
                          // and TimeScale; timeout ms for WaitIdle; real-time
                          // ms for TimerWait (scaled when it runs);
                          // kid_app::Gesture value for Gesture;
                          // kid_app::VoiceFace value for Voice (None = off);
                          // Battery: percent | kBatteryCharging, or kBatteryOff
    uint32_t gap_ms = 0;  // pause after releasing the key
    char name[16] = {};   // Mark only
};

class SelfTestScript {
public:
    static constexpr size_t kMaxSteps = 64;
    static constexpr uint32_t kTapMs = 80;
    static constexpr uint32_t kDefaultGapMs = 1000;
    static constexpr uint32_t kMaxMs = 600000;
    static constexpr uint32_t kBatteryCharging = 0x100;
    static constexpr uint32_t kBatteryOff = 0x200;
    static constexpr uint32_t kMinTimeScalePct = 10;  // SelfTestClock::kMinPct
    static constexpr uint32_t kIdleSettleMs = 1000;   // WaitIdle: idle this long

    // Text must be NUL-terminated. On failure no steps are kept.
    bool Parse(const char* text);
    // Raw flash sector: text ends at the first 0x00 or erased 0xFF byte.
    bool ParseSector(const uint8_t* sector, size_t size);

    size_t Count() const { return count_; }
    const std::array<SelfTestStep, kMaxSteps>& Steps() const { return steps_; }
    uint64_t TotalMs() const;

private:
    std::array<SelfTestStep, kMaxSteps> steps_{};
    size_t count_ = 0;
};

}  // namespace kid_app
