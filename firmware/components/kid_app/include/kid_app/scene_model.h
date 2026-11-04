#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "kid_app/status_bar.h"

namespace kid_app {

// Hardware-agnostic scene state. No Xiaozhi, LVGL, or ESP-IDF headers here.
class StoryPackReader;
// Sleep: shown before PMIC power-off and kept by the ePaper while off.
enum class SceneKind : uint8_t { Home, Reader, Sleep };
enum class Emotion : uint8_t { Neutral, Happy, Curious, Sleepy, Surprised };
// Short animation frames drawn on top of a scene (PetAnimationDirector).
// Still is the resting picture. Home: Blink (eyes closed), Wave (paw up,
// wink), Hearts1/Hearts2 (after feeding), Giggle (gentle shake: laughing
// eyes, wobble lines), Dizzy1/Dizzy2 (hard shake: spiral eyes, circling
// stars; a gentle reminder, never a reward). Sleep: Zzz1/Zzz2 show one/two
// of the three Z letters; Still shows all three (the picture kept while
// off). Poses only ever change pixels above the home action rail.
// Packed in 4 bits (PackScene): at most 16 poses.
enum class ScenePose : uint8_t {
    Still, Blink, Wave, Hearts1, Hearts2, Zzz1, Zzz2, Giggle, Dizzy1, Dizzy2
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
};

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

    static void Render(const SceneModel& model, MonoFrame& output);
};

}  // namespace kid_app
