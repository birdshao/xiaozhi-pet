#pragma once

#include <cstddef>
#include <cstdint>

namespace kid_app {

// V9-3A built-in melody synthesiser (pure core, no ESP-IDF, no allocation).
//
// A melody is a table of notes (MIDI pitch + length in ticks) and a tempo.
// The synthesiser renders it to 24 kHz mono PCM16, a chunk at a time, so the
// song worker can push 20 ms frames into the same local-PCM queue the TF card
// songs use. Nothing is stored on the card and no recording is needed.
//
// The interface is deliberately small so the rhythm band (slice 3B) can reuse
// it: notes with a length, a timbre and a volume.
//
//   MelodySynth synth;
//   synth.Start(melody, Timbre::Soft, 100);
//   while (size_t n = synth.Render(buf, 480)) push(buf, n);
//
// Rendering is deterministic and independent of the chunk size.
constexpr uint32_t kSynthSampleRate = 24000;
constexpr uint8_t kRestPitch = 0;      // Note::pitch of a silence
constexpr uint8_t kMinPitch = 21;      // A0
// C7. Above this the 1024-entry table lookup (no interpolation) and the
// overtones turn gritty and alias; songs for small children stay far below.
constexpr uint8_t kMaxPitch = 96;
constexpr uint8_t kTicksPerBeat = 4;   // a quarter note = 4 ticks, an eighth = 2
constexpr uint16_t kMinBpm = 30;
constexpr uint16_t kMaxBpm = 240;

enum class Timbre : uint8_t {
    Sine,   // plain and sustained
    Soft,   // sine plus two quiet overtones (default for songs)
    Pluck,  // Soft with a fast decay, like a plucked string
    Bell,   // inharmonic partial with a long decay (glockenspiel)
};

struct Note {
    uint8_t pitch;  // MIDI note number (60 = middle C), kRestPitch = rest
    uint8_t ticks;  // length, kTicksPerBeat per beat; 0 is skipped
};

struct Melody {
    const Note* notes = nullptr;
    size_t count = 0;
    uint16_t bpm = 100;  // beats per minute
};

class MelodySynth {
public:
    // False (and nothing to render) for a null / empty melody, a tempo outside
    // kMinBpm..kMaxBpm or a pitch outside kMinPitch..kMaxPitch. The melody
    // table must outlive the rendering. `volume_pct` 0..100 scales the level;
    // 100 is already a gentle level (about 35% of full scale).
    bool Start(const Melody& melody, Timbre timbre, uint8_t volume_pct);
    // Writes up to `max` samples; returns how many (fewer only at the end,
    // 0 once finished).
    size_t Render(int16_t* out, size_t max);
    bool Done() const { return !active_; }
    void Reset() { active_ = false; }

    // Whole-melody length in samples (0 for an invalid melody). Tempo rounding
    // is cumulative, so it never drifts over a long melody.
    static uint32_t TotalSamples(const Melody& melody);

private:
    uint32_t NoteStart(size_t ticks) const;
    void BeginNote();
    int16_t Sample();

    Melody melody_{};
    Timbre timbre_ = Timbre::Soft;
    uint8_t volume_ = 100;
    bool active_ = false;
    size_t index_ = 0;        // current note
    size_t tick_ = 0;         // ticks before the current note
    uint32_t pos_ = 0;        // sample inside the note
    uint32_t len_ = 0;        // note length in samples
    uint32_t phase_ = 0;      // phase accumulators (32 bit = one cycle)
    uint32_t phase2_ = 0;
    uint32_t inc_ = 0;
    uint32_t inc2_ = 0;
    uint32_t decay_ = 32768;  // Q15 per-sample decay multiplier
    uint32_t level_ = 32768;  // Q15 decay envelope
    int32_t amp_ = 0;         // peak sample value for the chosen volume
    uint32_t body_ = 0;       // samples of sound in the note (the rest is the gap)
    uint32_t tail_ = 0;       // release length in samples
    uint32_t rel_inc_ = 0;    // release ramp per sample, Q23
    bool rest_ = true;
};

// ---- the built-in songs ----------------------------------------------------

struct BuiltinMelody {
    const char* id;      // "song_..." / "lullaby_..." (the player's id rule)
    bool lullaby;        // lullabies get the bedtime timer and pass quiet hours
    const char* source;  // where the tune comes from (public domain)
    Melody melody;
    Timbre timbre;
};

// Streams a melody into a sink in 20 ms frames without any RTOS: this is the
// part of the song worker that can be tested on the host. The caller loops:
//
//   streamer.Start(tune.melody, tune.timbre);
//   for (;;) {
//     if (a newer command arrived) break;     // interrupt: just stop calling
//     Result r = streamer.Step(push, ctx);
//     if (r == Done || r == Stuck) break;     // Stuck: the sink stayed busy
//     if (streamer.Waiting()) sleep(10 ms);   // the sink refused, try again
//   }
//
// A frame the sink refused is kept and offered again (never re-rendered, so
// the stream has no gap or repeat); after kMaxFailures refusals in a row the
// stream is given up (Stuck), like a card page in the song worker.
class MelodyStreamer {
public:
    static constexpr size_t kFrameSamples = 480;   // 20 ms at 24 kHz
    static constexpr unsigned kMaxFailures = 150;  // about 1.5 s of busy / voice
    enum class Result : uint8_t { Continue, Done, Stuck };
    using PushFn = bool (*)(const int16_t* pcm, size_t count, void* context);

    bool Start(const Melody& melody, Timbre timbre, uint8_t volume_pct = 100);
    Result Step(PushFn push, void* context);
    // True when the last Step was refused by the sink (wait a little, retry).
    bool Waiting() const { return failures_ != 0; }
    uint32_t SamplesPushed() const { return pushed_; }

private:
    MelodySynth synth_{};
    int16_t frame_[kFrameSamples] = {};
    size_t frame_len_ = 0;  // > 0: a rendered frame is waiting for the sink
    unsigned failures_ = 0;
    uint32_t pushed_ = 0;
};

size_t BuiltinMelodyCount();
const BuiltinMelody& BuiltinMelodyAt(size_t index);  // index < BuiltinMelodyCount()

}  // namespace kid_app
