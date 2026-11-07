#pragma once

#include "kid_app/memory_store.h"
#include "kid_app/rhythm_scheduler.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace kid_app {

// V8 care and growth (docs/V8_CARE_AND_GROWTH.md). Pure data and rules, no
// clock source of its own: every time is a local "care minute" (minutes
// since 1970-01-01 00:00 local wall clock, see CareMinute) passed in by the
// caller; 0 = the clock is unknown. Owned by PetAtomicLedger and committed
// in the same NVS transaction as the Pet checkpoint and the memory (ledger
// schema v4). Nothing here can punish: needs only ever drive a face and a
// small icon, there is no illness, death, score loss or level loss.

// Local minute number for a plausible wall clock, else 0 (unknown).
uint32_t CareMinute(const CivilTime& now, bool clock_ok);
// Local civil day (MemoryEvent::day convention) of a care minute.
constexpr uint32_t CareDay(uint32_t minute) { return minute / 1440u; }

// ---- Foods and taste ----
enum class Food : uint8_t { None, Fish, Milk, Apple, Carrot, Cookie, RiceBall };
constexpr uint8_t kFoodCount = 6;
// No "dislike": every food is at least Okay.
enum class Taste : uint8_t { Okay, Like, Love };
Taste TasteOf(Temperament t, Food food);
const char* FoodName(Food food);

// ---- Needs ----
enum class Need : uint8_t { None, Hungry, Sleepy, Dirty, Bored };
constexpr uint8_t kNeedMax = 3;  // levels 0..3; >= 2 shows on the home face
struct NeedLevels {
    uint8_t hungry = 0, sleepy = 0, dirty = 0, bored = 0;
    bool operator==(const NeedLevels& o) const {
        return hungry == o.hungry && sleepy == o.sleepy && dirty == o.dirty && bored == o.bored;
    }
};

// ---- Stickers (one per growth milestone; ids are bit numbers, append-only) ----
enum class Sticker : uint8_t {
    FirstBook, FiveBooks, TenBooks, FirstGame, TenGames, FirstLike, SevenDays,
    FirstFeed, TenFeeds, FirstBath, FirstBrush, FirstTickle, FirstLull,
    Habit3, Habit7, Habit14, Birthday, Festival, CareStar,
};
constexpr uint8_t kStickerCount = 19;
constexpr uint32_t kAllStickers = (1u << kStickerCount) - 1;
constexpr uint32_t StickerBit(Sticker s) { return 1u << static_cast<uint8_t>(s); }
const char* StickerName(Sticker s);

// ---- Wardrobe and the little room ----
enum class WearSlot : uint8_t { Head, Neck, Face, Bed, Lamp };
constexpr uint8_t kWearSlots = 5;
// Item ids per slot start at 1 (0 = nothing worn). Each item is unlocked by
// one sticker, so dressing up is a deterministic function of growth.
struct WardrobeItem {
    WearSlot slot;
    uint8_t item;
    Sticker unlock;
    const char* name;
};
size_t WardrobeCount();
const WardrobeItem& WardrobeAt(size_t i);
const WardrobeItem* FindWardrobe(WearSlot slot, uint8_t item);
bool WardrobeUnlocked(const WardrobeItem& w, uint32_t stickers);
uint8_t WardrobeItemsIn(WearSlot slot);  // highest item id of the slot

// ---- Good habits (the parent picks one preset) ----
enum class Habit : uint8_t {
    None, BrushTeeth, WashHands, TidyToys, DrinkWater, DressSelf, EatVeggies, BedOnTime, ReadBook,
};
constexpr uint8_t kHabitCount = 8;
const char* HabitName(Habit h);

// ---- Festivals (detected in care_calendar.h; listed here for the sticker) ----
enum class Festival : uint8_t {
    None, NewYear, SpringFestival, Lantern, ChildrensDay, DragonBoat, MidAutumn, Birthday,
};
constexpr uint8_t kFestivalCount = 7;
const char* FestivalName(Festival f);

// ---- Events ----
enum class CareEventKind : uint8_t {
    Fed,           // option = Food, from a dial choice in the food menu
    Bathed,        // the bath session finished (shake bubbles, flip to rinse)
    Brushed,       // the brush session finished (dial back and forth)
    Tickled,       // the tickle session finished (gentle shakes)
    Lulled,        // the lullaby finished (face down / resting)
    HabitDone,     // the child ticked today's habit
    Dressed,       // slot = WearSlot, option = item (0 = take off)
    HabitSet,      // option = Habit (parent menu)
    BirthdaySet,   // slot = month (0 clears), option = day
    FestivalSeen,  // option = Festival (care_calendar.h), the device saw it
};

