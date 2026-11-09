#include "kid_app/melody_synth.h"

#include <cmath>

namespace kid_app {

namespace {
constexpr size_t kTableBits = 10;
constexpr size_t kTableSize = size_t{1} << kTableBits;
constexpr int32_t kPeak = 11500;      // ~35% of full scale at volume 100
constexpr uint32_t kAttackMs = 6;
constexpr uint32_t kReleaseMs = 25;   // fade at the note end: no clicks
constexpr uint32_t kGapMs = 12;       // silence that separates repeated notes

// Sine by Taylor series, evaluated at compile time: the table is a constant,
// so there is no lazy initialisation to race on when several tasks (the song
// worker, the rhythm band) synthesise at once.
constexpr double kPi = 3.14159265358979323846;

constexpr double TaylorSin(double x) {  // |x| <= pi/2, error < 1e-9
    const double x2 = x * x;
    double term = x, sum = x;
    for (int k = 1; k <= 8; ++k) {
        term = -term * x2 / ((2 * k) * (2 * k + 1));
        sum += term;
    }
    return sum;
}

constexpr double SinTurn(double turns) {  // turns in [0, 1)
    double t = turns;  // fold into [-1/4, 1/4] turns
    if (t > 0.75) t -= 1.0;
    else if (t > 0.25) t = 0.5 - t;
    return TaylorSin(2.0 * kPi * t);
}

struct SineLut {
    int16_t v[kTableSize];
    constexpr SineLut() : v() {
        for (size_t i = 0; i < kTableSize; ++i) {
            const double x = 32767.0 * SinTurn(static_cast<double>(i) / kTableSize);
            v[i] = static_cast<int16_t>(x >= 0 ? x + 0.5 : x - 0.5);
        }
    }
};
constexpr SineLut kSine{};

int32_t Sine(uint32_t phase) { return kSine.v[phase >> (32 - kTableBits)]; }

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
    amp_ = kPeak * volume_ / 100;
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
    // Envelope shape of this note, so Sample() needs no division.
    const uint32_t gap = kGapMs * kSynthSampleRate / 1000;
    body_ = len_ > gap + 2 ? len_ - gap : 0;
    tail_ = kReleaseMs * kSynthSampleRate / 1000;
    if (tail_ > body_ / 2) tail_ = body_ / 2;
    rel_inc_ = tail_ ? (32768u << 8) / tail_ : 0;
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
    constexpr uint32_t kAttack = kAttackMs * kSynthSampleRate / 1000;
    constexpr uint32_t kAttackInc = 32768 / kAttack;  // Q15 per sample
    if (rest_ || pos_ >= body_) return 0;
    // Envelope in Q15: decay level, attack ramp, release ramp. Only shifts and
    // 32-bit multiplies (no division) per sample.
    int32_t env = static_cast<int32_t>(level_);
    if (pos_ < kAttack) env = (env * static_cast<int32_t>(pos_ * kAttackInc)) >> 15;
    const uint32_t left = body_ - pos_;
    if (left < tail_) env = (env * static_cast<int32_t>((left * rel_inc_) >> 8)) >> 15;
    int32_t wave;
    switch (timbre_) {
        case Timbre::Sine:
            wave = Sine(phase_);
            break;
        case Timbre::Soft:
        case Timbre::Pluck:
            // 1 : 0.30 : 0.10, weights sum to 256 (183 + 55 + 18).
            wave = (183 * Sine(phase_) + 55 * Sine(phase_ * 2) + 18 * Sine(phase_ * 3)) >> 8;
            break;
        case Timbre::Bell:
        default:
            wave = (190 * Sine(phase_) + 66 * Sine(phase2_)) >> 8;
            break;
    }
    phase_ += inc_;
    phase2_ += inc2_;
    level_ = (level_ * decay_) >> 15;
    // wave (+-32767) * env (Q15) >> 15 stays +-32767; times amp_ (<= 11500) >> 15.
    return static_cast<int16_t>((((wave * env) >> 15) * amp_) >> 15);
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

bool MelodyStreamer::Start(const Melody& melody, Timbre timbre, uint8_t volume_pct) {
    frame_len_ = 0;
    failures_ = 0;
    pushed_ = 0;
    return synth_.Start(melody, timbre, volume_pct);
}

MelodyStreamer::Result MelodyStreamer::Step(PushFn push, void* context) {
    if (frame_len_ == 0) {
        frame_len_ = synth_.Render(frame_, kFrameSamples);
        if (frame_len_ == 0) return Result::Done;
    }
    if (!push || !push(frame_, frame_len_, context)) {
        if (++failures_ >= kMaxFailures) {
            frame_len_ = 0;
            failures_ = 0;
            synth_.Reset();
            return Result::Stuck;
        }
        return Result::Continue;  // the same frame is offered again
    }
    pushed_ += static_cast<uint32_t>(frame_len_);
    frame_len_ = 0;
    failures_ = 0;
    return Result::Continue;
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
// phrases and the home cadence (a pickup, then whole 3/4 bars; a phrase end is
// a short bar that the next pickup completes). Slow, Soft timbre.
constexpr Note kBrahms[] = {
    {E4, 2}, {E4, 2},                                      // pickup (1 beat)
    {G4, 8}, {E4, 2}, {E4, 2}, {G4, 8}, {E4, 2}, {G4, 2},  // 3/4 bars of 12 ticks
    {C5, 6}, {B4, 2}, {A4, 4}, {A4, 8},
    {D4, 2}, {D4, 2},                                      // pickup
    {F4, 8}, {D4, 2}, {D4, 2}, {F4, 8}, {D4, 2}, {F4, 2},
    {B4, 6}, {A4, 2}, {G4, 4}, {G4, 4}, {F4, 4},
    {E4, 2}, {E4, 2},                                      // pickup into the cadence
    {G4, 8}, {D4, 4}, {C4, 12},
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
