#pragma once

#include <array>
#include <cstdint>

namespace kid_app {

// V8 picture state, kept free of the care rules so the scene composer only
// needs plain numbers (see care_session.h / care_store.h for the meaning).

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
    std::array<uint8_t, 5> worn{};  // item per WearSlot (dress preview)
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

// What the home picture shows of the V8 care state (Home scenes only; not
// part of PackScene, like the status band). Item numbers are the
// WardrobeItem::item of each WearSlot (0 = none); need is the Need value of
// CareStore::TopNeed with its level (>= 2 only); festival a Festival value.
struct HomeLook {
    std::array<uint8_t, 5> worn{};  // Head, Neck, Face, Bed, Lamp
    uint8_t need = 0;
    uint8_t need_level = 0;
    uint8_t festival = 0;
    bool operator==(const HomeLook& o) const {
        return worn == o.worn && need == o.need && need_level == o.need_level &&
               festival == o.festival;
    }
    bool operator!=(const HomeLook& o) const { return !(*this == o); }
};
// One word for board atomics: 4 bits per worn slot (0..19), need 20..23,
// level 24..25, festival 26..29. Out-of-range values are dropped to 0.
uint32_t PackLook(const HomeLook& look);
HomeLook UnpackLook(uint32_t bits);

}  // namespace kid_app
