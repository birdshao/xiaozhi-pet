#pragma once

#include "kid_app/rhythm_scheduler.h"

#include <cstddef>
#include <cstdint>

namespace kid_app {

// What a parent may change on the device (PRD v0.2 §4 parent entry, §6, §10;
// V3-06). The day plan feeds RhythmScheduler; the volumes only scale the
// pet's own PCM (prompts, story narration) in software. XiaoZhi's codec
// volume, which XiaoZhi keeps in NVS, is never touched.
struct ParentSettings {
    static constexpr uint8_t kVolumeStepPct = 10;
    static constexpr uint8_t kMinVolumeCapPct = 10;

    DailySchedule schedule{};
    // Loudest a local clip may play, % of the clip (10..100, steps of 10).
    uint8_t volume_cap_pct = 100;
    // Ceiling during quiet hours (0..100, steps of 10; 0 = silent). The
    // default keeps the V3-01 behaviour: prompts at 30 % at night.
    uint8_t quiet_volume_pct = 30;
    // ADR-011: parents can switch the motion reactions off.
    bool motion_enabled = true;

    bool operator==(const ParentSettings& o) const {
        const DailySchedule &a = schedule, &b = o.schedule;
        return a.wake_min == b.wake_min && a.evening_min == b.evening_min &&
               a.quiet_start_min == b.quiet_start_min && a.quiet_end_min == b.quiet_end_min &&
               a.nap_enabled == b.nap_enabled && a.nap_start_min == b.nap_start_min &&
               a.nap_end_min == b.nap_end_min && volume_cap_pct == o.volume_cap_pct &&
               quiet_volume_pct == o.quiet_volume_pct && motion_enabled == o.motion_enabled;
    }
    bool operator!=(const ParentSettings& o) const { return !(*this == o); }
};

// Parent settings are untrusted input: the day plan must pass
// RhythmScheduler::Valid and the volumes their ranges and steps.
bool ValidParentSettings(const ParentSettings& settings);

// Volume (%) for a clip asked to play at `requested_pct`: capped by the
// parent, and by the quiet-hours ceiling at night. Invalid settings fall
// back to the defaults.
uint8_t LimitVolumePct(const ParentSettings& settings, uint8_t requested_pct, bool quiet);

// Stable little-endian encoding, shared by the atomic ledger (schema v3)
// and the board's own NVS key:
//   0 'P' 'S'  2 version (u16)  4 flags (bit0 nap, bit1 motion)  5 0
//   6 wake  8 evening  10 quiet start  12 quiet end  14 nap start
//   16 nap end (u16 minutes)  18 volume cap %  19 quiet volume %
//   20..23 0  24 CRC-32 of bytes 0..23
class ParentSettingsCodec {
public:
    static constexpr size_t kBytes = 28;
    static constexpr uint16_t kVersion = 1;

    // False for invalid settings, a null or short buffer.
    static bool Encode(const ParentSettings& settings, uint8_t* out, size_t size);
    // Rejects a bad tag, version or CRC, non-zero reserved bits and
    // settings that fail ValidParentSettings.
    static bool Decode(const uint8_t* bytes, size_t size, ParentSettings* out);
};

}  // namespace kid_app
