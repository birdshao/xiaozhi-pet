#include "kid_app/selftest_clock.h"

#include "kid_app/battery_policy.h"
#include "kid_app/parent_menu.h"
#include "kid_app/pet_animation.h"
#include "kid_app/power_manager.h"
#include "kid_app/selftest_script.h"

#include <cassert>
#include <iostream>

using namespace kid_app;

namespace {

// First tick at or after `from` (100 ms steps) that leaves Active.
uint32_t IdleAt(uint32_t from) {
    PowerManager pm;
    pm.Reset(from);
    PowerConditions c;
    for (uint32_t t = from; t < from + 2 * PowerManager::kIdleAfterMs; t += 100)
        if (pm.Tick(t, c) == PowerEvent::EnterIdle) return t - from;
    return 0;
}

uint32_t MorningSleepAt(uint32_t from) {
    PowerManager pm;
    pm.StartMorning(from);
    PowerConditions c;
    for (uint32_t t = from; t < from + 2 * PowerManager::kMorningAwakeMs; t += 100)
        if (pm.Tick(t, c) == PowerEvent::Sleep) return t - from;
    return 0;
}

uint32_t ParentExitAt(uint32_t from) {
    ParentMenu menu;
    menu.Open(ParentSettings{}, from);
    for (uint32_t t = from; t < from + 2 * ParentMenu::kIdleExitMs; t += 100)
        if (menu.Tick(t) == ParentMenuAction::Exit) return t - from;
    return 0;
}

// Delay from the scene being shown to the first blink (no cleanup due).
uint32_t BlinkAt(uint32_t from) {
    SceneModel home{};
    home.kind = SceneKind::Home;
    PetAnimationDirector d;
    d.SceneShown(home, from);
    for (uint32_t t = from; t < from + 2 * (PetAnimationDirector::kBlinkMinMs +
                                            PetAnimationDirector::kBlinkSpanMs);
         t += 100)
        if (d.Idle(home, t, true, false).kind == DirectorStep::Kind::Frame) return t - from;
    return 0;
}

uint32_t CleanupAt(uint32_t from) {
    SceneModel home{};
    home.kind = SceneKind::Home;
    PetAnimationDirector d;
    d.SceneShown(home, from);
    for (uint32_t t = from; t < from + 2 * PetAnimationDirector::kCleanupIdleMs; t += 100)
        if (d.Idle(home, t, true, true).kind == DirectorStep::Kind::Cleanup) return t - from;
    return 0;
}

// Plugged in at `from`: eats right away, then once per (scaled) period.
uint32_t SecondEatAt(uint32_t from) {
    BatteryReading battery{50, false, true}, usb{50, true, false};
    BatteryPolicy p;
    p.Update(battery, from);
    p.Update(usb, from);
    assert(p.Update(usb, from) == BatteryEvent::Plugged);
    assert(p.Reaction(from, true) == BatteryReaction::Eat);
    for (uint32_t t = from; t < from + 2 * BatteryPolicy::kEatEveryMs; t += 1000) {
        p.Update(usb, t);
        if (p.Reaction(t, true) == BatteryReaction::Eat) return t - from;
    }
    return 0;
}

}  // namespace

int main() {
    // Real time by default, and the parser's lower bound is the clock's.
    static_assert(SelfTestScript::kMinTimeScalePct == SelfTestClock::kMinPct, "bounds");
    assert(SelfTestClock::Pct() == 100 && !SelfTestClock::Scaling());
    assert(SelfTestClock::Scaled(0) == 0);
    assert(SelfTestClock::Scaled(45000) == 45000);
    assert(SelfTestClock::Scaled(0xFFFFFFFFu) == 0xFFFFFFFFu);

    // Real-time reference values (what a child's device always runs).
    const uint32_t idle = IdleAt(1000), morning = MorningSleepAt(1000);
    const uint32_t parent = ParentExitAt(1000), blink = BlinkAt(1000);
    const uint32_t cleanup = CleanupAt(1000), eat = SecondEatAt(1000);
    assert(idle == PowerManager::kIdleAfterMs && morning == PowerManager::kMorningAwakeMs);
    assert(parent == ParentMenu::kIdleExitMs && cleanup == PetAnimationDirector::kCleanupIdleMs);
    assert(blink >= PetAnimationDirector::kBlinkMinMs &&
           blink < PetAnimationDirector::kBlinkMinMs + PetAnimationDirector::kBlinkSpanMs + 100);
    assert(eat >= BatteryPolicy::kEatEveryMs && eat < BatteryPolicy::kEatEveryMs + 1000);

    // Out of range: refused, nothing changes.
    assert(!SelfTestClock::SetForSelfTest(0));
    assert(!SelfTestClock::SetForSelfTest(9));
    assert(!SelfTestClock::SetForSelfTest(101));
    assert(!SelfTestClock::SetForSelfTest(0x10000 + 25));  // no truncation to 25
    assert(SelfTestClock::Pct() == 100);

    // 25 %: every wrapped feature timer is a quarter as long.
    assert(SelfTestClock::SetForSelfTest(25) && SelfTestClock::Scaling());
    assert(SelfTestClock::Scaled(60000) == 15000);
    assert(SelfTestClock::Scaled(3) == 1);  // never 0 for a real duration
    assert(SelfTestClock::Scaled(0xFFFFFFFFu) == 0x3FFFFFFFu);  // no overflow
    assert(IdleAt(5000) == PowerManager::kIdleAfterMs / 4);
    assert(MorningSleepAt(5000) == PowerManager::kMorningAwakeMs / 4);
    assert(ParentExitAt(5000) == ParentMenu::kIdleExitMs / 4);
    assert(CleanupAt(5000) == PetAnimationDirector::kCleanupIdleMs / 4);
    const uint32_t fast_blink = BlinkAt(5000);
    assert(fast_blink >= PetAnimationDirector::kBlinkMinMs / 4 &&
           fast_blink < (PetAnimationDirector::kBlinkMinMs +
                         PetAnimationDirector::kBlinkSpanMs) / 4 + 200);
    const uint32_t fast_eat = SecondEatAt(5000);
    assert(fast_eat >= BatteryPolicy::kEatEveryMs / 4 &&
           fast_eat < BatteryPolicy::kEatEveryMs / 4 + 1000);

    // 10 %, the floor.
    assert(SelfTestClock::SetForSelfTest(SelfTestClock::kMinPct));
    assert(ParentExitAt(0) == ParentMenu::kIdleExitMs / 10);

    // Back to 100 % (what MaybeRunSelfTest does when a script ends): the
    // exact real values again.
    assert(SelfTestClock::SetForSelfTest(SelfTestClock::kRealPct) && !SelfTestClock::Scaling());
    assert(IdleAt(7000) == idle && MorningSleepAt(7000) == morning);
    assert(ParentExitAt(7000) == parent && CleanupAt(7000) == cleanup);
    assert(SecondEatAt(7000) == eat);

    std::cout << "PASS: self-test time scale (real by default, bounds, scaled feature timers)\n";
}
