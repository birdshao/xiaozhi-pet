#pragma once

#include "kid_app/pet_controller.h"
#include "kid_app/pet_persistence.h"

namespace kid_app {

enum class CommitResult : uint8_t {
    Committed, Duplicate, Rejected, StorageError
};

// Central single-thread owner for durable child interactions.
// Call in the input/event task, never an ISR/EPD flush callback.
// Publish audio/UI effects only after the event and replay ID have committed.
class PetEventExecutor {
public:
    PetEventExecutor(PetController& pet, PetPersistence& store)
        : pet_(pet), store_(store) {}
    CommitResult ApplyDurably(const PetEvent& event, PetEffects* effects);

private:
    PetController& pet_;
    PetPersistence& store_;
};

}  // namespace kid_app
