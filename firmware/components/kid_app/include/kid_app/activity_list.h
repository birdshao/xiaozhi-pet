#pragma once

#include "kid_app/game_view.h"

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
    // Other sessions: append here (31, 32, ...).
};

enum class ActivityGroup : uint8_t { Play, Care };
enum class ActivityNeeds : uint8_t { None, Motion, Mic };

// V8 care pictures (append-only, drawn by SceneComposer next to GameArt).
enum class CareArt : uint16_t {
    Bubbles = 201, Comb = 202, Feather = 203, Hanger = 204, Check = 205, Sticker = 206,
    // Foods (FoodId 1..6 -> 211..216).
    Fish = 211, Milk = 212, Apple = 213, Carrot = 214, Cookie = 215, RiceBall = 216,
    // Toys for the bedtime tidy-up.
    ToyBlock = 221, ToyBall = 222, ToyBear = 223,
    // Wardrobe cards: 231 + index in the wardrobe table (care_store.cc).
    WearFirst = 231, WearLast = 242,
    // Good-habit cards: 250 + Habit (1..8).
    HabitFirst = 251, HabitLast = 258,
    Lock = 260,  // nothing unlocked yet
};
constexpr bool CareArtValid(uint16_t id) {
    return (id >= 201 && id <= 206) || (id >= 211 && id <= 216) || (id >= 221 && id <= 223) ||
           (id >= 231 && id <= 242) || (id >= 251 && id <= 258) || id == 260;
}

struct Activity {
    GameChoice id;
    ActivityGroup group;
    uint16_t art;
    ActivityNeeds needs;
    const char* name;  // log / doc name, lower case ASCII
};

constexpr Activity kActivities[] = {
    {GameChoice::Quiz, ActivityGroup::Play, static_cast<uint16_t>(GameArt::Star),
     ActivityNeeds::None, "quiz"},
    {GameChoice::Ball, ActivityGroup::Play, static_cast<uint16_t>(GameArt::BallBig),
     ActivityNeeds::Motion, "ball"},
    {GameChoice::Peekaboo, ActivityGroup::Play, static_cast<uint16_t>(GameArt::Cat),
     ActivityNeeds::Motion, "peekaboo"},
    {GameChoice::Candle, ActivityGroup::Play, static_cast<uint16_t>(MicArt::CandleLit),
     ActivityNeeds::Mic, "candle"},
    {GameChoice::Clap, ActivityGroup::Play, static_cast<uint16_t>(MicArt::Hand),
     ActivityNeeds::Mic, "clap"},
    // V8 mini games (session ②), in menu order: age, then level (play_content.cc).
    {GameChoice::Shapes, ActivityGroup::Play, static_cast<uint16_t>(GameArt::Triangle),
     ActivityNeeds::None, "shapes"},
    {GameChoice::Counting, ActivityGroup::Play, static_cast<uint16_t>(GameArt::Dots3),
     ActivityNeeds::None, "counting"},
    {GameChoice::DayNight, ActivityGroup::Play, static_cast<uint16_t>(PlayArt::Moon),
     ActivityNeeds::Motion, "day_night"},
    {GameChoice::Memory4, ActivityGroup::Play, static_cast<uint16_t>(GameArt::Blocks4),
     ActivityNeeds::None, "memory4"},
    {GameChoice::MamaBaby, ActivityGroup::Play, static_cast<uint16_t>(PlayArt::BabyCat),
     ActivityNeeds::None, "mama_baby"},
    {GameChoice::Classify, ActivityGroup::Play, static_cast<uint16_t>(PlayArt::Apple),
     ActivityNeeds::None, "classify"},
    {GameChoice::Tree, ActivityGroup::Play, PlayTreeArt(3), ActivityNeeds::Motion, "shake_tree"},
    {GameChoice::Shadow, ActivityGroup::Play, PlayShadowArt(static_cast<uint16_t>(GameArt::Star)),
     ActivityNeeds::None, "shadow"},
    {GameChoice::Statue, ActivityGroup::Play, static_cast<uint16_t>(PlayArt::Statue),
     ActivityNeeds::Motion, "statue"},
    {GameChoice::Weather, ActivityGroup::Play, static_cast<uint16_t>(PlayArt::Sun),
     ActivityNeeds::None, "weather"},
    {GameChoice::LeftRight, ActivityGroup::Play, PlayArrowArt(0, 3), ActivityNeeds::None,
     "left_right"},
    {GameChoice::SpotDiff, ActivityGroup::Play, static_cast<uint16_t>(PlayArt::Pos2),
     ActivityNeeds::None, "spot_diff"},
    {GameChoice::Dice, ActivityGroup::Play, PlayDiceArt(5), ActivityNeeds::Motion, "shake_dice"},
    {GameChoice::Memory6, ActivityGroup::Play, static_cast<uint16_t>(PlayArt::Dots6),
     ActivityNeeds::None, "memory6"},
    {GameChoice::SizeOrder, ActivityGroup::Play, PlaySizedArt(0, 2), ActivityNeeds::None,
     "size_order"},
    {GameChoice::Pattern, ActivityGroup::Play, PlayPatternArt(0, 0, 1, 0), ActivityNeeds::None,
     "pattern"},
    {GameChoice::Maze, ActivityGroup::Play, static_cast<uint16_t>(PlayArt::Carrot),
     ActivityNeeds::Motion, "maze"},
    {GameChoice::Clock, ActivityGroup::Play, PlayClockArt(3, false), ActivityNeeds::None, "clock"},
    {GameChoice::Stickers, ActivityGroup::Play, static_cast<uint16_t>(CareArt::Sticker),
     ActivityNeeds::None, "stickers"},
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
            !PlayArtValid(a.art))
            return false;
        for (size_t j = 0; j < i; ++j)
            if (kActivities[j].id == a.id) return false;
    }
    return ActivityCount(ActivityGroup::Play) >= 3 && ActivityCount(ActivityGroup::Care) >= 3;
}
static_assert(ActivityTableValid(), "activity table: unique ids, known art, >= 3 cards a menu");

}  // namespace kid_app
