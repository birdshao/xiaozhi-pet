#pragma once

#include "kid_app/activity_list.h"
#include "kid_app/care_phrases.h"
#include "kid_app/care_store.h"
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

enum class CareKind : uint8_t {
    None, Feed, Bath, Brush, Tickle, Lull, Dress, Habit, Tidy, Diary, Album,
};
const char* CareKindName(CareKind kind);

// SceneKind::Care contents. Cards (feed, dress, habit) are drawn below
// SceneComposer::kGameRailTop like the game answer cards; the pet, the
// progress and the album page above it.
struct CareView {
    CareKind kind = CareKind::None;
    uint8_t option_count = 0;  // 0..3 cards
    uint8_t highlight = 0;
    std::array<uint16_t, 3> options{};  // CareArt ids
    uint8_t progress = 0;   // bubbles / strokes / giggles / still / toys put away
    uint8_t goal = 0;
    uint8_t phase = 0;      // bath: 1 = rinse; feed / dress / habit: 1 = reacting
    uint8_t hearts = 0;     // 1..3 after a care (taste for food)
    std::array<uint8_t, kWearSlots> worn{};  // the pet's look (dress preview)
    // Album: one page of kAlbumPerPage stickers.
    uint8_t page = 0, pages = 0;
    uint32_t stickers = 0;
    // Diary: what the page shows.
    uint8_t food = 0;   // Food
    bool book = false, game = false, habit_done = false;
    uint8_t cares = 0;  // CareBucket

    bool RailDiffers(const CareView& o) const {
        return option_count != o.option_count || highlight != o.highlight || options != o.options;
    }
    bool TopDiffers(const CareView& o) const {
        return kind != o.kind || progress != o.progress || goal != o.goal || phase != o.phase ||
               hearts != o.hearts || worn != o.worn || page != o.page || pages != o.pages ||
               stickers != o.stickers || food != o.food || book != o.book || game != o.game ||
               habit_done != o.habit_done || cares != o.cares;
    }
    bool operator==(const CareView& o) const { return !RailDiffers(o) && !TopDiffers(o); }
    bool operator!=(const CareView& o) const { return !(*this == o); }
};

constexpr uint8_t kAlbumPerPage = 6;
constexpr uint8_t kAlbumPages = (kStickerCount + kAlbumPerPage - 1) / kAlbumPerPage;

uint16_t FoodArt(Food food);
uint16_t WardrobeArt(size_t wardrobe_index);
uint16_t HabitArt(Habit habit);

struct CareOutput {
    bool redraw = false;
    uint16_t clip = 0;       // care phrase id (1000..1999) or a game tone
    bool finished = false;   // back to the home screen (or on to the next step)
    bool record = false;     // `event` is ready for PetAtomicLedger::RecordCare
    CareEvent event{};       // id and minute are filled in by the board
    bool sleep = false;      // lullaby / diary done: hand over to the sleep flow
    CareKind next = CareKind::None;  // bedtime ritual: the step to start next
};

// What a session may know about the pet when it starts.
struct CareContext {
    Temperament temperament = Temperament::Gentle;
    uint32_t stickers = 0;
    std::array<uint8_t, kWearSlots> worn{};
    Habit habit = Habit::None;
    bool habit_done_today = false;
    DiaryPage diary{};
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
    uint8_t hearts_ = 0;
    int8_t last_dir_ = 0;
    uint8_t page_ = 0;
    // Dress: unlocked wardrobe indexes, in table order.
    std::array<uint8_t, 16> dress_{};
    uint8_t dress_count_ = 0;
    CareKind after_ = CareKind::None;  // tidy -> diary
};

}  // namespace kid_app
