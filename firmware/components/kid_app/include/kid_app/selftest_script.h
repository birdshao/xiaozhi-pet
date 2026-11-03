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
//   mark <name>             log marker ([a-z0-9_], <= 15 chars)
//   alarm <seconds>         RTC wake-up 10..600 s after the next sleep power-off
//   end                     required: a truncated script is rejected
//
// keys: up, down, press, boot, pwr. Durations are limited to 10 minutes.
enum class SelfTestOp : uint8_t { Wait, TogglePet, Hold, Mark, Alarm };

struct SelfTestStep {
    SelfTestOp op = SelfTestOp::Wait;
    InputKey key = InputKey::DialPress;
    uint32_t ms = 0;      // wait/hold ms; seconds for Alarm
    uint32_t gap_ms = 0;  // pause after releasing the key
    char name[16] = {};   // Mark only
};

class SelfTestScript {
public:
    static constexpr size_t kMaxSteps = 64;
    static constexpr uint32_t kTapMs = 80;
    static constexpr uint32_t kDefaultGapMs = 1000;
    static constexpr uint32_t kMaxMs = 600000;

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
