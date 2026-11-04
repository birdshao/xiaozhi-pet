#include "kid_app/battery_policy.h"
#include "kid_app/selftest_clock.h"

namespace kid_app {

BatteryPolicy::Band BatteryPolicy::Wanted(int pct) const {
    switch (band_) {
        case Band::Normal:
            if (pct < kCriticalPct) return Band::Critical;
            return pct < kLowPct ? Band::Low : Band::Normal;
        case Band::Low:
            if (pct < kCriticalPct) return Band::Critical;
            return pct >= kLowClearPct ? Band::Normal : Band::Low;
        case Band::Critical:
            if (pct >= kLowClearPct) return Band::Normal;
            return pct >= kCriticalClearPct ? Band::Low : Band::Critical;
    }
    return band_;
}

void BatteryPolicy::Schedule(BatteryReaction kind, uint32_t due_ms) {
    next_ = kind;
    due_ms_ = due_ms;
}

BatteryEvent BatteryPolicy::Update(const BatteryReading& reading, uint32_t now_ms) {
    const bool valid = reading.pct >= 0 && reading.pct <= 100;
    // No level and no flags: there is no PMIC answer at all.
    if (!valid && !reading.charging && !reading.on_battery) return BatteryEvent::None;

    if (valid) {
        pct_ = reading.pct;
        have_level_ = true;
        const Band want = Wanted(reading.pct);
        if (want == band_) {
            band_count_ = 0;
        } else {
            if (want != band_candidate_) {
                band_candidate_ = want;
                band_count_ = 0;
            }
            if (++band_count_ >= kLevelReadings) {
                band_ = want;
                band_count_ = 0;
            }
        }
    }

    const Source raw = reading.on_battery ? Source::Battery
                       : reading.charging ? Source::Charging
                                          : Source::Full;
    const Source previous = source_;
    if (source_ == Source::Unknown) {
        source_ = raw;  // first reading after boot: no event
    } else if (raw == source_) {
        source_count_ = 0;
    } else {
        if (raw != source_candidate_) {
            source_candidate_ = raw;
            source_count_ = 0;
        }
        if (++source_count_ >= kSourceReadings) {
            source_ = raw;
            source_count_ = 0;
        }
    }

    BatteryEvent event = BatteryEvent::None;
    if (previous != Source::Unknown && source_ != previous) {
        const bool was_external = previous != Source::Battery;
        if (!External()) {
            event = BatteryEvent::Unplugged;
        } else if (!was_external) {
            event = BatteryEvent::Plugged;
            announced_ = Band::Normal;  // unplugged while low: remind again
            if (source_ == Source::Charging) Schedule(BatteryReaction::Eat, now_ms);
        } else if (source_ == Source::Full) {
            event = BatteryEvent::Full;
        }
    } else if (source_ == Source::Battery && have_level_) {
        if (band_ != announced_) {
            const Band was = announced_;
            announced_ = band_;
            if (band_ == Band::Critical) {
                critical_ms_ = now_ms;
                event = BatteryEvent::Critical;
            } else if (band_ == Band::Low && was == Band::Normal) {
                event = BatteryEvent::EnteredLow;
                Schedule(BatteryReaction::Yawn, now_ms);
            }
        } else if (band_ == Band::Critical &&
                   Reached(now_ms, critical_ms_ + SelfTestClock::Scaled(kCriticalRetryMs))) {
            critical_ms_ = now_ms;
            event = BatteryEvent::Critical;
        }
    }

    // Reactions follow the state; an event above may have made one due now.
    const BatteryState state = State();
    if (state == BatteryState::Charging) {
        if (next_ != BatteryReaction::Eat)
            Schedule(BatteryReaction::Eat, now_ms + SelfTestClock::Scaled(kEatEveryMs));
    } else if (state == BatteryState::Low) {
        if (next_ != BatteryReaction::Yawn)
            Schedule(BatteryReaction::Yawn, now_ms + SelfTestClock::Scaled(kYawnEveryMs));
    } else {
        next_ = BatteryReaction::None;
    }
    return event;
}

BatteryReaction BatteryPolicy::Reaction(uint32_t now_ms, bool allowed) {
    if (next_ == BatteryReaction::None || !allowed || !Reached(now_ms, due_ms_))
        return BatteryReaction::None;
    const BatteryReaction reaction = next_;
    due_ms_ = now_ms + SelfTestClock::Scaled(reaction == BatteryReaction::Eat ? kEatEveryMs
                                                                           : kYawnEveryMs);
    return reaction;
}

BatteryState BatteryPolicy::State() const {
    switch (source_) {
        case Source::Unknown: return BatteryState::Unknown;
        case Source::Charging: return BatteryState::Charging;
        case Source::Full: return BatteryState::Full;
        case Source::Battery: break;
    }
    if (!have_level_) return BatteryState::Unknown;
    if (band_ == Band::Critical) return BatteryState::Critical;
    return band_ == Band::Low ? BatteryState::Low : BatteryState::Normal;
}

const char* BatteryStateName(BatteryState state) {
    switch (state) {
        case BatteryState::Normal: return "normal";
        case BatteryState::Low: return "low";
        case BatteryState::Critical: return "critical";
        case BatteryState::Charging: return "charging";
        case BatteryState::Full: return "full";
        case BatteryState::Unknown: break;
    }
    return "unknown";
}

const char* BatteryEventName(BatteryEvent event) {
    switch (event) {
        case BatteryEvent::Plugged: return "plugged";
        case BatteryEvent::Unplugged: return "unplugged";
        case BatteryEvent::Full: return "full";
        case BatteryEvent::EnteredLow: return "low";
        case BatteryEvent::Critical: return "critical";
        case BatteryEvent::None: break;
    }
    return "none";
}

const char* BatteryReactionName(BatteryReaction reaction) {
    switch (reaction) {
        case BatteryReaction::Eat: return "eat";
        case BatteryReaction::Yawn: return "yawn";
        case BatteryReaction::None: break;
    }
    return "none";
}

uint32_t PackBatteryReading(const BatteryReading& reading) {
    const uint32_t pct = reading.pct >= 0 && reading.pct <= 100
                             ? static_cast<uint32_t>(reading.pct)
                             : 0xFFu;
    return pct | (reading.charging ? 1u << 8 : 0u) | (reading.on_battery ? 1u << 9 : 0u) |
           (1u << 10);
}

BatteryReading UnpackBatteryReading(uint32_t bits) {
    BatteryReading reading;
    const uint32_t pct = bits & 0xFFu;
    reading.pct = pct > 100 ? -1 : static_cast<int>(pct);
    reading.charging = ((bits >> 8) & 1u) != 0;
    reading.on_battery = ((bits >> 9) & 1u) != 0;
    return reading;
}

}  // namespace kid_app
