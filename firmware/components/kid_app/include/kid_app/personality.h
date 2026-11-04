#pragma once

#include "kid_app/home_menu.h"
#include "kid_app/memory_store.h"
#include "kid_app/pet_controller.h"
#include "kid_app/rhythm_scheduler.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace kid_app {

// Where a line is spoken. Every context has at least one slot-free template
// per temperament, so selection never fails.
enum class PhraseContext : uint8_t {
    Greeting, MorningRecap, FavoriteMention, Feed, StoryInvite, Bedtime
};
constexpr size_t kPhraseContextCount = 6;

// Memory slot a template needs; filled later from MemoryStore, never from
// free text. {book} = a recent book title, {like} = a chosen option name.
enum class PhraseSlot : uint8_t { None, Book, Like };

struct PhraseTemplate {
    uint16_t id;  // stable: audio clips and parent review refer to it
    Temperament temperament;
    PhraseContext context;
    PhraseSlot slot;
    uint8_t weight;
    const char* text;  // reviewed copy (UTF-8); slot markers {book}/{like}
};

struct PersonalityParams {
    Temperament temperament;
    uint16_t catchphrase_id;  // a PhraseTemplate id
    PetMood resting_mood;     // expression tendency when idle
    // Base proposal preference per HomeItem (Chat, Story, Game, Feed, Sleep):
    // curious likes books, gentle likes chatting, lively likes games.
    std::array<uint8_t, HomeMenu::kItemCount> proposal_weights;
};

// V5-01: fixed, reviewed, deterministic. Same inputs -> same outputs; no
// randomness source, no model. The caller supplies `seed` (e.g. ledger
// generation) to rotate between equally valid choices.
class Personality {
public:
    static bool Valid(Temperament t);
    static const PersonalityParams& Params(Temperament t);

    // Which home item the pet suggests in this phase of the day.
    static uint8_t ProposalWeight(Temperament t, DayPhase phase, HomeItem item);
    static HomeItem Propose(Temperament t, DayPhase phase, uint32_t seed);

    // Template id for this context, choosing only templates whose memory
    // slot is available. Returns 0 only for invalid input.
    static uint16_t SelectPhrase(Temperament t, PhraseContext context,
                                 bool has_book, bool has_like, uint32_t seed);
    // Convenience: slot availability from the pet's memory.
    static uint16_t SelectPhrase(const MemoryStore& memory, PhraseContext context,
                                 uint32_t seed);

    static const PhraseTemplate* FindPhrase(uint16_t id);
    static size_t PhraseCount();
    static const PhraseTemplate& PhraseAt(size_t index);
};

}  // namespace kid_app
