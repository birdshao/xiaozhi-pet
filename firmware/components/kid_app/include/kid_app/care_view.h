#pragma once

#include <array>
#include <cstdint>

namespace kid_app {

// V8 picture state, kept free of the care rules so the scene composer only
// needs plain numbers (see care_session.h / care_store.h for the meaning).

enum class CareKind : uint8_t {
    None, Feed, Bath, Brush, Tickle, Lull, Dress, Habit, Tidy, Diary, Album,
    Bedsong,  // V9 S4: after the diary, "a lullaby?" (two cards)
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
    // V9 C1: what changes from day to day (0 = the classic V8 picture).
    // Bath: mask of the mud spots (bit per kCareSpotSlots position; the
    // first `progress` set bits are scrubbed off). Brush: mask of the knots
    // (progress / 2 of them combed out). Tickle: see Tickle*Of below.
    uint16_t variant = 0;
    // V9 G5: the third toy of the tidy-up (CareArt::ToyBear, or the prop of
    // today's game: any picture GameArtIcon draws).
    uint16_t toy = 0;

    bool RailDiffers(const CareView& o) const {
        return option_count != o.option_count || highlight != o.highlight || options != o.options;
    }
    bool TopDiffers(const CareView& o) const {
        return kind != o.kind || progress != o.progress || goal != o.goal || phase != o.phase ||
               hearts != o.hearts || worn != o.worn || page != o.page || pages != o.pages ||
               stickers != o.stickers || food != o.food || book != o.book || game != o.game ||
               habit_done != o.habit_done || cares != o.cares || variant != o.variant ||
               toy != o.toy;
    }
    bool operator==(const CareView& o) const { return !RailDiffers(o) && !TopDiffers(o); }
    bool operator!=(const CareView& o) const { return !(*this == o); }
};

// V9 C1 positions. Bath mud spots and brush knots sit on 8 fixed places
// of the care picture's pet; tickle spots are head (0), belly (1), feet (2).
constexpr uint8_t kCareSpotSlots = 8;
// V9 C4 food pictures: 1..7 the Food, 8 zongzi, 9 mooncake.
constexpr uint8_t kWishPictures = 9;
// CareView::variant of the feed menu at Mid-Autumn (care_session.h
// TreatVariant(Treat::Mooncake)): food 8's card is drawn as a mooncake.
constexpr uint16_t kFeedVariantMooncake = 2;
constexpr uint8_t kTickleHead = 0, kTickleBelly = 1, kTickleFeet = 2;
constexpr uint16_t TickleVariant(uint8_t target, uint8_t feather, bool laugh) {
    return static_cast<uint16_t>(0x20 | (target & 3) | ((feather & 3) << 2) | (laugh ? 0x10 : 0));
}
constexpr uint8_t TickleTargetOf(uint16_t v) { return static_cast<uint8_t>(v & 3); }
constexpr uint8_t TickleFeatherOf(uint16_t v) { return static_cast<uint8_t>((v >> 2) & 3); }
constexpr bool TickleLaughOf(uint16_t v) { return (v & 0x10) != 0; }

// What the home picture shows of the V8 care state (Home scenes only; not
// part of PackScene, like the status band). Item numbers are the
// WardrobeItem::item of each WearSlot (0 = none); need is the Need value of
// CareStore::TopNeed with its level (>= 2 only); festival a Festival value.
struct HomeLook {
    std::array<uint8_t, 5> worn{};  // Head, Neck, Face, Bed, Lamp
    uint8_t need = 0;
    uint8_t need_level = 0;
    uint8_t festival = 0;
    uint8_t stage = 0;  // V9 C3 growth stage 0..3 (growth_stage.h)
    uint8_t wish = 0;   // V9 C4 today's wish, a food picture (care_session.h WishPicture)
    bool operator==(const HomeLook& o) const {
        return worn == o.worn && need == o.need && need_level == o.need_level &&
               festival == o.festival && stage == o.stage && wish == o.wish;
    }
    bool operator!=(const HomeLook& o) const { return !(*this == o); }
};
// One word for board atomics (RAM only: never stored, never sent).
// Bit allocation, registered with the V9 coordinator (slice 1C):
//    0..14  worn: 3 bits per WearSlot (V9; was 4) -> at most 7 items a slot
//   15..18  wish: today's wish food picture 0..9 (V9 C4)
//   19      free
//   20..23  need    24..25 need level    26..29 festival
//   30..31  stage: growth stage 0..3 (V9 C3)
// Out-of-range values are dropped to 0, never smeared into a neighbour.
uint32_t PackLook(const HomeLook& look);
HomeLook UnpackLook(uint32_t bits);

}  // namespace kid_app
