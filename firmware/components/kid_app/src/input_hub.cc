#include "kid_app/input_hub.h"

namespace kid_app {

namespace {
constexpr std::array<InputKey, 2> kRepeatKeys = {InputKey::DialUp, InputKey::DialDown};

// Unsigned subtraction keeps durations correct across uint32 wrap-around.
uint32_t Elapsed(uint32_t now_ms, uint32_t since_ms) { return now_ms - since_ms; }
bool Reached(uint32_t now_ms, uint32_t deadline_ms) {
    return static_cast<int32_t>(now_ms - deadline_ms) >= 0;
}
}  // namespace

bool InputHub::ComboHeld() const {
    return keys_[static_cast<size_t>(InputKey::Boot)].down &&
           keys_[static_cast<size_t>(InputKey::DialPress)].down;
}

void InputHub::Press(InputKey key, uint32_t now_ms) {
    KeyState& state = State(key);
    if (state.down) return;
    state = {true, false, now_ms, now_ms + config_.repeat_delay_ms};

    if (ComboHeld()) {
        if (key == InputKey::Boot || key == InputKey::DialPress) {
            // Both combo keys held: neither may produce its own action later.
            State(InputKey::Boot).consumed = true;
            State(InputKey::DialPress).consumed = true;
            combo_armed_ = true;
            combo_since_ms_ = now_ms;
        } else {
            // Other keys during a combo are swallowed; the timer keeps
            // running and a fired combo is not re-armed.
            state.consumed = true;
        }
        return;
    }
    if (key == InputKey::DialUp) Emit(InputKind::DialUp, now_ms);
    if (key == InputKey::DialDown) Emit(InputKind::DialDown, now_ms);
}

void InputHub::Release(InputKey key, uint32_t now_ms) {
    KeyState& state = State(key);
    if (!state.down) return;
    const bool consumed = state.consumed;
    state.down = false;
    if (key == InputKey::Boot || key == InputKey::DialPress) combo_armed_ = false;
    if (consumed) return;

    // A starved owner task may see press and release before any Tick;
    // the real hold duration still decides short versus long.
    const bool held_long = Elapsed(now_ms, state.since_ms) >= config_.long_ms;
    switch (key) {
        case InputKey::DialPress:
            Emit(held_long ? InputKind::DialLong : InputKind::DialPress, now_ms);
            break;
        case InputKey::Boot:
            Emit(held_long ? InputKind::BootLong : InputKind::Talk, now_ms);
            break;
        case InputKey::Power: Emit(InputKind::SleepRequest, now_ms); break;
        case InputKey::DialUp:
        case InputKey::DialDown: break;  // already emitted on press-down
    }
}

void InputHub::Tick(uint32_t now_ms) {
    if (combo_armed_) {
        if (Elapsed(now_ms, combo_since_ms_) >= config_.parent_combo_ms) {
            combo_armed_ = false;
            Emit(InputKind::ParentCombo, now_ms);
        }
        return;  // no long-press or repeat while combining
    }

    KeyState& press = State(InputKey::DialPress);
    if (press.down && !press.consumed &&
        Elapsed(now_ms, press.since_ms) >= config_.long_ms) {
        press.consumed = true;
        Emit(InputKind::DialLong, now_ms);
    }
    KeyState& boot = State(InputKey::Boot);
    if (boot.down && !boot.consumed &&
        Elapsed(now_ms, boot.since_ms) >= config_.long_ms) {
        boot.consumed = true;
        Emit(InputKind::BootLong, now_ms);
    }

    if (!auto_repeat_) return;
    for (InputKey key : kRepeatKeys) {
        KeyState& state = State(key);
        if (!state.down || state.consumed || !Reached(now_ms, state.next_repeat_ms))
            continue;
        // Schedule from "now" so a late tick never bursts several repeats.
        state.next_repeat_ms = now_ms + config_.repeat_interval_ms;
        Emit(key == InputKey::DialUp ? InputKind::DialUp : InputKind::DialDown,
             now_ms, true);
    }
}

void InputHub::Emit(InputKind kind, uint32_t at_ms, bool repeat) {
    if (voice_active_ && kind != InputKind::DialLong && kind != InputKind::Talk &&
        kind != InputKind::SleepRequest && kind != InputKind::ParentCombo) {
        return;
    }
    if (count_ == kCapacity) {
        ++dropped_;
        return;
    }
    queue_[(head_ + count_) % kCapacity] = {kind, at_ms, repeat};
    ++count_;
}

bool InputHub::Next(InputEvent* out) {
    if (!out || count_ == 0) return false;
    *out = queue_[head_];
    head_ = (head_ + 1) % kCapacity;
    --count_;
    return true;
}

}  // namespace kid_app
