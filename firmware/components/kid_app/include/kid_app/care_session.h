#pragma once

#include "kid_app/activity_list.h"
#include "kid_app/care_calendar.h"
#include "kid_app/care_phrases.h"
#include "kid_app/care_store.h"
#include "kid_app/care_view.h"
#include "kid_app/gesture_detector.h"
#include "kid_app/mini_game.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace kid_app {

// V8 care activities (docs/V8_CARE_AND_GROWTH.md). One small dial / motion
// session at a time, pure logic, single owner (the board's pet_input task),
// uint32 ms clock, wrap-safe. Sessions never write anything themselves:
// a finished activity hands back ONE CareEvent for the ledger, with the
// source the guard expects (dial choice for food and clothes, Device for
// the motion rules). Shaking hard is never rewarded, a resting board never
// counts as a shake, and nothing is lost by stopping early.

constexpr uint8_t kAlbumPerPage = 6;
constexpr uint8_t kAlbumPages = (kStickerCount + kAlbumPerPage - 1) / kAlbumPerPage;

uint16_t FoodArt(Food food);
uint16_t WardrobeArt(size_t wardrobe_index);
uint16_t HabitArt(Habit habit);

// ---- V9 C1: care actions that differ by day (pure, deterministic) ----
// Seed of the day's variation: civil day and temperament only (never a
// random source, never the network). Nonzero for any day.
uint32_t CareSeed(uint32_t day, Temperament t);
// Which of the three reaction lines plays today: 1..3 rotating with the
// day; 0 without a date (the shared V8 line).
uint8_t CheerArg(uint32_t day);
// Mud spots of a bath by the dirt need level: 2, 2, 3, 5.
uint8_t BathSpots(uint8_t dirty_level);
// `count` distinct positions (bits) out of kCareSpotSlots, from the seed.
uint8_t SpotMask(uint32_t seed, uint8_t count);
// Knots in the fur today: 2..4 (one back-and-forth of the dial each).
uint8_t BrushKnots(uint32_t seed);
// Where the itch is in tickle round 0..2: a permutation of head / belly /
// feet, so every spot comes up once a day.
uint8_t TickleTarget(uint32_t seed, uint8_t round);
// The most ticklish spot of a temperament (fixed: a big laugh there).
uint8_t TicklishSpot(Temperament t);

// ---- V9 C4: seasonal foods and today's wish ----
// What TuanTuan would love to eat today: one food from the menu of the
// day (the everyday six, plus the seasonal ones in season), chosen from
// the day and the temperament only. None without a date.
Food TodayWish(uint32_t day, Temperament t, const Season& season);
// CareView::variant of the feed menu and the diary: the treat's picture.
constexpr uint16_t TreatVariant(Treat t) { return static_cast<uint16_t>(t); }
// Food picture number (HomeLook::wish, CareView::food): 1..7 the food,
// 8 zongzi, 9 mooncake (food 8 drawn by its season); 0 = none.
uint8_t WishPicture(Food food, Treat treat);

// ---- V9 G5: today's game in the bedtime tidy-up ----
// The prop of a game (its menu card picture) by growth memory id
// (GAME_COMPLETED option: 1..5 V4, 21..39 V8); 0 for packs / unknown ids.
uint16_t GameToyArt(uint16_t game_id);

struct CareOutput {
    bool redraw = false;
    uint16_t clip = 0;       // care phrase id (1000..1999) or a game tone
    bool finished = false;   // back to the home screen (or on to the next step)
    bool record = false;     // `event` is ready for PetAtomicLedger::RecordCare
    CareEvent event{};       // id and minute are filled in by the board
    bool sleep = false;      // lullaby / diary done: hand over to the sleep flow
    CareKind next = CareKind::None;  // bedtime ritual: the step to start next
    bool song = false;  // V9 S4: the child chose the lullaby: play it, then sleep
};

