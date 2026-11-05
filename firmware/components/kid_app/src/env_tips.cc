#include "kid_app/env_tips.h"

#include "kid_app/selftest_clock.h"

namespace kid_app {

EnvZone EnvTipPolicy::Wanted(int room_c10) const {
    // Inside an overlap band the current zone stays (hysteresis).
    switch (zone_) {
        case EnvZone::Hot:
            if (room_c10 >= kHotOffC10) return EnvZone::Hot;
            break;
        case EnvZone::Cold:
            if (room_c10 <= kColdOffC10) return EnvZone::Cold;
            break;
        default:
            break;
    }
    if (room_c10 >= kHotOnC10) return EnvZone::Hot;
    if (room_c10 <= kColdOnC10) return EnvZone::Cold;
    return EnvZone::Normal;
}

void EnvTipPolicy::Raise(EnvTip tip, uint32_t now_ms) {
    pending_ = tip;
    pending_ms_ = now_ms;
    repeat_ms_ = now_ms + SelfTestClock::Scaled(kRepeatMs);
}

EnvTip EnvTipPolicy::Observe(bool ok, int temp_c10, uint32_t now_ms) {
    if (!ok) {
        candidate_ = EnvZone::Unknown;
        return EnvTip::None;
    }
    room_c10_ = temp_c10 - offset_c10_;
    const EnvZone wanted = Wanted(room_c10_);
    if (zone_ == EnvZone::Unknown) {  // the first reading sets the zone silently
        zone_ = wanted;
        candidate_ = wanted;
        repeat_ms_ = now_ms + SelfTestClock::Scaled(kRepeatMs);
        return EnvTip::None;
    }
    if (wanted == zone_) {
        candidate_ = zone_;
        // Still hot / cold: a reminder comes due after kRepeatMs.
        if (zone_ != EnvZone::Normal && pending_ == EnvTip::None && Reached(now_ms, repeat_ms_)) {
            Raise(zone_ == EnvZone::Hot ? EnvTip::Hot : EnvTip::Cold, now_ms);
            return pending_;
        }
        return EnvTip::None;
    }
    if (wanted != candidate_) {
        candidate_ = wanted;
        candidate_ms_ = now_ms;
        return EnvTip::None;
    }
    if (!Reached(now_ms, candidate_ms_ + SelfTestClock::Scaled(kHoldMs))) return EnvTip::None;
    zone_ = wanted;
    if (zone_ == EnvZone::Normal) {
        pending_ = EnvTip::None;  // the room recovered before the tip was given
        return EnvTip::None;
    }
    Raise(zone_ == EnvZone::Hot ? EnvTip::Hot : EnvTip::Cold, now_ms);
    return pending_;
}

EnvTip EnvTipPolicy::Take(bool quiet, bool allowed, uint32_t day, uint32_t now_ms) {
    if (pending_ == EnvTip::None) return EnvTip::None;
    if (Reached(now_ms, pending_ms_ + SelfTestClock::Scaled(kPendingMs))) {
        pending_ = EnvTip::None;  // stale: waited too long (quiet hours, busy)
        return EnvTip::None;
    }
    if (quiet || !allowed) return EnvTip::None;
    if (day != day_) {
        day_ = day;
        today_count_ = 0;
    }
    if (today_count_ >= kDailyLimit) {
        pending_ = EnvTip::None;  // drop; the next reminder comes after kRepeatMs
        return EnvTip::None;
    }
    ++today_count_;
    const EnvTip tip = pending_;
    pending_ = EnvTip::None;
    return tip;
}

const char* EnvTipName(EnvTip tip) {
    switch (tip) {
        case EnvTip::Hot: return "hot";
        case EnvTip::Cold: return "cold";
        case EnvTip::None: break;
    }
    return "none";
}

const char* EnvZoneName(EnvZone zone) {
    switch (zone) {
        case EnvZone::Normal: return "normal";
        case EnvZone::Hot: return "hot";
        case EnvZone::Cold: return "cold";
        case EnvZone::Unknown: break;
    }
    return "unknown";
}

}  // namespace kid_app
