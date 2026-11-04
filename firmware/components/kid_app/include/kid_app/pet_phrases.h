#pragma once

#include "kid_app/memory_store.h"
#include "kid_app/personality.h"

#include <array>
#include <cstdint>

namespace kid_app {

const char* PhraseContextName(PhraseContext context);

// A line that can be pre-recorded: a reviewed Personality template with no
// memory slot ({book}/{like} need speech synthesis, which the pet has not).
bool RecordablePhrase(uint16_t id);

// V5-04 (first half): which short reviewed line the pet says for an event.
// Only slot-free Personality templates, chosen with Personality's own
// weights; the seed is how often this context was spoken, so the same
// sequence of events gives the same lines per temperament (no randomness,
// no free text). Single owner (the board's pet_prompt task); uint32 ms
// clock, wrap-safe.
class PhrasePlanner {
public:
    // Lines never pile up: within this gap after a line the pet stays
    // quiet, except for good morning and good night.
    static constexpr uint32_t kMinGapMs = 4000;

    void SetTemperament(Temperament t) { temperament_ = t; }
    Temperament GetTemperament() const { return temperament_; }
    // Phrase id to say now, 0 = stay quiet.
    uint16_t Next(PhraseContext context, uint32_t now_ms);

private:
    Temperament temperament_ = Temperament::Gentle;
    std::array<uint32_t, kPhraseContextCount> said_{};
    bool spoke_ = false;
    uint32_t last_ms_ = 0;
};

}  // namespace kid_app
