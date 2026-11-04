#include "kid_app/battery_policy.h"

#include <cassert>
#include <cstring>
#include <iostream>

using namespace kid_app;

namespace {

constexpr uint32_t kStep = 2000;  // the board samples the PMIC every 2 s

BatteryReading OnBattery(int pct) {
    BatteryReading r;
    r.pct = pct;
    r.on_battery = true;
    return r;
}

BatteryReading Charging(int pct) {
    BatteryReading r;
    r.pct = pct;
    r.charging = true;
    return r;
}

BatteryReading Full(int pct) {
    BatteryReading r;
    r.pct = pct;
    return r;
}

// Feeds `reading` every kStep from *now, up to `count` times; returns the
// first event (the clock stops right after it).
BatteryEvent Feed(BatteryPolicy& p, uint32_t* now, const BatteryReading& reading, int count) {
    for (int i = 0; i < count; ++i) {
        *now += kStep;
        const BatteryEvent e = p.Update(reading, *now);
        if (e != BatteryEvent::None) return e;
    }
    return BatteryEvent::None;
}

}  // namespace

int main() {
    using BP = BatteryPolicy;
    static_assert(BP::kCriticalPct == 8, "V3-05: safe shutdown below 8 %");
    static_assert(BP::kCriticalPct < BP::kCriticalClearPct && BP::kCriticalClearPct < BP::kLowPct &&
                      BP::kLowPct < BP::kLowClearPct,
                  "bands overlap (hysteresis)");

    // Nothing known before the first reading; a reading without level and
    // flags (no PMIC) changes nothing.
    {
        BatteryPolicy p;
        assert(p.State() == BatteryState::Unknown && p.Percent() == -1);
        assert(!p.LowPower() && !p.External());
        assert(p.Update(BatteryReading{}, 1000) == BatteryEvent::None);
        assert(p.State() == BatteryState::Unknown);
        assert(p.Reaction(1000, true) == BatteryReaction::None);
    }

    // Boot on USB: charging at once, no "plugged" (every trial runs on USB).
    {
        BatteryPolicy p;
        uint32_t now = 0;
        assert(Feed(p, &now, Charging(78), 5) == BatteryEvent::None);
        assert(p.State() == BatteryState::Charging && p.External() && p.Percent() == 78);
        // No eating right away either; it comes every kEatEveryMs.
        assert(p.Reaction(now, true) == BatteryReaction::None);
    }

    // Plug in: debounced, then "plugged" and TuanTuan eats at once (only
    // when allowed), then again every kEatEveryMs while charging.
    {
        BatteryPolicy p;
        uint32_t now = 0;
        Feed(p, &now, OnBattery(50), 3);
        assert(p.State() == BatteryState::Normal && !p.External());
        // One odd sample is jitter, not a plug-in.
        now += kStep;
        assert(p.Update(Charging(50), now) == BatteryEvent::None);
        assert(p.State() == BatteryState::Normal);
        now += kStep;
        assert(p.Update(OnBattery(50), now) == BatteryEvent::None);
        now += kStep;
        assert(p.Update(Charging(50), now) == BatteryEvent::None);
        now += kStep;
        assert(p.Update(Charging(50), now) == BatteryEvent::Plugged);
        assert(p.State() == BatteryState::Charging && p.External());
        assert(p.Reaction(now, false) == BatteryReaction::None);  // e.g. voice session
        assert(p.Reaction(now + 5000, true) == BatteryReaction::Eat);
        assert(p.Reaction(now + 5000, true) == BatteryReaction::None);  // once
        const uint32_t ate = now + 5000;
        assert(p.Reaction(ate + BP::kEatEveryMs - 1, true) == BatteryReaction::None);
        assert(p.Reaction(ate + BP::kEatEveryMs, true) == BatteryReaction::Eat);
        // Charging done: "full", no more eating.
        now = ate + BP::kEatEveryMs;
        assert(Feed(p, &now, Full(100), 2) == BatteryEvent::Full);
        assert(p.State() == BatteryState::Full && p.External());
        assert(p.Reaction(now + BP::kEatEveryMs * 2, true) == BatteryReaction::None);
        // Topping up again is not a new plug-in.
        assert(Feed(p, &now, Charging(99), 3) == BatteryEvent::None);
        assert(p.State() == BatteryState::Charging);
        // Unplug: debounced the same way.
        now += kStep;
        assert(p.Update(OnBattery(99), now) == BatteryEvent::None);
        now += kStep;
        assert(p.Update(OnBattery(99), now) == BatteryEvent::Unplugged);
        assert(p.State() == BatteryState::Normal && !p.External());
        assert(p.Reaction(now + BP::kEatEveryMs * 2, true) == BatteryReaction::None);
    }

    // Plugging in a full battery: plugged, but nothing to eat.
    {
        BatteryPolicy p;
        uint32_t now = 0;
        Feed(p, &now, OnBattery(100), 2);
        assert(Feed(p, &now, Full(100), 3) == BatteryEvent::Plugged);
        assert(p.State() == BatteryState::Full);
        assert(p.Reaction(now, true) == BatteryReaction::None);
    }

    // Low: three readings below 20 %, a yawn at once and every
    // kYawnEveryMs; jitter around the threshold does not flap.
    {
        BatteryPolicy p;
        uint32_t now = 0;
        Feed(p, &now, OnBattery(21), 3);
        assert(p.State() == BatteryState::Normal);
        // 19, 20, 19, 20: never three in a row below 20 %.
        for (int i = 0; i < 6; ++i) {
            now += kStep;
            assert(p.Update(OnBattery(i % 2 ? 20 : 19), now) == BatteryEvent::None);
        }
        assert(p.State() == BatteryState::Normal && !p.LowPower());
        now += kStep;
        assert(p.Update(OnBattery(19), now) == BatteryEvent::None);
        now += kStep;
        assert(p.Update(OnBattery(18), now) == BatteryEvent::None);
        now += kStep;
        assert(p.Update(OnBattery(19), now) == BatteryEvent::EnteredLow);
        assert(p.State() == BatteryState::Low && p.LowPower());
        assert(p.Reaction(now, true) == BatteryReaction::Yawn);
        assert(p.Reaction(now + 1000, true) == BatteryReaction::None);
        // Rate-limited: due again only after kYawnEveryMs, and waits for
        // an allowed moment.
        assert(p.Reaction(now + BP::kYawnEveryMs - 1, true) == BatteryReaction::None);
        assert(p.Reaction(now + BP::kYawnEveryMs, false) == BatteryReaction::None);
        assert(p.Reaction(now + BP::kYawnEveryMs + 9000, true) == BatteryReaction::Yawn);
        // Back up to 20..24 %: still low (hysteresis); no second event.
        assert(Feed(p, &now, OnBattery(24), 6) == BatteryEvent::None);
        assert(p.State() == BatteryState::Low);
        // 25 % (e.g. the load went away): normal again, no yawns.
        assert(Feed(p, &now, OnBattery(25), 3) == BatteryEvent::None);
        assert(p.State() == BatteryState::Normal);
        assert(p.Reaction(now + BP::kYawnEveryMs * 3, true) == BatteryReaction::None);
    }

    // Critical below 8 %: one event, repeated every kCriticalRetryMs while
    // still critical (sleep cancelled / PMIC did not cut power); never on
    // USB power; no yawn (the board goes to sleep instead).
    {
        BatteryPolicy p;
        uint32_t now = 0;
        assert(Feed(p, &now, OnBattery(15), 3) == BatteryEvent::EnteredLow);
        assert(p.State() == BatteryState::Low);
        // 8 % is not critical yet.
        assert(Feed(p, &now, OnBattery(8), 5) == BatteryEvent::None);
        now += kStep;
        assert(p.Update(OnBattery(7), now) == BatteryEvent::None);
        now += kStep;
        assert(p.Update(OnBattery(8), now) == BatteryEvent::None);  // jitter resets
        assert(Feed(p, &now, OnBattery(7), 3) == BatteryEvent::Critical);
        assert(p.State() == BatteryState::Critical && p.LowPower());
        assert(p.Reaction(now + BP::kYawnEveryMs * 2, true) == BatteryReaction::None);
        const uint32_t critical = now;
        // Still critical: nothing until the retry interval passed.
        while (now + kStep < critical + BP::kCriticalRetryMs) {
            now += kStep;
            assert(p.Update(OnBattery(6), now) == BatteryEvent::None);
        }
        now = critical + BP::kCriticalRetryMs;
        assert(p.Update(OnBattery(6), now) == BatteryEvent::Critical);
        // 10 % after a rest: still critical (hysteresis); 11 %: low again
        // without a new "low" event.
        assert(Feed(p, &now, OnBattery(10), 3) == BatteryEvent::None);
        assert(p.State() == BatteryState::Critical);
        assert(Feed(p, &now, OnBattery(11), 3) == BatteryEvent::None);
        assert(p.State() == BatteryState::Low);
        // Plugged in at 6 %: charging, never critical while on USB.
        BatteryPolicy q;
        uint32_t t = 0;
        Feed(q, &t, OnBattery(6), 2);
        assert(Feed(q, &t, OnBattery(6), 3) == BatteryEvent::Critical);
        assert(Feed(q, &t, Charging(6), 3) == BatteryEvent::Plugged);
        assert(q.State() == BatteryState::Charging && !q.LowPower());
        for (int i = 0; i < 100; ++i) assert(q.Update(Charging(5), t += kStep) == BatteryEvent::None);
        // Unplugged again still at 6 %: critical right after "unplugged".
        assert(Feed(q, &t, OnBattery(6), 3) == BatteryEvent::Unplugged);
        assert(q.Update(OnBattery(6), t += kStep) == BatteryEvent::Critical);
    }

    // A straight drop from normal to critical skips "low"; unplugging
    // while low reminds again (low event after "unplugged").
    {
        BatteryPolicy p;
        uint32_t now = 0;
        Feed(p, &now, OnBattery(40), 2);
        assert(Feed(p, &now, OnBattery(5), 3) == BatteryEvent::Critical);

        BatteryPolicy q;
        uint32_t t = 0;
        assert(Feed(q, &t, OnBattery(15), 5) == BatteryEvent::EnteredLow);
        assert(q.Reaction(t, true) == BatteryReaction::Yawn);
        assert(Feed(q, &t, Charging(15), 3) == BatteryEvent::Plugged);
        assert(q.Reaction(t, true) == BatteryReaction::Eat);
        assert(Feed(q, &t, OnBattery(15), 3) == BatteryEvent::Unplugged);
        assert(q.State() == BatteryState::Low);
        assert(q.Update(OnBattery(15), t += kStep) == BatteryEvent::EnteredLow);
        assert(q.Reaction(t, true) == BatteryReaction::Yawn);
    }

    // Readings without a level keep the source; out-of-range levels are
    // ignored (the last valid percent stays).
    {
        BatteryPolicy p;
        uint32_t now = 0;
        Feed(p, &now, OnBattery(30), 2);
        BatteryReading odd = OnBattery(-1);
        assert(Feed(p, &now, odd, 5) == BatteryEvent::None);
        odd.pct = 140;
        assert(Feed(p, &now, odd, 5) == BatteryEvent::None);
        assert(p.Percent() == 30 && p.State() == BatteryState::Normal);
        // On battery but never a level: unknown, never low.
        BatteryPolicy q;
        uint32_t t = 0;
        assert(Feed(q, &t, OnBattery(-1), 5) == BatteryEvent::None);
        assert(q.State() == BatteryState::Unknown && !q.LowPower() && !q.External());
    }

    // The clock may wrap.
    {
        BatteryPolicy p;
        uint32_t now = 0xFFFFFFFFu - 5000;
        assert(Feed(p, &now, OnBattery(15), 5) == BatteryEvent::EnteredLow);
        assert(p.Reaction(now, true) == BatteryReaction::Yawn);
        assert(p.Reaction(now + BP::kYawnEveryMs - 1, true) == BatteryReaction::None);
        assert(p.Reaction(now + BP::kYawnEveryMs, true) == BatteryReaction::Yawn);
    }

    // Packed word for board tasks: round trip, never 0, unknown level.
    {
        for (int pct : {-1, 0, 7, 50, 100, 101}) {
            for (int flags = 0; flags < 4; ++flags) {
                BatteryReading in;
                in.pct = pct;
                in.charging = (flags & 1) != 0;
                in.on_battery = (flags & 2) != 0;
                const uint32_t bits = PackBatteryReading(in);
                assert(bits != 0 && bits < 0x10000u);
                const BatteryReading out = UnpackBatteryReading(bits);
                assert(out.pct == (pct >= 0 && pct <= 100 ? pct : -1));
                assert(out.charging == in.charging && out.on_battery == in.on_battery);
                // Bits 16-31 are free for a sequence number.
                const BatteryReading tagged = UnpackBatteryReading(bits | 0xABCD0000u);
                assert(tagged.pct == out.pct && tagged.charging == out.charging &&
                       tagged.on_battery == out.on_battery);
            }
        }
    }

    // Names used in the board logs (self-test expectations rely on them).
    assert(std::strcmp(BatteryEventName(BatteryEvent::Plugged), "plugged") == 0);
    assert(std::strcmp(BatteryEventName(BatteryEvent::Unplugged), "unplugged") == 0);
    assert(std::strcmp(BatteryEventName(BatteryEvent::EnteredLow), "low") == 0);
    assert(std::strcmp(BatteryEventName(BatteryEvent::Critical), "critical") == 0);
    assert(std::strcmp(BatteryEventName(BatteryEvent::Full), "full") == 0);
    assert(std::strcmp(BatteryStateName(BatteryState::Charging), "charging") == 0);
    assert(std::strcmp(BatteryStateName(BatteryState::Low), "low") == 0);
    assert(std::strcmp(BatteryReactionName(BatteryReaction::Eat), "eat") == 0);
    assert(std::strcmp(BatteryReactionName(BatteryReaction::Yawn), "yawn") == 0);

    std::cout << "PASS: battery policy (debounce, hysteresis, low/critical, charging reactions)\n";
    return 0;
}
