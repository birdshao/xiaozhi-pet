#pragma once

#include "kid_app/pet_controller.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace kid_app {

// Parent-chosen temperament (PRD v0.2 §8.1, ADR-012). Fixed parameter, never
// changed by the voice model; survives a memory wipe.
enum class Temperament : uint8_t { Curious, Gentle, Lively };

// Structured memory events only (ADR-012): no free text, no speech content.
enum class MemoryEventKind : uint8_t {
    PreferenceChosen,   // PREFERENCE_CHOSEN: topic slot + option id
    BookFinished,       // STORY_FINISHED: book id
    GameCompleted,      // GAME_COMPLETED: game id
    MemoryCleared,      // MEMORY_CLEARED: parent wipe
    TemperamentSet,     // parent profile: option = Temperament
};

// Who produced the event. Only trusted local rules may write memory, and
// likes come ONLY from a dial choice question (V5-03).
enum class MemorySource : uint8_t {
    DialChoice,  // child turned the dial and pressed on a choice question
    Button,      // BOOT / dial outside a choice question
    Device,      // trusted on-device rule engine (story reader, game rules)
    Parent,      // device parent entry (BOOT + dial 5 s) / parent API
    Voice,       // anything derived from speech or the voice model
    Network,     // unauthenticated or server-forwarded data
};

struct MemoryEvent {
    uint64_t id = 0;      // shares the Pet replay window: same id -> Duplicate
    MemoryEventKind kind = MemoryEventKind::BookFinished;
    MemorySource source = MemorySource::Device;
    uint32_t day = 0;     // local civil day number; 0 = clock unknown
    uint16_t slot = 0;    // PreferenceChosen: content-pack topic id
    uint16_t option = 0;  // PreferenceChosen option / book id / game id /
                          // Temperament
};

enum class MemoryResult : uint8_t { Applied, Rejected };

// Fixed-size, bounded, deterministic three-layer memory (today / likes /
// growth) plus recent books. Pure data: owned by PetAtomicLedger and committed
// in the same NVS transaction as the Pet checkpoint and outbox.
class MemoryStore {
public:
    static constexpr size_t kPreferenceCapacity = 16;
    static constexpr size_t kRecentBookCapacity = 8;
    static constexpr uint8_t kTodayCap = 250;  // saturating daily counters
    static constexpr size_t kBytes = 288;      // stable LE encoding

    struct Day {
        uint32_t day = 0;
        uint8_t feeds = 0;
        uint8_t greetings = 0;
        uint8_t books = 0;
        uint8_t games = 0;
        uint8_t choices = 0;
        uint16_t last_book = 0;
        uint16_t last_game = 0;
        bool Active() const { return feeds || greetings || books || games || choices; }
        bool operator==(const Day& o) const;
    };
    struct Preference {
        uint16_t slot = 0;
        uint16_t option = 0;
        uint64_t source_event = 0;  // audit trail; never sent to voice
    };
    enum Milestone : uint32_t {
        kFirstBook = 1u << 0,
        kFiveBooks = 1u << 1,
        kTenBooks = 1u << 2,
        kFirstGame = 1u << 3,
        kTenGames = 1u << 4,
        kFirstPreference = 1u << 5,
        kSevenDays = 1u << 6,
        kAllMilestones = (1u << 7) - 1,
    };
    struct Growth {
        uint16_t books_finished = 0;
        uint16_t games_completed = 0;
        uint16_t preferences_chosen = 0;
        uint16_t active_days = 0;
        uint32_t milestones = 0;
    };

    // Validates source/kind/payload, then mutates. Rejected leaves the store
    // unchanged. Replay dedupe is the ledger's job (shared event-id window).
    MemoryResult Apply(const MemoryEvent& event);

    // Counted inside the same ledger commit as an applied Pet action. `day`
    // follows the same rollover rules as MemoryEvent::day (0 = unknown).
    void NotePetAction(PetAction action, uint32_t day = 0);

    Temperament GetTemperament() const { return temperament_; }
    const Day& Today() const { return today_; }
    const Day& Yesterday() const { return yesterday_; }
    const Growth& GetGrowth() const { return growth_; }
    size_t PreferenceCount() const { return preference_count_; }
    // Index 0 = most recently chosen.
    const Preference& PreferenceAt(size_t i) const { return preferences_[i]; }
    bool FindPreference(uint16_t slot, uint16_t* option) const;
    size_t RecentBookCount() const { return recent_book_count_; }
    uint16_t RecentBookAt(size_t i) const { return recent_books_[i]; }  // 0 = newest
    // Last book finished on that calendar day if still remembered (today or
    // the last active day), else 0. Morning recap asks for current_day - 1.
    uint16_t BookReadOn(uint32_t day) const;
    bool Empty() const;  // nothing remembered ("just met"), profile ignored

    bool Encode(uint8_t* out, size_t size) const;
    // Strict canonical decode; false leaves *out unchanged.
    static bool Decode(const uint8_t* in, size_t size, MemoryStore* out);

    bool operator==(const MemoryStore& o) const;
    bool operator!=(const MemoryStore& o) const { return !(*this == o); }

private:
    static bool SourceAllowed(MemoryEventKind kind, MemorySource source);
    void Wipe();
    void RollTo(uint32_t day);
    bool CountsToday(uint32_t day) const;
    void RememberBook(uint16_t book);
    void ChoosePreference(uint16_t slot, uint16_t option, uint64_t event_id);
    void UpdateMilestones();

    Temperament temperament_ = Temperament::Gentle;
    Day today_{};
    Day yesterday_{};
    Growth growth_{};
    std::array<Preference, kPreferenceCapacity> preferences_{};
    uint8_t preference_count_ = 0;
    std::array<uint16_t, kRecentBookCapacity> recent_books_{};
    uint8_t recent_book_count_ = 0;
};

// What the voice service may see (ADR-012): temperament, chosen likes and
// today's counts. No event ids, dates, names or parent notes.
struct MinimalSummary {
    Temperament temperament = Temperament::Gentle;
    uint8_t preference_count = 0;
    std::array<std::array<uint16_t, 2>, MemoryStore::kPreferenceCapacity>
        preferences{};  // {slot, option}
    uint8_t feeds = 0, greetings = 0, books = 0, games = 0;
    uint16_t last_book = 0;
    uint32_t milestones = 0;
};

// With a known current_day, counts from an older day are reported as zero
// (nothing happened today yet) instead of leaking a stale "today".
MinimalSummary MakeMinimalSummary(const MemoryStore& memory,
                                  uint32_t current_day = 0);
// Compact JSON for the voice adapter. False if the buffer is too small.
bool FormatMinimalSummary(const MinimalSummary& summary, char* out, size_t size);
const char* TemperamentName(Temperament t);

}  // namespace kid_app
