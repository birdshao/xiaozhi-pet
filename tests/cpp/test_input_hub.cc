#include "kid_app/input_hub.h"

#include <cassert>
#include <iostream>
#include <vector>

using namespace kid_app;

// Releases are confirmed only after the bounce window (default 120 ms).
constexpr uint32_t kSettle = 120;

static std::vector<InputEvent> Drain(InputHub& hub) {
    std::vector<InputEvent> out;
    InputEvent event{};
    while (hub.Next(&event)) out.push_back(event);
    return out;
}

static void ExpectOnly(InputHub& hub, InputKind kind, bool repeat = false) {
    const auto events = Drain(hub);
    assert(events.size() == 1);
    assert(events[0].kind == kind);
    assert(events[0].repeat == repeat);
}

static void ExpectNone(InputHub& hub) { assert(Drain(hub).empty()); }

int main() {
    // Dial up/down respond on press-down so children hear feedback at once.
    {
        InputHub hub;
        hub.Press(InputKey::DialUp, 100);
        ExpectOnly(hub, InputKind::DialUp);
        hub.Tick(2000);  // no auto-repeat unless a list/page context asks
        ExpectNone(hub);
        hub.Release(InputKey::DialUp, 2100);
        hub.Tick(2100 + kSettle);
        ExpectNone(hub);
        hub.Press(InputKey::DialDown, 3000);
        ExpectOnly(hub, InputKind::DialDown);
        hub.Release(InputKey::DialDown, 3050);
        hub.Tick(3050 + kSettle);
        ExpectNone(hub);
    }

    // Auto-repeat is bounded: 500 ms delay, then one event per 350 ms.
    {
        InputHub hub;
        hub.SetAutoRepeat(true);
        hub.Press(InputKey::DialDown, 0);
        ExpectOnly(hub, InputKind::DialDown);
        hub.Tick(499);
        ExpectNone(hub);
        hub.Tick(500);
        ExpectOnly(hub, InputKind::DialDown, true);
        hub.Tick(849);
        ExpectNone(hub);
        hub.Tick(850);
        ExpectOnly(hub, InputKind::DialDown, true);
        // A late tick never bursts several repeats at once.
        hub.Tick(5000);
        ExpectOnly(hub, InputKind::DialDown, true);
        hub.Release(InputKey::DialDown, 5010);
        hub.Tick(6000);
        ExpectNone(hub);
    }

    // Dial press: short = confirm after release settles, long = home once.
    {
        InputHub hub;
        hub.Press(InputKey::DialPress, 0);
        ExpectNone(hub);
        hub.Release(InputKey::DialPress, 300);
        hub.Tick(300 + kSettle - 1);
        ExpectNone(hub);  // still inside the bounce window
        hub.Tick(300 + kSettle);
        ExpectOnly(hub, InputKind::DialPress);

        hub.Press(InputKey::DialPress, 1000);
        hub.Tick(1999);
        ExpectNone(hub);
        hub.Tick(2000);
        ExpectOnly(hub, InputKind::DialLong);
        hub.Tick(4000);
        ExpectNone(hub);
        hub.Release(InputKey::DialPress, 4100);
        hub.Tick(4100 + kSettle);
        ExpectNone(hub);  // consumed by the long press
    }

    // V1 hardware trial: holding the dial centre chattered (release and
    // re-press within ~100 ms). Chatter is one continuous hold.
    {
        InputHub hub;
        hub.Press(InputKey::DialPress, 19800);
        hub.Release(InputKey::DialPress, 19950);
        hub.Press(InputKey::DialPress, 19990);  // 40 ms gap: bounce
        hub.Release(InputKey::DialPress, 20060);
        hub.Press(InputKey::DialPress, 20100);
        hub.Tick(20300);
        ExpectNone(hub);  // no stray short presses
        hub.Tick(20800);
        ExpectOnly(hub, InputKind::DialLong);  // timed from the first press
        hub.Release(InputKey::DialPress, 21500);
        hub.Press(InputKey::DialPress, 21560);  // release chatter
        hub.Release(InputKey::DialPress, 21600);
        hub.Tick(21600 + kSettle);
        ExpectNone(hub);
    }

    // Up/down chatter never produces duplicate steps.
    {
        InputHub hub;
        hub.Press(InputKey::DialDown, 0);
        hub.Release(InputKey::DialDown, 60);
        hub.Press(InputKey::DialDown, 90);
        hub.Release(InputKey::DialDown, 150);
        hub.Tick(150 + kSettle);
        ExpectOnly(hub, InputKind::DialDown);
        hub.Press(InputKey::DialDown, 600);  // a deliberate second step
        ExpectOnly(hub, InputKind::DialDown);
    }

    // BOOT keeps voice semantics: short = talk, long = BootLong.
    {
        InputHub hub;
        hub.Press(InputKey::Boot, 0);
        hub.Release(InputKey::Boot, 200);
        hub.Tick(200 + kSettle);
        ExpectOnly(hub, InputKind::Talk);
        hub.Press(InputKey::Boot, 1000);
        hub.Tick(2000);
        ExpectOnly(hub, InputKind::BootLong);
        hub.Release(InputKey::Boot, 2500);
        hub.Tick(2500 + kSettle);
        ExpectNone(hub);
    }

    // PWR never powers off directly; it requests the cancellable sleep flow.
    {
        InputHub hub;
        hub.Press(InputKey::Power, 0);
        ExpectNone(hub);
        hub.Release(InputKey::Power, 150);
        hub.Tick(150 + kSettle);
        ExpectOnly(hub, InputKind::SleepRequest);
    }

    // Parent combo: BOOT + dial press held 5 s from the later press.
    {
        InputHub hub;
        hub.Press(InputKey::Boot, 0);
        hub.Press(InputKey::DialPress, 400);
        hub.Tick(1500);  // neither BootLong nor DialLong while combining
        ExpectNone(hub);
        hub.Tick(5399);
        ExpectNone(hub);
        hub.Tick(5400);
        ExpectOnly(hub, InputKind::ParentCombo);
        hub.Tick(9000);
        ExpectNone(hub);
        hub.Release(InputKey::DialPress, 9100);
        hub.Release(InputKey::Boot, 9200);
        hub.Tick(9200 + kSettle);
        ExpectNone(hub);  // no stray Talk / DialPress afterwards
    }

    // Other keys during a held combo are swallowed and neither restart the
    // 5 s timer nor re-arm it after ParentCombo fired.
    {
        InputHub hub;
        hub.Press(InputKey::Boot, 0);
        hub.Press(InputKey::DialPress, 100);
        hub.Press(InputKey::DialDown, 3000);
        hub.Release(InputKey::DialDown, 3100);
        ExpectNone(hub);
        hub.Tick(5100);
        ExpectOnly(hub, InputKind::ParentCombo);
        hub.Press(InputKey::DialUp, 6000);
        hub.Release(InputKey::DialUp, 6100);
        hub.Tick(12000);
        ExpectNone(hub);
        hub.Release(InputKey::Boot, 12100);
        hub.Release(InputKey::DialPress, 12200);
        hub.Tick(12200 + kSettle);
        ExpectNone(hub);
    }

    // Combo key chatter does not break or re-arm a combo.
    {
        InputHub hub;
        hub.Press(InputKey::Boot, 0);
        hub.Press(InputKey::DialPress, 100);
        hub.Release(InputKey::DialPress, 2000);
        hub.Press(InputKey::DialPress, 2040);  // bounce
        hub.Tick(5099);
        ExpectNone(hub);
        hub.Tick(5100);
        ExpectOnly(hub, InputKind::ParentCombo);
    }

    // A starved task that sees press+release before any Tick still honours
    // the real hold duration.
    {
        InputHub hub;
        hub.Press(InputKey::DialPress, 0);
        hub.Release(InputKey::DialPress, 1500);
        hub.Tick(1500 + kSettle);
        ExpectOnly(hub, InputKind::DialLong);
        hub.Press(InputKey::Boot, 2000);
        hub.Release(InputKey::Boot, 3200);
        hub.Tick(3200 + kSettle);
        ExpectOnly(hub, InputKind::BootLong);
    }

    // An aborted combo produces nothing (no accidental talk or confirm).
    {
        InputHub hub;
        hub.Press(InputKey::DialPress, 0);
        hub.Press(InputKey::Boot, 100);
        hub.Release(InputKey::Boot, 600);
        hub.Release(InputKey::DialPress, 700);
        hub.Tick(8000);
        ExpectNone(hub);
    }

    // While voice is active only home, talk, sleep and parent pass.
    {
        InputHub hub;
        hub.SetVoiceActive(true);
        hub.Press(InputKey::DialUp, 0);
        hub.Release(InputKey::DialUp, 50);
        hub.Press(InputKey::DialPress, 500);
        hub.Release(InputKey::DialPress, 600);
        hub.Tick(600 + kSettle);
        ExpectNone(hub);
        hub.Press(InputKey::DialPress, 1000);
        hub.Tick(2000);
        ExpectOnly(hub, InputKind::DialLong);
        hub.Release(InputKey::DialPress, 2100);
        hub.Press(InputKey::Boot, 3000);
        hub.Release(InputKey::Boot, 3100);
        hub.Tick(3100 + kSettle);
        ExpectOnly(hub, InputKind::Talk);
        hub.Press(InputKey::Power, 4000);
        hub.Release(InputKey::Power, 4100);
        hub.Tick(4100 + kSettle);
        ExpectOnly(hub, InputKind::SleepRequest);
    }

    // Duplicate edges are ignored; event timestamps survive the queue.
    {
        InputHub hub;
        hub.Release(InputKey::DialPress, 10);
        hub.Press(InputKey::DialUp, 20);
        hub.Press(InputKey::DialUp, 30);
        hub.Tick(1000);
        const auto events = Drain(hub);
        assert(events.size() == 1 && events[0].at_ms == 20);
    }

    // uint32 millisecond wrap-around keeps long-press timing correct.
    {
        InputHub hub;
        hub.Press(InputKey::DialPress, 0xFFFFFF00u);
        hub.Tick(0x000002E7u);  // 999 ms later
        ExpectNone(hub);
        hub.Tick(0x000002E8u);  // 1000 ms later
        ExpectOnly(hub, InputKind::DialLong);
    }

    // Bounded queue: overflow drops new events and is counted.
    {
        InputHub hub;
        for (uint32_t i = 0; i < InputHub::kCapacity + 3; ++i) {
            hub.Press(InputKey::DialUp, i * 300);
            hub.Release(InputKey::DialUp, i * 300 + 50);
        }
        assert(hub.Pending() == InputHub::kCapacity);
        assert(hub.Dropped() == 3);
        assert(Drain(hub).size() == InputHub::kCapacity);
    }

    std::cout << "PASS: InputHub dial/BOOT/PWR semantics, chatter, repeat, long, combo, voice gate\n";
}
