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
    if (mode == PetMode::Idle || mode == PetMode::Interact) {
        // Home action rail (V2-03) will give DialUp/DialDown meaning.
        if (kind == InputKind::DialPress) return {true, PetAction::Pickup};
    }
    return {};
}

}  // namespace kid_app
