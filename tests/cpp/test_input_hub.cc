#include "kid_app/input_hub.h"

#include <cassert>
#include <iostream>
#include <vector>

using namespace kid_app;

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
        ExpectNone(hub);
        hub.Press(InputKey::DialDown, 3000);
        ExpectOnly(hub, InputKind::DialDown);
        hub.Release(InputKey::DialDown, 3050);
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

    // Dial press: short = confirm on release, long = home exactly once.
    {
        InputHub hub;
        hub.Press(InputKey::DialPress, 0);
        ExpectNone(hub);
        hub.Release(InputKey::DialPress, 300);
        ExpectOnly(hub, InputKind::DialPress);

        hub.Press(InputKey::DialPress, 1000);
        hub.Tick(1999);
        ExpectNone(hub);
        hub.Tick(2000);
        ExpectOnly(hub, InputKind::DialLong);
        hub.Tick(4000);
        ExpectNone(hub);
        hub.Release(InputKey::DialPress, 4100);
        ExpectNone(hub);  // consumed by the long press
    }

    // BOOT keeps voice semantics: short = talk, long = BootLong.
    {
        InputHub hub;
        hub.Press(InputKey::Boot, 0);
        hub.Release(InputKey::Boot, 200);
        ExpectOnly(hub, InputKind::Talk);
        hub.Press(InputKey::Boot, 1000);
        hub.Tick(2000);
        ExpectOnly(hub, InputKind::BootLong);
        hub.Release(InputKey::Boot, 2500);
        ExpectNone(hub);
    }

    // PWR never powers off directly; it requests the cancellable sleep flow.
    {
        InputHub hub;
        hub.Press(InputKey::Power, 0);
        ExpectNone(hub);
        hub.Release(InputKey::Power, 150);
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
        ExpectNone(hub);  // no stray Talk / DialPress afterwards
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
        hub.Press(InputKey::DialPress, 100);
        hub.Release(InputKey::DialPress, 200);
        ExpectNone(hub);
        hub.Press(InputKey::DialPress, 300);
        hub.Tick(1300);
        ExpectOnly(hub, InputKind::DialLong);
        hub.Release(InputKey::DialPress, 1400);
        hub.Press(InputKey::Boot, 2000);
        hub.Release(InputKey::Boot, 2100);
        ExpectOnly(hub, InputKind::Talk);
        hub.Press(InputKey::Power, 3000);
        hub.Release(InputKey::Power, 3100);
        ExpectOnly(hub, InputKind::SleepRequest);
    }

    // Duplicate edges are ignored; event timestamps survive the queue.
    {
        InputHub hub;
        hub.Release(InputKey::DialPress, 10);
        hub.Press(InputKey::DialUp, 20);
        hub.Press(InputKey::DialUp, 30);
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
            hub.Press(InputKey::DialUp, i * 10);
            hub.Release(InputKey::DialUp, i * 10 + 5);
        }
        assert(hub.Pending() == InputHub::kCapacity);
        assert(hub.Dropped() == 3);
        assert(Drain(hub).size() == InputHub::kCapacity);
    }

    std::cout << "PASS: InputHub dial/BOOT/PWR semantics, repeat, long, combo, voice gate\n";
}
