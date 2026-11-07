#pragma once

#include <array>
#include <cstdint>

namespace kid_app {

struct PromptRequest {
    uint8_t index = 0;   // HomeItem index to speak
    uint32_t token = 0;  // increasing; a newer token supersedes older audio
    uint8_t phrase = 0;  // V5-04: 1 + PhraseContext = a pet line, 0 = option name
    uint16_t game = 0;   // V4-06: a mini-game clip id (mini_game.h), 0 = none
    // V8: reviewed care lines (care_phrases.h ids) played in order after
    // whatever else this request plays (or alone). The player re-posts the
    // rest after each line, so any newer request still interrupts them.
    static constexpr uint8_t kCareLines = 8;
    uint8_t care_count = 0;
    std::array<uint16_t, kCareLines> care{};
    bool care_next = false;  // a continuation: do not cut the line before it
};

// Option name prompts on the home rail (V2-07): speak the item a child
// stops on, not every item passed while dialling. Single owner (board
// input task); uint32 ms clock, wrap-safe.
class PromptScheduler {
public:
    static constexpr uint32_t kSettleMs = 350;

    void OnSelect(uint8_t index, uint32_t now_ms);
    // Drop a pending prompt (press, leaving home, sleep, voice).
    void Cancel() { pending_ = false; }
    // Disabled outside the home rail / in quiet or voice; drops pending.
    void SetEnabled(bool enabled);
    bool Tick(uint32_t now_ms, PromptRequest* out);

private:
    bool enabled_ = true;
    bool pending_ = false;
    uint8_t index_ = 0;
    uint32_t due_ms_ = 0;
    uint32_t token_ = 0;
};

}  // namespace kid_app
