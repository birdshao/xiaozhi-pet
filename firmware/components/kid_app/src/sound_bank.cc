#include "kid_app/sound_bank.h"

#include "kid_app/care_phrases.h"
#include "kid_app/game_rules.h"
#include "kid_app/home_menu.h"

#include <algorithm>
#include <cmath>

namespace kid_app {
namespace {
struct Note {
    uint16_t hz;
    uint16_t ms;
};
struct Motif {
    const Note* notes;
    size_t count;
};

// Distinct from the option melodies of tools/make_prompt_audio.py.
constexpr Note kGreeting[] = {{659, 100}, {880, 100}, {1047, 180}};
constexpr Note kMorning[] = {{523, 120}, {659, 120}, {784, 120}, {1047, 260}};
constexpr Note kFavorite[] = {{784, 120}, {988, 200}};
constexpr Note kFeed[] = {{784, 90}, {659, 90}, {784, 90}, {1047, 200}};
constexpr Note kStory[] = {{659, 140}, {784, 140}, {659, 200}};
constexpr Note kBedtime[] = {{523, 220}, {440, 220}, {392, 360}};
constexpr Note kGiggle[] = {{1047, 60}, {1175, 60}, {1047, 60}, {1175, 60}, {1319, 120}};
constexpr Note kHotTip[] = {{880, 100}, {784, 100}, {659, 200}};  // falling: cool down
constexpr Note kColdTip[] = {{523, 100}, {659, 100}, {784, 200}};  // rising: warm up

template <size_t N>
constexpr Motif M(const Note (&notes)[N]) {
    return {notes, N};
}
// PhraseContext order.
constexpr Motif kMotifs[] = {M(kGreeting), M(kMorning), M(kFavorite), M(kFeed),
                             M(kStory),    M(kBedtime), M(kGiggle),
                             M(kHotTip),   M(kColdTip)};
static_assert(sizeof(kMotifs) / sizeof(kMotifs[0]) == kPhraseContextCount,
              "one chime per phrase context");

constexpr size_t kRate = WavClip::kSampleRate;
constexpr size_t kGapSamples = kRate * 30 / 1000;  // between notes
constexpr float kPi = 3.14159265f;

size_t NoteSamples(const Note& note) { return kRate * note.ms / 1000; }

const Motif* MotifFor(PhraseContext context) {
    const size_t c = static_cast<size_t>(context);
    return c < kPhraseContextCount ? &kMotifs[c] : nullptr;
}

int16_t ToneSample(const Note& note, size_t i) {
    const size_t n = NoteSamples(note);
    const float t = static_cast<float>(i) / kRate;
    // 8 ms attack, 40 ms release: no clicks at note edges.
    const float env = std::min({1.0f, static_cast<float>(i) / (0.008f * kRate),
                                static_cast<float>(n - i) / (0.040f * kRate)});
    const float w = 2.0f * kPi * note.hz * t;
    const float s = std::sin(w) + 0.25f * std::sin(2.0f * w);
    return static_cast<int16_t>(0.2f * 32767.0f * env * s / 1.25f);
}
}  // namespace

size_t ChimeSamples(PhraseContext context) {
    const Motif* m = MotifFor(context);
    if (!m) return 0;
    size_t total = 0;
    for (size_t i = 0; i < m->count; ++i) total += NoteSamples(m->notes[i]) + kGapSamples;
    return total;
}

size_t RenderChime(PhraseContext context, size_t offset, int16_t* out, size_t max) {
    const Motif* m = MotifFor(context);
    const size_t total = ChimeSamples(context);
    if (!m || !out || offset >= total) return 0;
    const size_t n = std::min(max, total - offset);
    for (size_t k = 0; k < n; ++k) {
        size_t pos = offset + k;
        int16_t value = 0;
        for (size_t j = 0; j < m->count; ++j) {
            const size_t len = NoteSamples(m->notes[j]);
            if (pos < len) {
                value = ToneSample(m->notes[j], pos);
                break;
            }
            pos -= len;
            if (pos < kGapSamples) break;  // silence between notes
            pos -= kGapSamples;
        }
        out[k] = value;
    }
    return n;
}

const char* SoundSourceName(SoundClip::Source source) {
    switch (source) {
        case SoundClip::Source::Pack: return "pack";
        case SoundClip::Source::Embedded: return "embedded";
        case SoundClip::Source::Chime: return "chime";
        case SoundClip::Source::None: break;
    }
    return "none";
}

size_t SoundClip::Copy(size_t offset, int16_t* out, size_t max) {
    if (!out || offset >= samples) return 0;
    const size_t n = std::min(max, samples - offset);
    switch (source) {
        case Source::Embedded:
            return wav_.Copy(offset, out, n);
        case Source::Chime:
            return RenderChime(chime_, offset, out, n);
        case Source::Pack:
            // KPS1 stores little-endian PCM16, the ESP32-S3's own order.
            return pack_ && pack_->ReadPcm(info_, static_cast<uint32_t>(offset * 2), out, n * 2)
                       ? n
                       : 0;
        case Source::None:
            break;
    }
    return 0;
}

bool SoundBank::FromPack(uint16_t id, SoundClip* out) const {
    SoundClipInfo info{};
    if (!pack_ || !pack_->Find(id, &info)) return false;
    *out = SoundClip{};
    out->source = SoundClip::Source::Pack;
    out->samples = info.Samples();
    out->pack_ = pack_;
    out->info_ = info;
    return true;
}

bool SoundBank::LoadOption(uint8_t index, SoundClip* out) const {
    if (!out || index >= HomeMenu::kItemCount) return false;
    if (FromPack(OptionClipId(index), out)) return true;
    *out = SoundClip{};
    if (!embedded_ || !embedded_(index, &out->wav_) || !out->wav_.samples) return false;
    out->source = SoundClip::Source::Embedded;
    out->samples = out->wav_.samples;
    return true;
}

bool SoundBank::LoadPhrase(uint16_t phrase_id, SoundClip* out) const {
    if (!out) return false;
    const PhraseTemplate* phrase = Personality::FindPhrase(phrase_id);
    if (!phrase) return false;
    if (FromPack(phrase_id, out)) return true;
    *out = SoundClip{};
    out->source = SoundClip::Source::Chime;
    out->chime_ = phrase->context;
    out->samples = ChimeSamples(phrase->context);
    return true;
}

bool SoundBank::LoadGameRule(uint16_t game_id, SoundClip* out) const {
    const uint16_t id = GameRuleKpsId(game_id);
    return out && id != 0 && FromPack(id, out);
}

bool SoundBank::LoadCareLine(uint16_t care_id, SoundClip* out) const {
    if (!out) return false;
    const CarePhrase* line = FindCarePhrase(care_id);
    if (!line) return false;
    if (FromPack(care_id, out)) return true;
    // No recording yet (TODO in the V8 asset list): a short chime of the
    // pet context closest to the line, so nothing waits on recordings.
    PhraseContext context = PhraseContext::Greeting;
    switch (line->line) {
        case CareLine::DiaryOpen: case CareLine::DiaryFood: case CareLine::DiaryBook:
        case CareLine::DiaryGame: case CareLine::DiaryCare: case CareLine::DiaryHabit:
        case CareLine::DiaryQuiet: case CareLine::DiaryClose: case CareLine::LullStart:
        case CareLine::LullDone: case CareLine::TidyStart: case CareLine::TidyDone:
            context = PhraseContext::Bedtime;
            break;
        case CareLine::FoodReact: case CareLine::FeedPrompt:
            context = PhraseContext::Feed;
            break;
        case CareLine::TickleStart: case CareLine::TickleDone: case CareLine::BathStart:
        case CareLine::BathRinse: case CareLine::BathDone:
            context = PhraseContext::Giggle;
            break;
        case CareLine::MorningFeel:
            context = line->arg == static_cast<uint8_t>(Feel::Hot)    ? PhraseContext::HotTip
                      : line->arg == static_cast<uint8_t>(Feel::Cold) ? PhraseContext::ColdTip
                                                                       : PhraseContext::MorningRecap;
            break;
        case CareLine::MorningHello: case CareLine::FestivalHello: case CareLine::HabitRemind:
            context = PhraseContext::MorningRecap;
            break;
        default:
            break;
    }
    *out = SoundClip{};
    out->source = SoundClip::Source::Chime;
    out->chime_ = context;
    out->samples = ChimeSamples(context);
    return true;
}

}  // namespace kid_app
