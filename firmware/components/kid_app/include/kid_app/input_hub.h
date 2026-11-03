#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace kid_app {

// Physical keys on the Waveshare 3.97 board: three-way dial on GPIO4/5/6
// (active low), BOOT on GPIO0 and PWR_OUT on GPIO1. No GPIO access here.
enum class InputKey : uint8_t { DialUp, DialPress, DialDown, Boot, Power };
constexpr size_t kInputKeyCount = 5;

// Child-facing semantics shared by every scene (ADR-009).
enum class InputKind : uint8_t {
    DialUp,        // previous item / page (on press-down)
    DialDown,      // next item / page (on press-down)
    DialPress,     // confirm (short press, on release)
    DialLong,      // back home (held >= long_ms)
    Talk,          // BOOT short press: start/stop voice
    BootLong,      // BOOT held >= long_ms (board-level mode toggle)
    SleepRequest,  // PWR short press: cancellable sleep flow, never direct off
    ParentCombo,   // BOOT + dial press held >= parent_combo_ms; press the
                   // second key within long_ms or the first one's long fires
};

struct InputEvent {
    InputKind kind = InputKind::DialUp;
    uint32_t at_ms = 0;
    bool repeat = false;
};

struct InputHubConfig {
    uint32_t long_ms = 1000;
    uint32_t repeat_delay_ms = 500;
    uint32_t repeat_interval_ms = 350;  // <= 3 events per second
    uint32_t parent_combo_ms = 5000;
};

// Single-owner, hardware-agnostic key semantics. Button callbacks must
// forward raw edges to the owning task; never call this from an ISR.
// Times are a free-running uint32 millisecond clock (wrap-safe).
class InputHub {
public:
    static constexpr size_t kCapacity = 16;

    explicit InputHub(InputHubConfig config = {}) : config_(config) {}

    void Press(InputKey key, uint32_t now_ms);
    void Release(InputKey key, uint32_t now_ms);
    void Tick(uint32_t now_ms);

    // Enable dial auto-repeat only in list/page contexts.
    void SetAutoRepeat(bool enabled) { auto_repeat_ = enabled; }
    // During a voice session only DialLong, Talk, SleepRequest and
    // ParentCombo are delivered.
    void SetVoiceActive(bool active) { voice_active_ = active; }

    bool Next(InputEvent* out);
    size_t Pending() const { return count_; }
    uint32_t Dropped() const { return dropped_; }

private:
    struct KeyState {
        bool down = false;
        bool consumed = false;  // long/combo already handled this hold
        uint32_t since_ms = 0;
        uint32_t next_repeat_ms = 0;
    };

    KeyState& State(InputKey key) { return keys_[static_cast<size_t>(key)]; }
    bool ComboHeld() const;
    void Emit(InputKind kind, uint32_t at_ms, bool repeat = false);

    InputHubConfig config_;
    std::array<KeyState, kInputKeyCount> keys_{};
    bool auto_repeat_ = false;
    bool voice_active_ = false;
    bool combo_armed_ = false;
    uint32_t combo_since_ms_ = 0;

    std::array<InputEvent, kCapacity> queue_{};
    size_t head_ = 0;
    size_t count_ = 0;
    uint32_t dropped_ = 0;
};

}  // namespace kid_app
