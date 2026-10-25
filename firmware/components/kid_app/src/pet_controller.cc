#include "kid_app/pet_controller.h"

#include <limits>

namespace kid_app {

bool PetController::WasSeen(uint64_t id) const {
    for (size_t i = 0; i < seen_count_; ++i) {
        if (seen_[i] == id) return true;
    }
    return false;
}

void PetController::Remember(uint64_t id) {
    seen_[next_slot_] = id;
    next_slot_ = (next_slot_ + 1) % kSeenCapacity;
    if (seen_count_ < kSeenCapacity) ++seen_count_;
}

PetResult PetController::Apply(const PetEvent& event, PetEffects* out) {
    if (!out) return PetResult::Rejected;
    *out = PetEffects{};
    if (event.id == 0) return PetResult::Rejected;
    if (WasSeen(event.id)) return PetResult::Duplicate;

    PetSnapshot proposed = state_;
    PetEffects effects{};
    switch (event.action) {
    case PetAction::Pickup:
        if (proposed.quiet) return PetResult::Rejected;
        proposed.mode = PetMode::Interact;
        proposed.mood = PetMood::Happy;
        effects.play_positive_sound = true;
        break;

    case PetAction::Feed:
        if (proposed.quiet || proposed.mode == PetMode::Listening ||
            proposed.mode == PetMode::Speaking)
            return PetResult::Rejected;
        if (proposed.interaction_count == std::numeric_limits<uint32_t>::max())
            return PetResult::Rejected;
        ++proposed.interaction_count;
        proposed.mode = PetMode::Interact;
        proposed.mood = PetMood::Happy;
        effects.play_positive_sound = true;
        break;

    case PetAction::StartStory:
        if (proposed.quiet) return PetResult::Rejected;
        proposed.mode = PetMode::Story;
        proposed.mood = PetMood::Curious;
        proposed.active_page = 1;
        break;

    case PetAction::NextPage:
        if (proposed.mode != PetMode::Story ||
            proposed.active_page == std::numeric_limits<uint16_t>::max())
            return PetResult::Rejected;
        ++proposed.active_page;
        break;

    case PetAction::StopStory:
        if (proposed.mode != PetMode::Story) return PetResult::Rejected;
        proposed.mode = PetMode::Idle;
        proposed.mood = PetMood::Neutral;
        break;

    case PetAction::VoiceBegin:
        if (proposed.quiet || proposed.mode == PetMode::Listening ||
            proposed.mode == PetMode::Speaking) return PetResult::Rejected;
        proposed.mode = PetMode::Listening;
        proposed.mood = PetMood::Thinking;
        effects.enter_voice = true;
        break;

    case PetAction::VoiceReplyBegin:
        if (proposed.quiet || proposed.mode != PetMode::Listening)
            return PetResult::Rejected;
        proposed.mode = PetMode::Speaking;
        proposed.mood = PetMood::Happy;
        break;

    case PetAction::VoiceEnd:
        if (proposed.mode != PetMode::Listening &&
            proposed.mode != PetMode::Speaking) return PetResult::Rejected;
        proposed.mode = proposed.quiet ? PetMode::Quiet : PetMode::Idle;
        proposed.mood = PetMood::Neutral;
        effects.stop_voice = true;
        break;

    case PetAction::QuietBegin:
        proposed.quiet = true;
        if (proposed.mode == PetMode::Listening ||
            proposed.mode == PetMode::Speaking)
            effects.stop_voice = true;
        proposed.mode = PetMode::Quiet;
        proposed.mood = PetMood::Sleepy;
        break;

    case PetAction::QuietEnd:
        if (!proposed.quiet) return PetResult::Rejected;
        proposed.quiet = false;
        proposed.mode = PetMode::Idle;
        proposed.mood = PetMood::Neutral;
        break;

    default:
        return PetResult::Rejected;
    }

    // One serial transition point, intended to be owned by a single event task.
    effects.repaint = proposed.mode != state_.mode ||
                      proposed.mood != state_.mood ||
                      proposed.active_page != state_.active_page;
    effects.persist = true;
    state_ = proposed;
    Remember(event.id);
    *out = effects;
    return PetResult::Applied;
}

void PetController::ResetForTest() {
    seen_.fill(0);
    seen_count_ = 0;
    next_slot_ = 0;
    state_ = PetSnapshot{};
}


PetCheckpoint PetController::ExportCheckpoint() const {
    PetCheckpoint result{};
    result.snapshot = state_;
    result.seen = seen_;
    result.seen_count = static_cast<uint16_t>(seen_count_);
    result.next_slot = static_cast<uint16_t>(next_slot_);
    return result;
}

bool PetController::RestoreCheckpoint(const PetCheckpoint& checkpoint) {
    const auto& s = checkpoint.snapshot;
    if (s.schema_version != 1 ||
        static_cast<uint8_t>(s.mode) > static_cast<uint8_t>(PetMode::Quiet) ||
        static_cast<uint8_t>(s.mood) > static_cast<uint8_t>(PetMood::Sleepy) ||
        s.active_page == 0 || checkpoint.seen_count > kSeenCapacity ||
        checkpoint.next_slot >= kSeenCapacity ||
        (checkpoint.seen_count < kSeenCapacity &&
         checkpoint.next_slot != checkpoint.seen_count) ||
        (s.quiet && s.mode != PetMode::Quiet) ||
        (!s.quiet && s.mode == PetMode::Quiet)) {
        return false;
    }

    // Reject invalid IDs and duplicate IDs inside the active replay window.
    for (size_t i = 0; i < checkpoint.seen_count; ++i) {
        if (checkpoint.seen[i] == 0) return false;
        for (size_t j = i + 1; j < checkpoint.seen_count; ++j) {
            if (checkpoint.seen[i] == checkpoint.seen[j]) return false;
        }
    }
    if (checkpoint.seen_count < kSeenCapacity) {
        for (size_t i = checkpoint.seen_count; i < kSeenCapacity; ++i) {
            if (checkpoint.seen[i] != 0) return false;
        }
    }

    // Exact in-memory restore is also used to abort a failed event commit.
    // Reboot-only normalization belongs to PetPersistence::Load().
    state_ = s;
    seen_ = checkpoint.seen;
    seen_count_ = checkpoint.seen_count;
    next_slot_ = checkpoint.next_slot;
    return true;
}

}  // namespace kid_app
