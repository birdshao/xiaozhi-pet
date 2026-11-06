#pragma once

#include "kid_app/memory_store.h"
#include "kid_app/personality.h"

#include <cstdint>

namespace kid_app {

// V5-04 (second half): when the pet may use a memory line. Pure selection:
// same inputs -> same outputs, no clock, no randomness, no free text. The
// caller supplies memory, temperament, local day and a millisecond clock.
// Only reviewed Personality templates with a {book}/{like} slot are chosen;
// when there is nothing real to say the answer is "no" and the caller keeps
// the plain greeting (nothing is ever invented).
enum class MemoryPhraseKind : uint8_t {
    MorningRecap,  // good morning: yesterday's book
    PickupLike,    // picked up: a like the child chose on a dial question
};

enum class MemoryPhraseReason : uint8_t {
    Used,
    NoMemory,   // no yesterday book / no chosen like
    Quiet,      // parent quiet hours
    TooSoon,    // frequency cap: gap since the last mention
    DayLimit,   // frequency cap: mentions today
    NoTemplate,
};

struct MemoryPhraseDecision {
    bool use = false;
    MemoryPhraseReason reason = MemoryPhraseReason::NoMemory;
    uint16_t phrase_id = 0;  // template with a slot; 0 when !use
    PhraseSlot slot = PhraseSlot::None;
    uint16_t value = 0;      // book id or like option id
    uint16_t like_slot = 0;  // PickupLike: content-pack topic id
};

const char* MemoryPhraseKindName(MemoryPhraseKind kind);
const char* MemoryPhraseReasonName(MemoryPhraseReason reason);

// Single owner (the board's pet_prompt task); uint32 ms clock, wrap-safe.
class MemoryPhraseSelector {
public:
    // Pickup mentions of a like are occasional: at most this many a local
    // day, never closer together than this.
    static constexpr uint8_t kPickupDayLimit = 3;
    static constexpr uint32_t kPickupMinGapMs = 10u * 60u * 1000u;

    MemoryPhraseDecision Select(MemoryPhraseKind kind, const MemoryStore& memory,
                                uint32_t day, bool quiet, uint32_t now_ms);

private:
    uint32_t pickup_day_ = 0;
    uint8_t pickup_today_ = 0;
    uint32_t pickup_total_ = 0;  // rotates which like is mentioned
    bool pickup_spoke_ = false;
    uint32_t pickup_last_ms_ = 0;
};

}  // namespace kid_app
