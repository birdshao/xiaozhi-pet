#pragma once

#include "kid_app/game_view.h"
#include "kid_app/sound_view.h"

#include <cstddef>
#include <cstdint>

namespace kid_app {

// V8 shared entry point (docs/V8_CARE_AND_GROWTH.md §"活动列表"): the cards
// the "play" menu (home rail Game item) and the "care" menu (home rail Feed
// item) offer, as ONE data table. A new activity is one appended row plus
// one dispatch branch in the board (HandleMenuOutput); nothing else in the
// menu changes. Header-only on purpose: adding a row never adds a source
// file to a build line.
//
// Rules for other sessions:
//  - Ids are stable: append a GameChoice value, never renumber or reuse.
//  - Append the row at the end of its group: the order is the card order.
//  - A Play row names its menu page (`category`, V9 1A); a Care row has none.
//    A new Play row goes to the page where a child would look for it.
//  - `art` is a picture SceneComposer already draws (GameArt / MicArt /
//    CareArt ids); a new picture is appended to its own enum first.
//  - `needs` lets the board refuse a card the parents switched off
//    (motion) without knowing the game.

enum class GameChoice : uint8_t {
    None = 0,
    // V4-06/07/08 play menu.
    Quiz = 1, Ball = 2, Peekaboo = 3, Candle = 4, Clap = 5,
    // V8 care menu (session ①).
    Feed = 6, Bath = 7, Brush = 8, Tickle = 9, Dress = 10, Habit = 11,
    // V8 play menu additions (session ①).
    Stickers = 12,
    // V8 mini games (session ②, play_content.h): 12 + PlayGame, 13..30.
    Memory4 = 13, Memory6 = 14, Maze = 15, Dice = 16, Tree = 17, Statue = 18, DayNight = 19,
    Shapes = 20, Counting = 21, SpotDiff = 22, MamaBaby = 23, SizeOrder = 24, Pattern = 25,
    Classify = 26, Shadow = 27, LeftRight = 28, Clock = 29, Weather = 30,
    // V8 sound (session 3, sound_view.h): ids 31..34, pictures SoundArt 701..705.
    Parrot = 31, Songs = 32, ToothBrush = 33, HandWash = 34,
    // Other sessions: append here (35, 36, ...).
};

enum class ActivityGroup : uint8_t { Play, Care };
enum class ActivityNeeds : uint8_t { None, Motion, Mic };

// V9 1A: the play menu's first level. Every Play row names the page it is
// listed on (Care rows: None). Today is the virtual first page, the games
// recommend picks (activity_recommend.h): no row belongs to it. Album is a
// page of one: its card opens the sticker album directly.
enum class ActivityCategory : uint8_t { None, Today, Brain, Move, Sound, Album };

constexpr const char* ActivityCategoryName(ActivityCategory c) {
    switch (c) {
        case ActivityCategory::Today: return "today";
        case ActivityCategory::Brain: return "brain";
        case ActivityCategory::Move: return "move";
        case ActivityCategory::Sound: return "sound";
        case ActivityCategory::Album: return "album";
        case ActivityCategory::None: break;
    }
    return "none";
}

// Category card pictures (V9 1A, block 200..299 was free): drawn by
// menu_draw.inc next to the other game pictures.
enum class MenuArt : uint16_t { Today = 201, Brain = 202, Move = 203, Sound = 204 };
constexpr bool MenuArtValid(uint16_t id) { return id >= 201 && id <= 204; }

// Picture id ranges (one per session, docs/V8_CARE_AND_GROWTH.md §5):
//   1..99 GameArt (KPG1 catalogue)   101..199 MicArt (V4-08)
//   401..513 (block 400..599) and 0x2000..0x3FFF PlayArt (session 2, game_view.h)
//   601..660 (block 600..699) CareArt (session 1, below)
//   701.. (block 700..799) session 3 (sound)   801.. (block 800..899) session 4
//   201..204 MenuArt (V9 1A, below)   205..399 free; 0x4000.. never: tone clip ids (GameArtTone)
// V8 care pictures (append-only, drawn by SceneComposer next to GameArt).
enum class CareArt : uint16_t {
    Bubbles = 601, Comb = 602, Feather = 603, Hanger = 604, Check = 605, Sticker = 606,
    // Foods (FoodId 1..6 -> 611..616).
    Fish = 611, Milk = 612, Apple = 613, Carrot = 614, Cookie = 615, RiceBall = 616,
    // Toys for the bedtime tidy-up.
    ToyBlock = 621, ToyBall = 622, ToyBear = 623,
    // Wardrobe cards: 631 + index in the wardrobe table (care_store.cc).
    WearFirst = 631, WearLast = 642,
    // Good-habit cards: 650 + Habit (1..8).
    HabitFirst = 651, HabitLast = 658,
    Lock = 660,  // nothing unlocked yet
};
constexpr bool CareArtValid(uint16_t id) {
    return (id >= 601 && id <= 606) || (id >= 611 && id <= 616) || (id >= 621 && id <= 623) ||
           (id >= 631 && id <= 642) || (id >= 651 && id <= 658) || id == 660;
}

struct Activity {
    GameChoice id;
    ActivityGroup group;
    uint16_t art;
    ActivityNeeds needs;
    const char* name;  // log / doc name, lower case ASCII
    ActivityCategory category = ActivityCategory::None;  // Play rows: the menu page
};

constexpr Activity kActivities[] = {
    {GameChoice::Quiz, ActivityGroup::Play, static_cast<uint16_t>(GameArt::Star),
     ActivityNeeds::None, "quiz", ActivityCategory::Brain},
    {GameChoice::Ball, ActivityGroup::Play, static_cast<uint16_t>(GameArt::BallBig),
     ActivityNeeds::Motion, "ball", ActivityCategory::Move},
    {GameChoice::Peekaboo, ActivityGroup::Play, static_cast<uint16_t>(GameArt::Cat),
     ActivityNeeds::Motion, "peekaboo", ActivityCategory::Move},
    {GameChoice::Candle, ActivityGroup::Play, static_cast<uint16_t>(MicArt::CandleLit),
     ActivityNeeds::Mic, "candle", ActivityCategory::Sound},
    {GameChoice::Clap, ActivityGroup::Play, static_cast<uint16_t>(MicArt::Hand),
     ActivityNeeds::Mic, "clap", ActivityCategory::Sound},
    // V8 mini games (session ②), in menu order: age, then level (play_content.cc).
    {GameChoice::Shapes, ActivityGroup::Play, static_cast<uint16_t>(GameArt::Triangle),
     ActivityNeeds::None, "shapes", ActivityCategory::Brain},
    {GameChoice::Counting, ActivityGroup::Play, static_cast<uint16_t>(GameArt::Dots3),
     ActivityNeeds::None, "counting", ActivityCategory::Brain},
    {GameChoice::DayNight, ActivityGroup::Play, static_cast<uint16_t>(PlayArt::Moon),
     ActivityNeeds::Motion, "day_night", ActivityCategory::Move},
    {GameChoice::Memory4, ActivityGroup::Play, static_cast<uint16_t>(GameArt::Blocks4),
     ActivityNeeds::None, "memory4", ActivityCategory::Brain},
    {GameChoice::MamaBaby, ActivityGroup::Play, static_cast<uint16_t>(PlayArt::BabyCat),
     ActivityNeeds::None, "mama_baby", ActivityCategory::Brain},
    {GameChoice::Classify, ActivityGroup::Play, static_cast<uint16_t>(PlayArt::Apple),
     ActivityNeeds::None, "classify", ActivityCategory::Brain},
    {GameChoice::Tree, ActivityGroup::Play, PlayTreeArt(3), ActivityNeeds::Motion,
     "shake_tree", ActivityCategory::Move},
    {GameChoice::Shadow, ActivityGroup::Play, PlayShadowArt(static_cast<uint16_t>(GameArt::Star)),
     ActivityNeeds::None, "shadow", ActivityCategory::Brain},
    {GameChoice::Statue, ActivityGroup::Play, static_cast<uint16_t>(PlayArt::Statue),
     ActivityNeeds::Motion, "statue", ActivityCategory::Move},
    {GameChoice::Weather, ActivityGroup::Play, static_cast<uint16_t>(PlayArt::Sun),
     ActivityNeeds::None, "weather", ActivityCategory::Brain},
    {GameChoice::LeftRight, ActivityGroup::Play, PlayArrowArt(0, 3), ActivityNeeds::None,
     "left_right", ActivityCategory::Brain},
    {GameChoice::SpotDiff, ActivityGroup::Play, static_cast<uint16_t>(PlayArt::Pos2),
     ActivityNeeds::None, "spot_diff", ActivityCategory::Brain},
    {GameChoice::Dice, ActivityGroup::Play, PlayDiceArt(5), ActivityNeeds::Motion,
     "shake_dice", ActivityCategory::Move},
    {GameChoice::Memory6, ActivityGroup::Play, static_cast<uint16_t>(PlayArt::Dots6),
     ActivityNeeds::None, "memory6", ActivityCategory::Brain},
    {GameChoice::SizeOrder, ActivityGroup::Play, PlaySizedArt(0, 2), ActivityNeeds::None,
     "size_order", ActivityCategory::Brain},
    {GameChoice::Pattern, ActivityGroup::Play, PlayPatternArt(0, 0, 1, 0), ActivityNeeds::None,
     "pattern", ActivityCategory::Brain},
    {GameChoice::Maze, ActivityGroup::Play, static_cast<uint16_t>(PlayArt::Carrot),
     ActivityNeeds::Motion, "maze", ActivityCategory::Move},
    {GameChoice::Clock, ActivityGroup::Play, PlayClockArt(3, false), ActivityNeeds::None,
     "clock", ActivityCategory::Brain},
    {GameChoice::Stickers, ActivityGroup::Play, static_cast<uint16_t>(CareArt::Sticker),
     ActivityNeeds::None, "stickers", ActivityCategory::Album},
    // V8 sound (session 3): the parrot and the song player.
    {GameChoice::Parrot, ActivityGroup::Play, static_cast<uint16_t>(SoundArt::Parrot),
     ActivityNeeds::Mic, "parrot", ActivityCategory::Sound},
    {GameChoice::Songs, ActivityGroup::Play, static_cast<uint16_t>(SoundArt::Song),
     ActivityNeeds::None, "songs", ActivityCategory::Sound},
    // ---- other sessions: append Play rows above this line ----
    {GameChoice::Feed, ActivityGroup::Care, static_cast<uint16_t>(CareArt::Fish),
     ActivityNeeds::None, "feed"},
    {GameChoice::Bath, ActivityGroup::Care, static_cast<uint16_t>(CareArt::Bubbles),
     ActivityNeeds::Motion, "bath"},
    {GameChoice::Brush, ActivityGroup::Care, static_cast<uint16_t>(CareArt::Comb),
     ActivityNeeds::None, "brush"},
    {GameChoice::Tickle, ActivityGroup::Care, static_cast<uint16_t>(CareArt::Feather),
     ActivityNeeds::Motion, "tickle"},
    {GameChoice::Dress, ActivityGroup::Care, static_cast<uint16_t>(CareArt::Hanger),
     ActivityNeeds::None, "dress"},
    {GameChoice::Habit, ActivityGroup::Care, static_cast<uint16_t>(CareArt::Check),
     ActivityNeeds::None, "habit"},
    // V8 sound (session 3) care routines: tooth brushing (2 min), hand washing (20 s).
    {GameChoice::ToothBrush, ActivityGroup::Care, static_cast<uint16_t>(SoundArt::ToothBrush),
     ActivityNeeds::None, "tooth_brush"},
    {GameChoice::HandWash, ActivityGroup::Care, static_cast<uint16_t>(SoundArt::HandWash),
     ActivityNeeds::None, "hand_wash"},
    // ---- care rows (session 1) end here ----
};
constexpr size_t kActivityRows = sizeof(kActivities) / sizeof(kActivities[0]);

constexpr size_t ActivityCount(ActivityGroup group) {
    size_t n = 0;
    for (const Activity& a : kActivities)
        if (a.group == group) ++n;
    return n;
}
// index-th card of the group (0-based), nullptr past the end.
constexpr const Activity* ActivityAt(ActivityGroup group, size_t index) {
    for (const Activity& a : kActivities) {
        if (a.group != group) continue;
        if (index == 0) return &a;
        --index;
    }
    return nullptr;
}
// Rows of one category page, in table order (the page's card order).
constexpr size_t ActivityCountIn(ActivityCategory category) {
    size_t n = 0;
    for (const Activity& a : kActivities)
        if (a.group == ActivityGroup::Play && a.category == category) ++n;
    return n;
}
constexpr const Activity* ActivityInCategory(ActivityCategory category, size_t index) {
    for (const Activity& a : kActivities) {
        if (a.group != ActivityGroup::Play || a.category != category) continue;
        if (index == 0) return &a;
        --index;
    }
    return nullptr;
}
constexpr const Activity* FindActivity(GameChoice id) {
    for (const Activity& a : kActivities)
        if (a.id == id) return &a;
    return nullptr;
}
// Card index of `id` inside its group (to reopen a menu on that card), or 0.
constexpr uint8_t ActivityIndexOf(GameChoice id) {
    const Activity* found = FindActivity(id);
    if (!found) return 0;
    uint8_t i = 0;
    for (const Activity& a : kActivities) {
        if (a.group != found->group) continue;
        if (a.id == id) return i;
        ++i;
    }
    return 0;
}

// Table sanity, checked at compile time: unique ids, known pictures.
constexpr bool ActivityTableValid() {
    for (size_t i = 0; i < kActivityRows; ++i) {
        const Activity& a = kActivities[i];
        if (a.id == GameChoice::None || !a.name || !a.art) return false;
        if (!GameArtIsOption(a.art) && !MicArtValid(a.art) && !CareArtValid(a.art) &&
            !PlayArtValid(a.art) && !SoundArtValid(a.art))
            return false;
        // V9: Play rows sit on a real page, Care rows on none.
        if ((a.group == ActivityGroup::Care) != (a.category == ActivityCategory::None))
            return false;
        if (a.category == ActivityCategory::Today) return false;
        for (size_t j = 0; j < i; ++j)
            if (kActivities[j].id == a.id) return false;
    }
    return ActivityCount(ActivityGroup::Play) >= 3 && ActivityCount(ActivityGroup::Care) >= 3;
}
static_assert(ActivityTableValid(),
              "activity table: unique ids, known art, categories, >= 3 cards a menu");

}  // namespace kid_app
