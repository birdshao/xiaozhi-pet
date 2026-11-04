#pragma once

#include "kid_app/content_pack_reader.h"

#include <cstdint>

namespace kid_app {

class MonoFrame;

// "Choose one of N icons" with the three-way dial (PRD §4): up/down move the
// highlight, press chooses. Clamped at both ends like the home rail, so a
// push past the last icon does nothing (no refresh, no sound). UI-only
// state: no NVS, no audio, no memory. Used by the picture-book questions
// (V4-05); games can reuse it with their own icon boxes.
class IconChoice {
public:
    static constexpr uint8_t kMaxOptions = 4;

    // count 0 = nothing to choose (Move always fails).
    void Reset(uint8_t count, uint8_t start = 0);
    // delta < 0: previous icon (dial up), > 0: next (dial down). False when
    // the highlight did not move.
    bool Move(int delta);
    uint8_t Index() const { return index_; }
    uint8_t Count() const { return count_; }

private:
    uint8_t count_ = 0;
    uint8_t index_ = 0;
};

// How an icon's 8 px margin (HighlightRect minus the icon box) is drawn.
// Plain: white, so every icon sits on a clean card over a busy picture.
// Highlight: black outer ring (4 px) = "this one?". Chosen: the whole
// margin black = "this one!" (the answer, during the reaction).
enum class ChoiceRing : uint8_t { Plain, Highlight, Chosen };

constexpr uint8_t kChoiceRingPx = 4;

// ring is the full highlight rectangle; the inner `margin` px border is
// painted, the icon area inside it is never touched. Clipped to the frame.
void DrawChoiceRing(MonoFrame& frame, const PageRect& ring, uint8_t margin,
                    ChoiceRing style);

// Sets every pixel of rect in `previous` to the opposite of `next`. Used to
// build the "previous image" of a forced 1-bit partial refresh: every pixel
// in rect then differs from the new image, so the partial waveform drives
// all of them (also where the panel shows grey). Clipped to the frame.
void OpposeRect(MonoFrame& previous, const MonoFrame& next, const PageRect& rect);

}  // namespace kid_app
