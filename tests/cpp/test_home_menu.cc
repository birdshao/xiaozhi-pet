#include "kid_app/home_menu.h"
#include "kid_app/pet_input_router.h"

#include <cassert>
#include <iostream>

using namespace kid_app;

int main() {
    // Five big icons; the reader is preselected (most common request).
    {
        HomeMenu menu;
        assert(HomeMenu::kItemCount == 5);
        assert(menu.Selected() == HomeItem::Story && menu.Index() == 1);
        assert(menu.Move(-1) && menu.Selected() == HomeItem::Chat);
        // Clamped at the ends: a child pushing further does not jump around.
        assert(!menu.Move(-1) && menu.Selected() == HomeItem::Chat);
        assert(menu.Move(+1) && menu.Move(+1) && menu.Move(+1) && menu.Move(+1));
        assert(menu.Selected() == HomeItem::Sleep && menu.Index() == 4);
        assert(!menu.Move(+1) && menu.Selected() == HomeItem::Sleep);
        menu.Reset();
        assert(menu.Selected() == HomeItem::Story);
        assert(menu.Select(3) && menu.Selected() == HomeItem::Feed);
        assert(!menu.Select(5) && menu.Selected() == HomeItem::Feed);
    }

    // Confirm on the home rail: Story/Feed are real Pet actions; Chat says
    // hi until V6; Game says hi only when the board cannot start a V4-06
    // mini game; Sleep belongs to the board's sleep flow.
    {
        const auto story = PetInputRouter::DecideHome(PetMode::Idle, HomeItem::Story);
        assert(story.valid && story.action == PetAction::StartStory);
        const auto feed = PetInputRouter::DecideHome(PetMode::Interact, HomeItem::Feed);
        assert(feed.valid && feed.action == PetAction::Feed);
        for (HomeItem item : {HomeItem::Chat, HomeItem::Game}) {
            const auto hi = PetInputRouter::DecideHome(PetMode::Idle, item);
            assert(hi.valid && hi.action == PetAction::Pickup);
        }
        assert(!PetInputRouter::DecideHome(PetMode::Idle, HomeItem::Sleep).valid);
        for (PetMode mode : {PetMode::Story, PetMode::Quiet, PetMode::Listening,
                             PetMode::Speaking}) {
            assert(!PetInputRouter::DecideHome(mode, HomeItem::Story).valid);
        }
    }

    // On the home rail the dial moves the highlight, it no longer says hi;
    // in the reader dial semantics are unchanged.
    {
        assert(!PetInputRouter::DecideDial(PetMode::Idle, InputKind::DialPress).valid);
        assert(!PetInputRouter::DecideDial(PetMode::Idle, InputKind::DialUp).valid);
        assert(PetInputRouter::DecideDial(PetMode::Story, InputKind::DialDown).action ==
               PetAction::NextPage);
        assert(PetInputRouter::DecideDial(PetMode::Story, InputKind::DialLong).action ==
               PetAction::StopStory);
        assert(PetInputRouter::IsHomeRail(PetMode::Idle));
        assert(PetInputRouter::IsHomeRail(PetMode::Interact));
        assert(!PetInputRouter::IsHomeRail(PetMode::Story));
        assert(!PetInputRouter::IsHomeRail(PetMode::Quiet));
    }

    std::cout << "PASS: home rail selection clamps, confirm maps to Pet actions\n";
}
