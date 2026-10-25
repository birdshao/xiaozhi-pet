#include "kid_app/pet_event_executor.h"

namespace kid_app {

CommitResult PetEventExecutor::ApplyDurably(const PetEvent& event,
                                            PetEffects* effects) {
    if (!effects) return CommitResult::Rejected;
    *effects = PetEffects{};
    const auto before = pet_.ExportCheckpoint();
    PetEffects pending{};
    switch (pet_.Apply(event, &pending)) {
        case PetResult::Rejected: return CommitResult::Rejected;
        case PetResult::Duplicate: return CommitResult::Duplicate;
        case PetResult::Applied: break;
    }

    if (!store_.Save(pet_)) {
        // Abort the in-memory effect and dedup entry. The caller must not ACK
        // or play sounds for this event; a retry can succeed later.
        if (!pet_.RestoreCheckpoint(before)) return CommitResult::StorageError;
        return CommitResult::StorageError;
    }
    *effects = pending;
    return CommitResult::Committed;
}

}  // namespace kid_app