struct CareEvent {
    uint64_t id = 0;  // shares the Pet replay window
    CareEventKind kind = CareEventKind::Fed;
    MemorySource source = MemorySource::Device;
    uint32_t minute = 0;  // CareMinute, 0 = clock unknown
    uint16_t slot = 0;
    uint16_t option = 0;
};

enum class CareResult : uint8_t { Applied, Rejected };

class CareStore {
public:
    static constexpr size_t kBytes = 96;   // stable LE encoding (ledger v4)
    static constexpr uint8_t kTodayCap = 250;
    // Need growth per level (minutes). Slow on purpose: the pet "asks",
    // never suffers.
    static constexpr uint32_t kHungryStepMin = 4 * 60;
    static constexpr uint32_t kDirtyStepMin = 12 * 60;
    static constexpr uint32_t kBoredStepMin = 3 * 60;
    static constexpr uint32_t kRestedMin = 90;  // a lullaby keeps sleepiness away

    struct Today {
        uint32_t day = 0;
        uint8_t foods = 0;      // bit (food - 1) per food eaten today
        uint8_t last_food = 0;  // Food
        uint8_t feeds = 0, baths = 0, brushes = 0, tickles = 0, lulls = 0;
        bool habit_done = false;
        uint16_t Cares() const {
            return static_cast<uint16_t>(feeds + baths + brushes + tickles + lulls);
        }
        bool operator==(const Today& o) const;
    };
    struct Totals {
        uint16_t feeds = 0, baths = 0, brushes = 0, tickles = 0, lulls = 0;
        uint32_t All() const {
            return uint32_t(feeds) + baths + brushes + tickles + lulls;
        }
    };
    struct HabitTrack {
        Habit habit = Habit::None;
        uint16_t streak = 0;     // consecutive days ending on last_day
        uint16_t best = 0;
        uint16_t total = 0;      // days ticked, ever
        uint32_t last_day = 0;   // civil day of the last tick, 0 = never
    };

    // Guards the source, validates the payload, then mutates; Rejected
    // leaves the store unchanged. Voice / Network never apply. Stickers
    // are NOT re-evaluated here: the ledger calls EarnStickers with the
    // memory growth inside the same commit.
    CareResult Apply(const CareEvent& event);
    // Books and games keep the pet from getting bored (same commit as the
    // MemoryEvent). minute 0 = unknown: nothing changes.
    void NotePlay(uint32_t minute);
    // Sticky: a sticker once earned stays (only a parent memory wipe clears
    // it). Returns the newly earned bits.
    uint32_t EarnStickers(const MemoryStore::Growth& growth);
    // Parent "forget everything": back to just met, the parent's choices
    // (habit preset, birthday) are kept.
    void WipeChildData();

    NeedLevels Needs(uint32_t minute, DayPhase phase) const;
    // Most pressing need of level >= 2 (ties: hungry, sleepy, dirty, bored).
    static Need TopNeed(const NeedLevels& n, uint8_t* level = nullptr);
    // Favourite by count (ties: lower food id); None before the first meal.
    Food MostFed() const;
    uint8_t Worn(WearSlot slot) const { return wear_[static_cast<uint8_t>(slot)]; }
    uint32_t Stickers() const { return stickers_; }
    const Today& GetToday() const { return today_; }
    // Today's counters only when they belong to `day` (else empty).
    Today TodayOn(uint32_t day) const;
    const Totals& GetTotals() const { return totals_; }
    const HabitTrack& GetHabit() const { return habit_; }
    uint16_t FoodCount(Food f) const;
    uint8_t BirthMonth() const { return birth_month_; }
    uint8_t BirthDay() const { return birth_day_; }

    bool Encode(uint8_t* out, size_t size) const;
    // Strict canonical decode; false leaves *out unchanged.
    static bool Decode(const uint8_t* in, size_t size, CareStore* out);
    bool operator==(const CareStore& o) const;
    bool operator!=(const CareStore& o) const { return !(*this == o); }

private:
    static bool SourceAllowed(CareEventKind kind, MemorySource source);
    void RollTo(uint32_t day);
    bool CountsToday(uint32_t day) const { return !day || day == today_.day; }
    void SeedAnchors(uint32_t minute);

    uint8_t birth_month_ = 0, birth_day_ = 0;
    std::array<uint8_t, kWearSlots> wear_{};
    // Need anchors (care minutes, 0 = never / unknown).
    uint32_t fed_at_ = 0, rested_at_ = 0, bathed_at_ = 0, played_at_ = 0;
    std::array<uint16_t, kFoodCount> food_counts_{};
    uint32_t stickers_ = 0;
    HabitTrack habit_{};
    Totals totals_{};
    Today today_{};
};

bool ValidBirthday(uint8_t month, uint8_t day);

}  // namespace kid_app
