#pragma once

#include <cstddef>
#include <cstdint>

namespace kid_app {

// Home action rail (PRD v0.2 §5.1), left to right.
enum class HomeItem : uint8_t { Chat, Story, Game, Feed, Sleep };

// Volatile UI selection: moving the highlight is not a Pet event and is
// never written to NVS. Single owner (the board input task).
class HomeMenu {
public:
    static constexpr size_t kItemCount = 5;
    static constexpr uint8_t kDefaultIndex = 1;  // Story

    HomeItem Selected() const { return static_cast<HomeItem>(index_); }
    uint8_t Index() const { return index_; }
    // Clamped at both ends; returns true when the highlight moved.
    bool Move(int delta);
    bool Select(uint8_t index);
    void Reset() { index_ = kDefaultIndex; }

private:
    uint8_t index_ = kDefaultIndex;
};

}  // namespace kid_app
