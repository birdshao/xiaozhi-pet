#pragma once

#include "kid_app/pet_event_executor.h"
#include "kid_app/pet_event_queue.h"

namespace kid_app {

enum class PumpResult : uint8_t {
    Idle, Committed, Duplicate, Rejected, StorageBlocked
};

// Pure single-thread adapter. Keeps a pending event when persistence fails;
// new queued events cannot bypass the failed durable event.
template <size_t Capacity>
class PetEventRuntime {
public:
    explicit PetEventRuntime(PetEventExecutor& executor) : executor_(executor) {}

    bool Enqueue(const PetEvent& event) { return queue_.Push(event); }
    size_t QueuedCount() const { return queue_.Size(); }
    bool HasPendingRetry() const { return pending_; }

    PumpResult PumpOne(PetEffects* effects, PetEvent* finished = nullptr) {
        if (!effects) return PumpResult::Rejected;
        *effects = PetEffects{};
        if (!pending_) {
            if (!queue_.Pop(&retry_)) return PumpResult::Idle;
            pending_ = true;
        }

        const auto result = executor_.ApplyDurably(retry_, effects);
        if (result == CommitResult::StorageError) {
            // Do NOT discard, ACK, or play sound for an event not persisted.
            return PumpResult::StorageBlocked;
        }
        if (finished) *finished = retry_;
        pending_ = false;
        switch (result) {
            case CommitResult::Committed: return PumpResult::Committed;
            case CommitResult::Duplicate: return PumpResult::Duplicate;
            case CommitResult::Rejected: return PumpResult::Rejected;
            case CommitResult::StorageError: break;
        }
        return PumpResult::StorageBlocked;
    }

private:
    PetEventExecutor& executor_;
    PetEventQueue<Capacity> queue_;
    PetEvent retry_{};
    bool pending_ = false;
};

}  // namespace kid_app
