#include "kid_app/pet_input_router.h"

namespace kid_app {

PetInputDecision PetInputRouter::Decide(PetMode mode, PetClick click) {
    if (mode == PetMode::Story) {
        switch (click) {
            case PetClick::Single: return {true, PetAction::NextPage};
            case PetClick::Double: return {true, PetAction::StopStory};
            case PetClick::Triple: return {};  // Do not feed while reading.
        }
    }
    if (mode == PetMode::Idle || mode == PetMode::Interact) {
        switch (click) {
            case PetClick::Single: return {true, PetAction::Pickup};
            case PetClick::Double: return {true, PetAction::StartStory};
            case PetClick::Triple: return {true, PetAction::Feed};
        }
    }
    // Quiet / listening / speaking do not consume new Pet actions.
    return {};
}

PetInputDecision PetInputRouter::DecideDial(PetMode mode, InputKind kind) {
    if (mode == PetMode::Story) {
        if (kind == InputKind::DialDown) return {true, PetAction::NextPage};
        if (kind == InputKind::DialLong) return {true, PetAction::StopStory};
        return {};  // no previous-page action in the v1 PetController yet
    }
    // At home the dial drives the action rail (DecideHome).
    return {};
}

bool PetInputRouter::IsHomeRail(PetMode mode) {
    return mode == PetMode::Idle || mode == PetMode::Interact;
}

PetInputDecision PetInputRouter::DecideHome(PetMode mode, HomeItem item) {
    if (!IsHomeRail(mode)) return {};
    switch (item) {
        case HomeItem::Story: return {true, PetAction::StartStory};
        case HomeItem::Feed: return {true, PetAction::Feed};
        // Chat (V6) and games (V4) are not built yet: the pet says hi.
        case HomeItem::Chat:
        case HomeItem::Game: return {true, PetAction::Pickup};
        case HomeItem::Sleep: return {};  // board-owned sleep flow (V2-09)
    }
    return {};
}

}  // namespace kid_app