// What a session may know about the pet when it starts.
struct CareContext {
    Temperament temperament = Temperament::Gentle;
    uint32_t stickers = 0;
    std::array<uint8_t, kWearSlots> worn{};
    Habit habit = Habit::None;
    bool habit_done_today = false;
    bool clock_known = true;  // false: no habit tick (the ledger needs a date)
    DiaryPage diary{};
    // V9 (all optional: the defaults give the V8 sessions).
    uint32_t day = 0;        // civil day of today's variation; 0 = classic V8 flow
    uint8_t dirty = 0;       // dirt need level 0..3 (how many mud spots)
    uint16_t toy_art = 0;    // today's game prop for the tidy-up, 0 = the bear
    bool lullaby = false;    // a lullaby to play (built in or on the card): the diary asks (S4)
    Season season{};         // seasonal foods on today's menu (C4)
    Food wish = Food::None;  // today's wish (TodayWish): three hearts (C4)
};

class CareSession {
public:
    static constexpr uint32_t kIdleExitMs = 60000;
    static constexpr uint32_t kReactMs = 2500;      // reaction picture before leaving
    static constexpr uint8_t kBathBubbles = 3;
    static constexpr uint8_t kBrushStrokes = 6;
    static constexpr uint8_t kTickles = 3;
    static constexpr uint8_t kLullStillWindows = 6;  // 6 x 500 ms resting
    static constexpr uint8_t kToys = 3;

    bool Start(CareKind kind, const CareContext& context, uint32_t now_ms, CareOutput* out);
    CareOutput Input(GameInput input, uint32_t now_ms);
    CareOutput OnGesture(Gesture gesture, uint32_t now_ms);
    // One IMU window (500 ms): `still` when the board lies at rest. Only the
    // lullaby listens; every other session ignores resting windows.
    CareOutput OnWindow(bool still, uint32_t now_ms);
    CareOutput Tick(uint32_t now_ms);
    // The ledger's answer to the habit tick this session asked for: the
    // check and the praise only follow a committed tick (refused: "already
    // ticked" and back home). Other sessions ignore it.
    CareOutput Confirm(bool committed, uint32_t now_ms);
    static constexpr uint32_t kConfirmMs = 3000;  // no answer: home, quietly
    void Interrupt() { active_ = false; }

    bool Active() const { return active_; }
    CareKind Kind() const { return kind_; }
    // Motion sessions want gestures (and lull resting windows).
    bool WantsMotion() const {
        return active_ && (kind_ == CareKind::Bath || kind_ == CareKind::Tickle ||
                           kind_ == CareKind::Lull);
    }
    CareView View() const;

private:
    CareOutput Finish(uint16_t clip, uint32_t now_ms);
    CareOutput Record(CareEventKind kind, MemorySource source, uint16_t slot, uint16_t option);
    uint8_t CardCount() const;
    CareOutput AfterDiary(uint32_t now_ms);
    uint16_t CardArt(uint8_t i) const;
    void DressList();

    CareKind kind_ = CareKind::None;
    CareContext ctx_{};
    bool active_ = false;
    bool reacting_ = false;
    uint32_t react_until_ms_ = 0;
    uint32_t last_input_ms_ = 0;
    uint8_t index_ = 0;
    uint8_t progress_ = 0;
    uint8_t phase_ = 0;
    bool awaiting_ = false;  // habit tick posted, ledger answer pending
    uint32_t awaiting_since_ms_ = 0;
    uint8_t hearts_ = 0;
    int8_t last_dir_ = 0;
    uint8_t page_ = 0;
    // Feed: the foods of today's menu, seasonal ones first (V9 C4).
    std::array<uint8_t, kAllFoodCount> menu_{};
    uint8_t menu_count_ = 0;
    // Dress: unlocked wardrobe indexes, in table order.
    std::array<uint8_t, 16> dress_{};
    uint8_t dress_count_ = 0;
    CareKind after_ = CareKind::None;  // tidy -> diary
    // V9 C1: today's variation (see care_view.h CareView::variant).
    uint32_t seed_ = 0;
    uint8_t goal_ = 0;
    uint16_t variant_ = 0;
    uint8_t feather_ = 0;
    bool laugh_ = false;
};

}  // namespace kid_app
