#include "kid_app/parent_settings.h"

namespace kid_app {

namespace {
constexpr size_t kCrcAt = 24;
constexpr uint8_t kFlagNap = 0x01, kFlagMotion = 0x02;

bool ValidVolume(uint8_t pct, uint8_t min) {
    return pct >= min && pct <= 100 && pct % ParentSettings::kVolumeStepPct == 0;
}

void Put16(uint8_t* b, size_t p, uint16_t n) {
    b[p] = static_cast<uint8_t>(n);
    b[p + 1] = static_cast<uint8_t>(n >> 8);
}
uint16_t Get16(const uint8_t* b, size_t p) {
    return static_cast<uint16_t>(b[p] | (b[p + 1] << 8));
}
uint32_t Crc32(const uint8_t* b, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        crc ^= b[i];
        for (unsigned j = 0; j < 8; ++j) crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0);
    }
    return ~crc;
}
}  // namespace

bool ValidParentSettings(const ParentSettings& s) {
    return RhythmScheduler::Valid(s.schedule) &&
           ValidVolume(s.volume_cap_pct, ParentSettings::kMinVolumeCapPct) &&
           ValidVolume(s.quiet_volume_pct, 0);
}

uint8_t LimitVolumePct(const ParentSettings& settings, uint8_t requested_pct, bool quiet) {
    const ParentSettings s = ValidParentSettings(settings) ? settings : ParentSettings{};
    uint8_t limit = s.volume_cap_pct;
    if (quiet && s.quiet_volume_pct < limit) limit = s.quiet_volume_pct;
    const unsigned requested = requested_pct > 100 ? 100u : requested_pct;
    return static_cast<uint8_t>(requested * limit / 100);
}

bool ParentSettingsCodec::Encode(const ParentSettings& s, uint8_t* out, size_t size) {
    if (!out || size < kBytes || !ValidParentSettings(s)) return false;
    uint8_t b[kBytes] = {};
    b[0] = 'P';
    b[1] = 'S';
    Put16(b, 2, kVersion);
    b[4] = static_cast<uint8_t>((s.schedule.nap_enabled ? kFlagNap : 0) |
                                (s.motion_enabled ? kFlagMotion : 0));
    Put16(b, 6, s.schedule.wake_min);
    Put16(b, 8, s.schedule.evening_min);
    Put16(b, 10, s.schedule.quiet_start_min);
    Put16(b, 12, s.schedule.quiet_end_min);
    Put16(b, 14, s.schedule.nap_start_min);
    Put16(b, 16, s.schedule.nap_end_min);
    b[18] = s.volume_cap_pct;
    b[19] = s.quiet_volume_pct;
    const uint32_t crc = Crc32(b, kCrcAt);
    for (size_t i = 0; i < 4; ++i) b[kCrcAt + i] = static_cast<uint8_t>(crc >> (8 * i));
    for (size_t i = 0; i < kBytes; ++i) out[i] = b[i];
    return true;
}

bool ParentSettingsCodec::Decode(const uint8_t* b, size_t size, ParentSettings* out) {
    if (!b || !out || size < kBytes || b[0] != 'P' || b[1] != 'S' || Get16(b, 2) != kVersion ||
        (b[4] & ~(kFlagNap | kFlagMotion)) || b[5])
        return false;
    for (size_t i = 20; i < kCrcAt; ++i)
        if (b[i]) return false;
    uint32_t crc = 0;
    for (size_t i = 0; i < 4; ++i) crc |= static_cast<uint32_t>(b[kCrcAt + i]) << (8 * i);
    if (crc != Crc32(b, kCrcAt)) return false;
    ParentSettings s;
    s.schedule.nap_enabled = (b[4] & kFlagNap) != 0;
    s.motion_enabled = (b[4] & kFlagMotion) != 0;
    s.schedule.wake_min = Get16(b, 6);
    s.schedule.evening_min = Get16(b, 8);
    s.schedule.quiet_start_min = Get16(b, 10);
    s.schedule.quiet_end_min = Get16(b, 12);
    s.schedule.nap_start_min = Get16(b, 14);
    s.schedule.nap_end_min = Get16(b, 16);
    s.volume_cap_pct = b[18];
    s.quiet_volume_pct = b[19];
    if (!ValidParentSettings(s)) return false;
    *out = s;
    return true;
}

}  // namespace kid_app
