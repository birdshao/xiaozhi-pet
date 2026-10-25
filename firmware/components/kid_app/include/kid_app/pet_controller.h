#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace kid_app {

enum class PetMode : uint8_t {
    Idle, Interact, Story, Listening, Speaking, Quiet
};
enum class PetAction : uint8_t {
    Pickup, Feed, StartStory, NextPage, StopStory,
    VoiceBegin, VoiceReplyBegin, VoiceEnd,
    QuietBegin, QuietEnd
};
enum class PetMood : uint8_t { Neutral, Happy, Curious, Thinking, Sleepy };
enum class PetResult : uint8_t { Applied, Duplicate, Rejected };

struct PetEvent {
    uint64_t id = 0;            // Persistently unique on the producing device.
    PetAction action = PetAction::Pickup;
};

struct PetSnapshot {
    uint32_t schema_version = 1;
    PetMode mode = PetMode::Idle;
    PetMood mood = PetMood::Neutral;
    uint32_t interaction_count = 0;
    uint16_t active_page = 1;
    bool quiet = false;
};

struct PetEffects {
    bool repaint = false;
    bool play_positive_sound = false;
    bool enter_voice = false;
    bool stop_voice = false;
    bool persist = false;
};

class PetController {
public:
    // In-memory last-128 event deduplication; durable journal follows in P2-05.
    static constexpr size_t kSeenCapacity = 128;
    PetResult Apply(const PetEvent& event, PetEffects* out);
    const PetSnapshot& Snapshot() const { return state_; }
    void ResetForTest();

private:
    bool WasSeen(uint64_t id) const;
    void Remember(uint64_t id);
    std::array<uint64_t, kSeenCapacity> seen_{};
    size_t seen_count_ = 0;
    size_t next_slot_ = 0;
    PetSnapshot state_{};
};

}  // namespace kid_app
