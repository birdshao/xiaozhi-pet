#pragma once

#include "kid_app/personality.h"
#include "kid_app/sound_pack_reader.h"
#include "kid_app/wav_clip.h"

#include <cstddef>
#include <cstdint>

namespace kid_app {

// Clip ids shared by tools/sound_pack.py, the TF sound pack and firmware:
// home item i -> 1 + i (HomeItem order); pet lines use their Personality
// phrase id (100..999).
constexpr uint16_t kOptionClipBase = 1;
constexpr uint16_t OptionClipId(uint8_t index) {
    return static_cast<uint16_t>(kOptionClipBase + index);
}

// One clip to stream: same Copy() contract as WavClip, whatever its source.
class SoundClip {
public:
    enum class Source : uint8_t { None, Pack, Embedded, Chime };

    Source source = Source::None;
    size_t samples = 0;
    // Copies up to max samples from offset; 0 at the end or when the TF
    // card can no longer be read.
    size_t Copy(size_t offset, int16_t* out, size_t max);

private:
    friend class SoundBank;
    WavClip wav_{};
    SoundPackReader* pack_ = nullptr;
    SoundClipInfo info_{};
    PhraseContext chime_ = PhraseContext::Greeting;
};

const char* SoundSourceName(SoundClip::Source source);

// Built-in placeholder for a pet line while no recording exists: a short,
// quiet, deterministic motif per context (generated, nothing in flash).
size_t ChimeSamples(PhraseContext context);
size_t RenderChime(PhraseContext context, size_t offset, int16_t* out, size_t max);

// V2-08: where each clip comes from. A verified TF pack wins clip by clip;
// otherwise option names use the clips embedded in the app image and pet
// lines a chime. Single owner (the board's pet_prompt task).
class SoundBank {
public:
    using EmbeddedLoader = bool (*)(uint8_t index, WavClip* clip);

    explicit SoundBank(EmbeddedLoader embedded = nullptr) : embedded_(embedded) {}
    // nullptr or a pack that is not Ready(): built-in sounds only.
    void UsePack(SoundPackReader* pack) { pack_ = pack && pack->Ready() ? pack : nullptr; }
    bool HasPack() const { return pack_ != nullptr; }

    bool LoadOption(uint8_t index, SoundClip* out) const;
    bool LoadPhrase(uint16_t phrase_id, SoundClip* out) const;
    // V8 care line (care_phrases.h, ids 1000..1999): the pack clip, else the
    // chime of the nearest pet context. False for an unknown id.
    bool LoadCareLine(uint16_t care_id, SoundClip* out) const;

private:
    bool FromPack(uint16_t id, SoundClip* out) const;

    EmbeddedLoader embedded_ = nullptr;
    SoundPackReader* pack_ = nullptr;
};

}  // namespace kid_app
