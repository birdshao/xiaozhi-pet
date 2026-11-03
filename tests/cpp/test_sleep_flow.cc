#include "kid_app/sleep_flow.h"

#include <cassert>
#include <iostream>

using namespace kid_app;

int main() {
    // Request -> sleep scene; countdown starts only once it is on screen;
    // 3 s later the board powers off.
    {
        SleepFlow flow;
        assert(!flow.Active() && flow.Phase() == SleepPhase::Awake);
        assert(flow.Request(1000) == SleepAction::ShowSleepScene);
        assert(flow.Active() && flow.Phase() == SleepPhase::Showing);
        assert(flow.Request(1100) == SleepAction::None);  // already going to sleep
        assert(flow.Tick(9000) == SleepAction::None);     // ePaper still refreshing
        flow.OnSceneShown(5300);
        assert(flow.Phase() == SleepPhase::Countdown);
        assert(flow.Tick(8299) == SleepAction::None);
        assert(flow.Tick(8300) == SleepAction::PowerOff);
        assert(flow.Phase() == SleepPhase::PoweringOff);
        assert(flow.Tick(20000) == SleepAction::None);  // only once
        // Inputs no longer cancel once power-off has started.
        assert(flow.OnInput(InputKind::DialPress, 8400) == SleepAction::None);
        // If the PMIC did not cut power (e.g. USB), go back to awake.
        flow.PowerOffReturned();
        assert(flow.Phase() == SleepPhase::Awake && !flow.Active());
    }

    // Any dial or BOOT action cancels, before or after the scene is shown.
    {
        for (InputKind kind : {InputKind::DialUp, InputKind::DialDown, InputKind::DialPress,
                               InputKind::DialLong, InputKind::Talk, InputKind::BootLong}) {
            SleepFlow flow;
            flow.Request(0);
            assert(flow.OnInput(kind, 100) == SleepAction::RestoreScene);
            assert(!flow.Active());
            flow.Request(1000);
            flow.OnSceneShown(5000);
            assert(flow.OnInput(kind, 7999) == SleepAction::RestoreScene);
            assert(!flow.Active());
            assert(flow.Tick(9000) == SleepAction::None);
        }
    }

    // A second PWR press while going to sleep does not cancel it; a parent
    // combo does not cancel either (no surprise wake-up for the child).
    {
        SleepFlow flow;
        flow.Request(0);
        flow.OnSceneShown(4000);
        assert(flow.OnInput(InputKind::SleepRequest, 5000) == SleepAction::None);
        assert(flow.OnInput(InputKind::ParentCombo, 5500) == SleepAction::None);
        assert(flow.Tick(7000) == SleepAction::PowerOff);
    }

    // Input while awake is not the flow's business.
    {
        SleepFlow flow;
        assert(flow.OnInput(InputKind::DialPress, 0) == SleepAction::None);
        flow.OnSceneShown(10);  // stale notification is ignored
        assert(flow.Phase() == SleepPhase::Awake);
    }

    // The sleep scene never appeared (EPD failure): give up and restore.
    {
        SleepFlow flow;
        flow.Request(1000);
        assert(flow.Tick(1000 + SleepFlow::kShowTimeoutMs - 1) == SleepAction::None);
        assert(flow.Tick(1000 + SleepFlow::kShowTimeoutMs) == SleepAction::RestoreScene);
        assert(!flow.Active());
    }

    // Pet mode turned off mid-flow (BOOT long / toggle): abort, never power
    // off from ordinary XiaoZhi mode.
    {
        SleepFlow flow;
        assert(!flow.Cancel());  // nothing to cancel
        flow.Request(0);
        flow.OnSceneShown(4000);
        assert(flow.Cancel());
        assert(!flow.Active());
        assert(flow.Tick(9000) == SleepAction::None);
        flow.Request(10000);
        assert(flow.Cancel() && !flow.Active());
        flow.Request(20000);
        flow.OnSceneShown(24000);
        assert(flow.Tick(27000) == SleepAction::PowerOff);
        assert(!flow.Cancel());  // power-off already started
        assert(flow.Phase() == SleepPhase::PoweringOff);
    }

    // uint32 millisecond wrap-around.
    {
        SleepFlow flow;
        flow.Request(0xFFFFF000u);
        flow.OnSceneShown(0xFFFFFF00u);
        assert(flow.Tick(0x00000AB7u) == SleepAction::None);  // 2999 ms later
        assert(flow.Tick(0x00000AB8u) == SleepAction::PowerOff);
    }

    std::cout << "PASS: sleep flow shows, counts down after display, cancels, powers off once\n";
}
