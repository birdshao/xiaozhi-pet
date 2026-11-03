#pragma once

#include "kid_app/input_hub.h"
#include "kid_app/pet_controller.h"

#include <cstdint>

namespace kid_app {

// Physical click-count mapping kept separate from ESP32 drivers, NVS and
// content/voice services. LongPress still belongs to board mode switching.
enum class PetClick : uint8_t { Single, Double, Triple };

struct PetInputDecision {
    bool valid = false;
    PetAction action = PetAction::Pickup;
};

// Only returns an action. The board must enqueue it to the existing Pet task;
// it MUST NOT directly update a PetController or claim a durable effect.
class PetInputRouter {
public:
    static PetInputDecision Decide(PetMode mode, PetClick click);
    // Three-way dial semantics from InputHub. Talk/BootLong/SleepRequest/
    // ParentCombo are owned by the board and never become Pet actions here.
    static PetInputDecision DecideDial(PetMode mode, InputKind kind);
};

}  // namespace kid_app
