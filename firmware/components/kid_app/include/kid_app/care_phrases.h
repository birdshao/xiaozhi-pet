#pragma once

#include "kid_app/care_calendar.h"
#include "kid_app/care_store.h"

#include <cstddef>
#include <cstdint>

namespace kid_app {

// V8 reviewed care lines (docs/V8_CARE_AND_GROWTH.md §素材). Same rules as
// the V5 Personality phrases: fixed, reviewed copy with stable ids, chosen
// by deterministic rules only; never generated, never free text. Ids
// 1000..1999 are clip ids in the KPS1 sound pack (tools/care_phrases.py
// makes the TTS / recording checklist from this table). Without a clip the
// board plays the context chime, so a missing recording never blocks.

enum class CareLine : uint8_t {
    DiaryOpen, DiaryFood, DiaryBook, DiaryGame, DiaryCare, DiaryHabit, DiaryQuiet, DiaryClose,
    MorningHello, MorningFeel, FestivalHello, HabitRemind, HabitTicked, HabitAlready,
    FoodReact, FeedPrompt, BathStart, BathRinse, BathDone, BrushStart, BrushDone,
    TickleStart, TickleDone, LullStart, LullDone, TidyStart, TidyDone, StickerNew, DressDone,
    GentlePlease,
    // V9 (docs/V9_MENU_DEEPENING_PROPOSAL.md slice 1C). BathDone / BrushDone
    // / TickleDone also gain args 1..3 (CheerArg: the line of the day, one
    // set per temperament); arg 0 stays the shared V8 line.
    TickleSeek,      // dated tickle: dial the feather to the itch, then shake
    TickleLaugh,     // the most ticklish spot (TicklishSpot)
    DiaryGameNamed,  // arg = growth memory id of today's last game
    BoredGone,       // a game finished while TuanTuan was bored (level >= 2)
    SongAsk,         // bedtime: "a lullaby?" (two cards)
    GrowMorning,     // C3: arg = the stage reached (good morning after it)
    GrowDiary,       // C3: arg = the stage reached today (before good night)
    WishHit,         // C4: fed today's wish (three hearts)
    // C4 also adds DiaryFood args 7 (dumplings), 8 (zongzi), 9 (mooncake).
};

// V9 lines (ids kFirstDraftCareId..) are TTS drafts until an adult has
// listened to each one; tools/care_phrases.py marks them in the checklist.
// The firmware treats them like any line: no clip in the pack -> the chime.
constexpr uint16_t kFirstDraftCareId = 1300;
constexpr bool CarePhraseDraft(uint16_t id) { return id >= kFirstDraftCareId; }

// How the room feels in the morning (SHTC3 reading).
enum class Feel : uint8_t { Unknown, Hot, Cold, Dry, Humid, Comfy };
// temp in 0.1 degC, humidity in %; valid false -> Unknown.
Feel FeelOf(int16_t temp_dc, uint8_t humidity_pct, bool valid);

struct CarePhrase {
    uint16_t id;
    CareLine line;
    uint8_t arg;          // food / bucket / habit / feel / festival / taste, else 0
    int8_t temperament;   // Temperament value, or -1 for every temperament
    const char* text;     // reviewed copy (UTF-8)
};

size_t CarePhraseCount();
const CarePhrase& CarePhraseAt(size_t i);
const CarePhrase* FindCarePhrase(uint16_t id);
// Temperament-specific line first, else the shared one; 0 when none.
uint16_t CarePhraseId(CareLine line, uint8_t arg, Temperament t);

// "Today" for the bedtime diary: what was eaten, read, played and how
// often the child cared for TuanTuan. Built only from the ledger (care +
// memory) for `day`; a stale day reads as an empty one.
struct DiaryPage {
    uint32_t day = 0;
    uint8_t last_food = 0;   // Food
    uint8_t books = 0, games = 0;
    uint16_t last_game = 0;  // V9 G5: growth memory id of today's last game
    uint16_t cares = 0;      // feeds + baths + brushes + tickles + lulls
    Habit habit = Habit::None;
    bool habit_done = false;
    uint8_t grew = 0;        // V9 C3: growth stage reached today (GrewOn), 0 = none
    Treat treat = Treat::None;  // V9 C4: today's festival treat (food 8's picture / name)
    bool Empty() const { return !last_food && !books && !games && !cares && !habit_done; }
};
DiaryPage MakeDiaryPage(const CareStore& care, const MemoryStore& memory, uint32_t day);
// Care-count wording bucket: 1, 2, 3, or 4 = "lots".
uint8_t CareBucket(uint16_t cares);

constexpr size_t kMaxDiaryLines = 8;
// Clip ids to read out, in order: opening, food, book, game, care count,
// habit, closing (or opening, "quiet day", closing). Returns the count.
size_t ComposeDiary(const DiaryPage& page, Temperament t, uint16_t* out, size_t max);
// V9 G5: after a finished game, "not bored any more" when the bored need
// was at level >= 2 before it; 0 otherwise.
uint16_t BoredReliefClip(uint8_t bored_level, Temperament t);
// Good morning: hello, room feeling, festival, habit reminder.
// grew: the growth stage reached yesterday or today (GrewOn), 0 = none.
size_t ComposeMorning(Temperament t, Feel feel, Festival festival, Habit habit, uint16_t* out,
                      size_t max, uint8_t grew = 0);

}  // namespace kid_app
