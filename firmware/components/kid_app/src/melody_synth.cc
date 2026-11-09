#include "kid_app/melody_synth.h"

#include <cmath>

namespace kid_app {

namespace {
constexpr size_t kTableBits = 10;
constexpr size_t kTableSize = size_t{1} << kTableBits;
constexpr int64_t kPeak = 11500;      // ~35% of full scale at volume 100
constexpr uint32_t kAttackMs = 6;
constexpr uint32_t kReleaseMs = 25;   // fade at the note end: no clicks
constexpr uint32_t kGapMs = 12;       // silence that separates repeated notes

const int16_t* SineTable() {
    static int16_t table[kTableSize];
    static bool ready = false;
    if (!ready) {
        for (size_t i = 0; i < kTableSize; ++i)
            table[i] = static_cast<int16_t>(std::lround(
                32767.0 * std::sin(6.283185307179586 * static_cast<double>(i) / kTableSize)));
        ready = true;
    }
    return table;
}

int32_t Sine(uint32_t phase) { return SineTable()[phase >> (32 - kTableBits)]; }

uint32_t PhaseInc(double hz) {
    return static_cast<uint32_t>(hz * 4294967296.0 / kSynthSampleRate);
}

bool Valid(const Melody& m) {
    if (!m.notes || m.count == 0 || m.bpm < kMinBpm || m.bpm > kMaxBpm) return false;
    size_t ticks = 0;
    for (size_t i = 0; i < m.count; ++i) {
        const uint8_t p = m.notes[i].pitch;
        if (p != kRestPitch && (p < kMinPitch || p > kMaxPitch)) return false;
        ticks += m.notes[i].ticks;
    }
    return ticks > 0;
}

// Per-sample Q15 multiplier whose half-life is `ms`.
uint32_t DecayFor(uint32_t half_life_ms) {
    const double samples = half_life_ms * kSynthSampleRate / 1000.0;
    return static_cast<uint32_t>(std::lround(32768.0 * std::pow(0.5, 1.0 / samples)));
}
}  // namespace

uint32_t MelodySynth::NoteStart(size_t ticks) const {
    // Cumulative, so rounding never drifts across a long melody.
    const uint64_t num = static_cast<uint64_t>(ticks) * 60u * kSynthSampleRate;
    return static_cast<uint32_t>(num / (static_cast<uint64_t>(melody_.bpm) * kTicksPerBeat));
}

uint32_t MelodySynth::TotalSamples(const Melody& melody) {
    if (!Valid(melody)) return 0;
    MelodySynth s;
    s.melody_ = melody;
    size_t ticks = 0;
    for (size_t i = 0; i < melody.count; ++i) ticks += melody.notes[i].ticks;
    return s.NoteStart(ticks);
}

bool MelodySynth::Start(const Melody& melody, Timbre timbre, uint8_t volume_pct) {
    active_ = false;
    if (!Valid(melody)) return false;
    melody_ = melody;
    timbre_ = timbre;
    volume_ = volume_pct > 100 ? 100 : volume_pct;
    index_ = 0;
    tick_ = 0;
    phase_ = phase2_ = 0;
    active_ = true;
    BeginNote();
    return true;
}

void MelodySynth::BeginNote() {
    // Skip zero-length entries; finish when the table is used up.
    while (index_ < melody_.count && melody_.notes[index_].ticks == 0) ++index_;
    if (index_ >= melody_.count) {
        active_ = false;
        return;
    }
    const Note& n = melody_.notes[index_];
    len_ = NoteStart(tick_ + n.ticks) - NoteStart(tick_);
    pos_ = 0;
    level_ = 32768;
    rest_ = n.pitch == kRestPitch;
    if (rest_) return;
    const double hz = 440.0 * std::pow(2.0, (static_cast<int>(n.pitch) - 69) / 12.0);
    inc_ = PhaseInc(hz);
    inc2_ = 0;
    decay_ = 32768;
    if (timbre_ == Timbre::Pluck) decay_ = DecayFor(220);
    if (timbre_ == Timbre::Bell) {
        inc2_ = PhaseInc(hz * 2.76);
        decay_ = DecayFor(450);
    }
    // Phases keep running across notes: the attack ramp hides any step.
}

int16_t MelodySynth::Sample() {
    const uint32_t gap = kGapMs * kSynthSampleRate / 1000;
    const uint32_t attack = kAttackMs * kSynthSampleRate / 1000;
    uint32_t tail = kReleaseMs * kSynthSampleRate / 1000;
    if (rest_ || len_ <= gap + 2) return 0;
    const uint32_t body = len_ - gap;
    if (pos_ >= body) return 0;
    if (tail > body / 2) tail = body / 2;
    uint64_t env = level_;  // Q15
    if (pos_ < attack) env = env * pos_ / attack;
    if (pos_ + tail > body) env = env * (body - pos_) / tail;
    int32_t wave;
    switch (timbre_) {
        case Timbre::Sine:
            wave = Sine(phase_);
            break;
        case Timbre::Soft:
        case Timbre::Pluck:
            // 1 : 0.30 : 0.10, weights sum to 256 (183 + 55 + 18).
            wave = (183 * Sine(phase_) + 55 * Sine(phase_ * 2) + 18 * Sine(phase_ * 3)) / 256;
            break;
        case Timbre::Bell:
        default:
            wave = (190 * Sine(phase_) + 66 * Sine(phase2_)) / 256;
            break;
    }
    phase_ += inc_;
    phase2_ += inc2_;
    level_ = (level_ * decay_) >> 15;
    const int64_t v = static_cast<int64_t>(wave) * static_cast<int64_t>(env) / 32768 * kPeak /
                      32767 * volume_ / 100;
    return static_cast<int16_t>(v);
}

size_t MelodySynth::Render(int16_t* out, size_t max) {
    size_t n = 0;
    while (active_ && n < max) {
        if (pos_ >= len_) {
            tick_ += melody_.notes[index_].ticks;
            ++index_;
            BeginNote();
            continue;
        }
        out[n++] = Sample();
        ++pos_;
    }
    return n;
}

// ---- built-in songs ---------------------------------------------------------
//
// Only public-domain tunes, tune only (no lyrics). Written in C major, tick
// = 1/4 beat: quarter 4, eighth 2, half 8, dotted half 12, whole 16. Every
// tune is traditional or by a composer dead for well over 70 years; the
// source of each is noted at its table and in BuiltinMelody::source.
namespace {
constexpr uint8_t G3 = 55;
constexpr uint8_t C4 = 60, D4 = 62, E4 = 64, F4 = 65, G4 = 67, A4 = 69, B4 = 71;
constexpr uint8_t C5 = 72;

// Twinkle, Twinkle, Little Star: French folk tune "Ah! vous dirai-je, maman"
// (published 1761; Mozart's variations K. 265, 1781-82).
constexpr Note kTwinkle[] = {
    {C4, 4}, {C4, 4}, {G4, 4}, {G4, 4}, {A4, 4}, {A4, 4}, {G4, 8},
    {F4, 4}, {F4, 4}, {E4, 4}, {E4, 4}, {D4, 4}, {D4, 4}, {C4, 8},
    {G4, 4}, {G4, 4}, {F4, 4}, {F4, 4}, {E4, 4}, {E4, 4}, {D4, 8},
    {G4, 4}, {G4, 4}, {F4, 4}, {F4, 4}, {E4, 4}, {E4, 4}, {D4, 8},
    {C4, 4}, {C4, 4}, {G4, 4}, {G4, 4}, {A4, 4}, {A4, 4}, {G4, 8},
    {F4, 4}, {F4, 4}, {E4, 4}, {E4, 4}, {D4, 4}, {D4, 4}, {C4, 8},
};

// Two Tigers (Liang zhi lao hu) = Frere Jacques: traditional French round,
// 18th century (earliest prints about 1780-1811), anonymous.
constexpr Note kTwoTigers[] = {
    {C4, 4}, {D4, 4}, {E4, 4}, {C4, 4}, {C4, 4}, {D4, 4}, {E4, 4}, {C4, 4},
    {E4, 4}, {F4, 4}, {G4, 8}, {E4, 4}, {F4, 4}, {G4, 8},
    {G4, 2}, {A4, 2}, {G4, 2}, {F4, 2}, {E4, 4}, {C4, 4},
    {G4, 2}, {A4, 2}, {G4, 2}, {F4, 2}, {E4, 4}, {C4, 4},
    {C4, 4}, {G3, 4}, {C4, 8}, {C4, 4}, {G3, 4}, {C4, 8},
};

// Mary Had a Little Lamb: American tune of the 1830s (words by Sarah Josepha
// Hale, 1830; tune credited to Lowell Mason, d. 1872), public domain.
constexpr Note kMaryLamb[] = {
    {E4, 4}, {D4, 4}, {C4, 4}, {D4, 4}, {E4, 4}, {E4, 4}, {E4, 8},
    {D4, 4}, {D4, 4}, {D4, 8}, {E4, 4}, {G4, 4}, {G4, 8},
    {E4, 4}, {D4, 4}, {C4, 4}, {D4, 4}, {E4, 4}, {E4, 4}, {E4, 4}, {E4, 4},
    {D4, 4}, {D4, 4}, {E4, 4}, {D4, 4}, {C4, 16},
};

// Alle meine Entchen: German children's song, tune first printed in 1840
// (Ludwig Erk's circle), traditional, public domain.
constexpr Note kLittleDucks[] = {
    {C4, 4}, {D4, 4}, {E4, 4}, {F4, 4}, {G4, 8}, {G4, 8},
    {A4, 4}, {A4, 4}, {A4, 4}, {A4, 4}, {G4, 16},
    {A4, 4}, {A4, 4}, {A4, 4}, {A4, 4}, {G4, 16},
    {F4, 4}, {F4, 4}, {F4, 4}, {F4, 4}, {E4, 8}, {E4, 8},
    {D4, 4}, {D4, 4}, {D4, 4}, {D4, 4}, {C4, 16},
};

// Brahms' Lullaby (Wiegenlied, op. 49 no. 4): Johannes Brahms, 1868; he died
// in 1897, public domain. 3/4, simplified and moved from E-flat to C: two
// phrases and the home cadence. Slow, Soft timbre.
constexpr Note kBrahms[] = {
    {E4, 2}, {E4, 2},
    {G4, 8}, {E4, 2}, {E4, 2}, {G4, 8}, {E4, 2}, {G4, 2},
    {C5, 6}, {B4, 2}, {A4, 4}, {A4, 8}, {G4, 4},
    {D4, 2}, {D4, 2},
    {F4, 8}, {D4, 2}, {D4, 2}, {F4, 8}, {D4, 2}, {F4, 2},
    {B4, 6}, {A4, 2}, {G4, 4}, {G4, 4}, {F4, 4}, {E4, 4},
    {E4, 2}, {E4, 2}, {G4, 8}, {D4, 4}, {C4, 12},
};

template <size_t N>
constexpr Melody Tune(const Note (&notes)[N], uint16_t bpm) {
    return Melody{notes, N, bpm};
}

const BuiltinMelody kBuiltins[] = {
    {"song_xiaoxingxing", false,
     "Ah! vous dirai-je, maman (French folk, 1761), public domain", Tune(kTwinkle, 100),
     Timbre::Bell},
    {"song_liangzhilaohu", false,
     "Frere Jacques (traditional French round, 18th c.), public domain", Tune(kTwoTigers, 112),
     Timbre::Soft},
    {"song_xiaoyanggao", false,
     "Mary Had a Little Lamb (US, 1830s, Mason/Hale), public domain", Tune(kMaryLamb, 108),
     Timbre::Pluck},
    {"song_xiaoyazi", false,
     "Alle meine Entchen (German folk, printed 1840), public domain", Tune(kLittleDucks, 104),
     Timbre::Soft},
    {"lullaby_bolamusi", true,
     "Brahms, Wiegenlied op. 49 no. 4 (1868; d. 1897), public domain", Tune(kBrahms, 66),
     Timbre::Soft},
};
}  // namespace

size_t BuiltinMelodyCount() { return sizeof(kBuiltins) / sizeof(kBuiltins[0]); }
const BuiltinMelody& BuiltinMelodyAt(size_t index) { return kBuiltins[index]; }

}  // namespace kid_app
