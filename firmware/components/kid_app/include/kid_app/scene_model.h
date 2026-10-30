#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace kid_app {

// Hardware-agnostic scene state. No Xiaozhi, LVGL, or ESP-IDF headers here.
class StoryPackReader;
enum class SceneKind : uint8_t { Home, Reader };
enum class Emotion : uint8_t { Neutral, Happy, Curious, Sleepy, Surprised };

struct SceneModel {
    SceneKind kind = SceneKind::Home;
    Emotion emotion = Emotion::Neutral;
    uint16_t book_page = 1;
};

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
    static void Render(const SceneModel& model, MonoFrame& output);
};

}  // namespace kid_app
