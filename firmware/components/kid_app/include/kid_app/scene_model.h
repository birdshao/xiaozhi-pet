#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "kid_app/care_view.h"
#include "kid_app/game_view.h"
#include "kid_app/parent_menu.h"
#include "kid_app/status_bar.h"

namespace kid_app {

// Hardware-agnostic scene state. No Xiaozhi, LVGL, or ESP-IDF headers here.
class StoryPackReader;
// Sleep: shown before PMIC power-off and kept by the ePaper while off.
// Parent: the V3-06 on-device parent menu (draws SceneModel::parent).
// Game: a V4-06 mini game round (draws SceneModel::game).
// Care: a V8 care activity, the bedtime diary or the sticker album (draws
// SceneModel::care). Owned by V8 session 1: other features use Game / Reader.
enum class SceneKind : uint8_t { Home, Reader, Sleep, Parent, Game, Care };
// Listening / Thinking / Speaking: voice-session faces (V6-01, see
// voice_face.h); like every expression they stay above the action rail.
// Packed in 4 bits (PackScene): at most 16 emotions.
enum class Emotion : uint8_t {
    Neutral, Happy, Curious, Sleepy, Surprised, Listening, Thinking, Speaking
};
// Short animation frames drawn on top of a scene (PetAnimationDirector).
// Still is the resting picture. Home: Blink (eyes closed), Wave (paw up,
// wink), Hearts1/Hearts2 (after feeding), Giggle (gentle shake: laughing
// eyes, wobble lines), Dizzy1/Dizzy2 (hard shake: spiral eyes, circling
// stars; a gentle reminder, never a reward). Sleep: Zzz1/Zzz2 show one/two
// of the three Z letters; Still shows all three (the picture kept while
// off). Home good-morning picture (V3-04): Sunrise (sun behind a cloud,
// TuanTuan stretching and yawning), Morning (sun up, TuanTuan waving).
// Battery (V3-05): Charging ("eating electricity": munching a lightning
// bolt held up in the left paw), Yawn (low battery: eyes shut, big yawn,
// a bubble with an almost empty battery: "find a grown-up to charge me").
// Poses only ever change pixels above the home action rail.
// Packed in 4 bits (PackScene): at most 16 poses.
enum class ScenePose : uint8_t {
    Still, Blink, Wave, Hearts1, Hearts2, Zzz1, Zzz2, Giggle, Dizzy1, Dizzy2,
    Sunrise, Morning, Charging, Yawn,
    TipHot, TipCold  // V4-03: room too hot / cold, life-tip bubble (last value 15)
};

struct SceneModel {
    SceneKind kind = SceneKind::Home;
    Emotion emotion = Emotion::Neutral;
    uint16_t book_page = 1;
    // Highlighted HomeItem index on the home action rail (UI-only state).
    uint8_t home_selection = 1;
    ScenePose pose = ScenePose::Still;
    // Top status band, drawn on Home only. Not part of PackScene: the board
    // keeps the readings in their own word (PackStatus) and the ePaper task
    // fills this in when it draws (see RefreshPlanner's status region).
    StatusBar status{};
    // Parent menu rows (SceneKind::Parent only). Not packed either: the
    // board never keeps a parent scene in its packed pet scene word.
    ParentView parent{};
    // Mini game round (SceneKind::Game only). Not packed either: the board
    // keeps the game in its own session, like the parent menu.
    GameView game{};
    // V8 care activity (SceneKind::Care only), not packed either.
    CareView care{};
    // V8 clothes, room, need bubble and festival prop on the home picture.
    // Not packed: the board keeps it in its own word (PackLook) and fills
    // it in when it draws a Home scene, like the status band.
    HomeLook look{};
};

// Why the reader has no book to show: no card mounted, the book file is not
// on the card, or it is there but does not validate. A Reader scene with
// book_page bits 0-7 = 0 and bits 8-15 = the reason draws the "no book"
// picture (icons only: card + slot arrow, or an empty book) instead of a
// page; BadBook looks like NoBook. Pages start at 1, so nothing collides.
enum class BookMissing : uint8_t { None, NoCard, NoBook, BadBook };
SceneModel BookMissingScene(BookMissing reason);
BookMissing BookMissingOf(const SceneModel& scene);

// One-word scene snapshot shared between board tasks (atomic<uint32_t>):
// bits 0-3 kind, 4-7 home selection, 8-11 emotion, 12-15 pose,
// 16-31 book page. The status band is not packed (UnpackScene leaves it
// unknown).
uint32_t PackScene(const SceneModel& scene);
SceneModel UnpackScene(uint32_t bits);

class MonoFrame {
public:
    static constexpr int kWidth = 800;
    static constexpr int kHeight = 480;
    static constexpr int kStride = kWidth / 8;
    static constexpr size_t kSize = kStride * kHeight;

    // Physical-driver convention: bit=1 white, bit=0 black.
    MonoFrame() { Clear(); }
    void Clear();
    void SetPixel(int x, int y, bool black);
    // Row y, x0..x1 inclusive (clipped): paint the pixels whose bit is set
    // in the repeating 8-pixel mask (0x80 = x % 8 == 0). Byte-wise, so a
    // scene costs a few ms on the ESP32-S3 instead of per-pixel PSRAM writes.
    void PaintSpan(int x0, int x1, int y, uint8_t mask, bool black);
    bool IsBlack(int x, int y) const;
    const std::array<uint8_t, kSize>& Data() const { return pixels_; }

private:
    // Allow validated SD book pages to load directly into the existing
    // 48KiB PSRAM-backed MonoFrame without allocating a second framebuffer.
    friend class StoryPackReader;
    std::array<uint8_t, kSize> pixels_{};
};

class SceneComposer {
public:
    // Home layout: pet portrait above, five-slot action rail below.
    static constexpr int kRailTop = 380;
    static constexpr int kRailSlotWidth = 160;
    // Status band (V2-05): two corner boxes on rows 0..kStatusBottom-1,
    // left x < kStatusLeftEnd (clock, quiet moon, Wi-Fi) and right
    // x >= kStatusRightStart (temperature, charging bolt, battery). The
    // pet's ears and pose ornaments use the gap between them; no other
    // home picture ever draws inside the boxes.
    static constexpr int kStatusBottom = 40;
    static constexpr int kStatusLeftEnd = 248;
    static constexpr int kStatusRightStart = 632;
    static bool InStatusBox(int x, int y) {
        return y >= 0 && y < kStatusBottom && (x < kStatusLeftEnd || x >= kStatusRightStart);
    }

    // Game scene (V4-06): question area above, answer cards below.
    static constexpr int kGameRailTop = 296;

    static void Render(const SceneModel& model, MonoFrame& output);
};

}  // namespace kid_app
