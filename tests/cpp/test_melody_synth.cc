// Asserts carry real calls in places: they must never compile away.
#undef NDEBUG
#include "kid_app/melody_synth.h"
#include "kid_app/song_player.h"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

using namespace kid_app;

namespace {
std::vector<int16_t> RenderAll(const Melody& m, Timbre t, uint8_t vol, size_t chunk) {
    MelodySynth s;
    const bool started = s.Start(m, t, vol);
    assert(started);
    (void)started;
    std::vector<int16_t> out;
    std::vector<int16_t> buf(chunk);
    for (;;) {
        const size_t n = s.Render(buf.data(), chunk);
        if (n == 0) break;
        assert(n <= chunk);
        out.insert(out.end(), buf.begin(), buf.begin() + n);
    }
    assert(s.Done());
    return out;
}

int Peak(const std::vector<int16_t>& v, size_t from = 0, size_t to = SIZE_MAX) {
    int p = 0;
    for (size_t i = from; i < v.size() && i < to; ++i) p = std::max(p, std::abs(static_cast<int>(v[i])));
    return p;
}

int Crossings(const std::vector<int16_t>& v) {
    int n = 0;
    for (size_t i = 1; i < v.size(); ++i)
        if ((v[i - 1] < 0) != (v[i] < 0)) ++n;
    return n;
}

double Energy(const std::vector<int16_t>& v, size_t from, size_t to) {
    double e = 0;
    for (size_t i = from; i < to; ++i) e += static_cast<double>(v[i]) * v[i];
    return e;
}
}  // namespace

int main() {
    const Note a4_sec[] = {{69, 8}};  // 2 beats at 120 bpm = 1 s
    const Melody a4{a4_sec, 1, 120};

    // Validation.
    {
        MelodySynth s;
        int16_t b[8];
        assert(!s.Start(Melody{}, Timbre::Soft, 100) && s.Done() && s.Render(b, 8) == 0);
        assert(!s.Start(Melody{a4_sec, 0, 120}, Timbre::Soft, 100));
        assert(!s.Start(Melody{a4_sec, 1, 0}, Timbre::Soft, 100));
        assert(!s.Start(Melody{a4_sec, 1, kMinBpm - 1}, Timbre::Soft, 100));
        assert(!s.Start(Melody{a4_sec, 1, kMaxBpm + 1}, Timbre::Soft, 100));
        const Note low[] = {{kMinPitch - 1, 4}};
        const Note high[] = {{kMaxPitch + 1, 4}};
        const Note top[] = {{kMaxPitch, 4}};
        assert(kMaxPitch <= 96);  // above C7 the table lookup turns gritty
        assert(s.Start(Melody{top, 1, 100}, Timbre::Bell, 100));
        const Note silent[] = {{kRestPitch, 0}};
        assert(!s.Start(Melody{low, 1, 100}, Timbre::Soft, 100));
        assert(!s.Start(Melody{high, 1, 100}, Timbre::Soft, 100));
        assert(!s.Start(Melody{silent, 1, 100}, Timbre::Soft, 100));
        assert(MelodySynth::TotalSamples(Melody{low, 1, 100}) == 0);
        assert(s.Start(a4, Timbre::Soft, 100) && !s.Done());
    }

    // Length: exact, and tempo rounding never drifts.
    {
        assert(MelodySynth::TotalSamples(a4) == kSynthSampleRate);
        const Note odd[] = {{60, 1}, {62, 1}, {64, 1}, {0, 3}, {65, 0}, {67, 7}};
        const Melody m{odd, 6, 97};  // 13 ticks at an awkward tempo
        const uint32_t want = static_cast<uint32_t>(13ull * 60 * kSynthSampleRate / (97ull * 4));
        assert(MelodySynth::TotalSamples(m) == want);
        for (Timbre t : {Timbre::Sine, Timbre::Soft, Timbre::Pluck, Timbre::Bell})
            assert(RenderAll(m, t, 100, 480).size() == want);
    }

    // Deterministic and independent of the chunk size.
    {
        const BuiltinMelody& b = BuiltinMelodyAt(0);
        const auto ref = RenderAll(b.melody, b.timbre, 80, 480);
        for (size_t chunk : {1u, 7u, 333u, 4096u}) {
            const auto other = RenderAll(b.melody, b.timbre, 80, chunk);
            assert(other == ref);
        }
        assert(RenderAll(b.melody, b.timbre, 80, 480) == ref);  // twice: same
    }

    // Pitch: A4 is 440 Hz (880 zero crossings per second); an octave doubles it.
    {
        const auto v = RenderAll(a4, Timbre::Sine, 100, 480);
        assert(std::abs(Crossings(v) - 880) <= 20);
        const Note a5_sec[] = {{81, 8}};
        const auto w = RenderAll(Melody{a5_sec, 1, 120}, Timbre::Sine, 100, 480);
        assert(std::abs(Crossings(w) - 1760) <= 40);
    }

    // Volume: 0 is silence, 50 is about half of 100, nothing clips; the level
    // is gentle (well under full scale) for every timbre.
    {
        for (Timbre t : {Timbre::Sine, Timbre::Soft, Timbre::Pluck, Timbre::Bell}) {
            const auto full = RenderAll(a4, t, 100, 480);
            const auto half = RenderAll(a4, t, 50, 480);
            const auto mute = RenderAll(a4, t, 0, 480);
            assert(Peak(mute) == 0);
            assert(Peak(full) > 3000 && Peak(full) < 16000);
            assert(Peak(half) * 2 >= Peak(full) - 8 && Peak(half) * 2 <= Peak(full) + 8);
            assert(RenderAll(a4, t, 250, 480) == full);  // clamped to 100
        }
    }

    // Shape: starts at zero (no click), each note ends in silence, a repeated
    // note is separated by a gap, a rest is silent.
    {
        const Note rep[] = {{69, 4}, {69, 4}, {kRestPitch, 4}, {72, 4}};
        const Melody m{rep, 4, 120};  // 0.5 s = 12000 samples per note
        const auto v = RenderAll(m, Timbre::Soft, 100, 480);
        assert(v.size() == 48000 && v[0] == 0);
        for (size_t end : {12000u, 24000u}) {
            assert(std::abs(v[end - 1]) < 50);                 // the note ended softly
            assert(Peak(v, end - 150, end) < 300);             // a 6 ms gap before the next
            assert(Peak(v, end - 8000, end - 6000) > 2000);    // and was loud inside
        }
        assert(Peak(v, 24000, 36000) == 0);                    // rest
        assert(Peak(v, 36000, 48000) > 2000);                  // the next note
    }

    // Decaying timbres die away; the sustained ones do not.
    {
        const Note held[] = {{69, 16}};  // 2 s at 120 bpm
        const Melody m{held, 1, 120};
        const auto soft = RenderAll(m, Timbre::Soft, 100, 480);
        const auto pluck = RenderAll(m, Timbre::Pluck, 100, 480);
        const auto bell = RenderAll(m, Timbre::Bell, 100, 480);
        const size_t early = 3000, late = 30000;
        assert(Energy(soft, late, late + 3000) > 0.7 * Energy(soft, early, early + 3000));
        assert(Energy(pluck, late, late + 3000) < 0.2 * Energy(pluck, early, early + 3000));
        assert(Energy(bell, late, late + 3000) < 0.5 * Energy(bell, early, early + 3000));
        assert(soft != pluck && pluck != bell);
    }

    // Reset stops, Start again restarts from the beginning.
    {
        MelodySynth s;
        int16_t b[480];
        assert(s.Start(a4, Timbre::Soft, 100));
        assert(s.Render(b, 480) == 480);
        s.Reset();
        assert(s.Done() && s.Render(b, 480) == 0);
        assert(s.Start(a4, Timbre::Soft, 100) && s.Render(b, 480) == 480 && b[0] == 0);
    }

    // The melody streamer (the song worker's synthesised path, no RTOS): frames
    // of 20 ms, a refused frame is offered again and never re-rendered, a sink
    // that stays busy gives up, an interrupt is just "stop calling".
    {
        struct Sink {
            std::vector<int16_t> got;
            unsigned refuse_next = 0;   // refuse this many pushes, then accept
            unsigned refused = 0;
            bool always_refuse = false;
        };
        const auto push = [](const int16_t* pcm, size_t n, void* ctx) {
            auto* s = static_cast<Sink*>(ctx);
            if (s->always_refuse) return false;
            if (s->refuse_next) {
                --s->refuse_next;
                ++s->refused;
                return false;
            }
            assert(n >= 1 && n <= MelodyStreamer::kFrameSamples);
            s->got.insert(s->got.end(), pcm, pcm + n);
            return true;
        };
        const BuiltinMelody& tune = BuiltinMelodyAt(1);
        const auto want = RenderAll(tune.melody, tune.timbre, 100, 480);

        // Plain run: the same samples as rendering directly, then Done.
        {
            Sink sink;
            MelodyStreamer st;
            assert(st.Start(tune.melody, tune.timbre));
            size_t steps = 0;
            while (st.Step(push, &sink) == MelodyStreamer::Result::Continue) ++steps;
            assert(sink.got == want && st.SamplesPushed() == want.size());
            assert(steps == (want.size() + 479) / 480);
        }
        // A busy queue every few frames: nothing lost, nothing repeated.
        {
            Sink sink;
            MelodyStreamer st;
            assert(st.Start(tune.melody, tune.timbre));
            unsigned n = 0, waits = 0;
            for (;;) {
                if (++n % 7 == 0) sink.refuse_next = 3;
                const auto r = st.Step(push, &sink);
                if (r == MelodyStreamer::Result::Done) break;
                assert(r == MelodyStreamer::Result::Continue);
                if (st.Waiting()) ++waits;
            }
            assert(sink.refused > 10 && waits == sink.refused);
            assert(sink.got == want);
        }
        // A sink that never takes a frame: Stuck after exactly kMaxFailures
        // refusals, the stream is over (Done on the next call), and a restart works.
        {
            Sink sink;
            sink.always_refuse = true;
            MelodyStreamer st;
            assert(st.Start(tune.melody, tune.timbre));
            for (unsigned i = 1; i < MelodyStreamer::kMaxFailures; ++i) {
                assert(st.Step(push, &sink) == MelodyStreamer::Result::Continue && st.Waiting());
            }
            assert(st.Step(push, &sink) == MelodyStreamer::Result::Stuck);
            assert(st.Step(push, &sink) == MelodyStreamer::Result::Done);
            sink.always_refuse = false;
            assert(st.Start(tune.melody, tune.timbre));
            while (st.Step(push, &sink) == MelodyStreamer::Result::Continue) {}
            assert(sink.got == want);
        }
        // One success resets the failure count: 100 refusals twice is no problem.
        {
            Sink sink;
            MelodyStreamer st;
            assert(st.Start(tune.melody, tune.timbre));
            for (int round = 0; round < 2; ++round) {
                sink.refuse_next = 100;
                for (int i = 0; i < 100; ++i)
                    assert(st.Step(push, &sink) == MelodyStreamer::Result::Continue);
                assert(st.Step(push, &sink) == MelodyStreamer::Result::Continue && !st.Waiting());
            }
        }
        // Interrupt: stop calling, start another tune, the first leaves no trace.
        {
            Sink sink;
            MelodyStreamer st;
            assert(st.Start(tune.melody, tune.timbre));
            for (int i = 0; i < 5; ++i) st.Step(push, &sink);
            assert(sink.got.size() == 5 * 480);
            Sink other;
            const BuiltinMelody& next = BuiltinMelodyAt(0);
            assert(st.Start(next.melody, next.timbre));
            while (st.Step(push, &other) == MelodyStreamer::Result::Continue) {}
            assert(other.got == RenderAll(next.melody, next.timbre, 100, 480));
        }
        // An invalid melody never starts; a null sink counts as refusing.
        MelodyStreamer st;
        assert(!st.Start(Melody{}, Timbre::Soft));
        assert(st.Step(push, nullptr) == MelodyStreamer::Result::Done);
    }

    // The tunes do not change by accident: the first notes of each, and the
    // bar structure of the 3/4 lullaby (one beat of pickup, then whole bars).
    {
        const auto first = [](const char* id, std::vector<int> want) {
            for (size_t i = 0; i < BuiltinMelodyCount(); ++i) {
                const BuiltinMelody& b = BuiltinMelodyAt(i);
                if (std::strcmp(b.id, id) != 0) continue;
                assert(b.melody.count >= want.size());
                for (size_t k = 0; k < want.size(); ++k) assert(b.melody.notes[k].pitch == want[k]);
                return;
            }
            assert(false && "tune missing");
        };
        first("song_xiaoxingxing", {60, 60, 67, 67, 69, 69, 67, 65});   // Twinkle
        first("song_liangzhilaohu", {60, 62, 64, 60, 60, 62, 64, 60});  // Two Tigers
        first("song_xiaoyanggao", {64, 62, 60, 62, 64, 64, 64, 62});    // Mary's lamb
        first("song_xiaoyazi", {60, 62, 64, 65, 67, 67, 69, 69});       // Little ducks
        first("lullaby_bolamusi", {64, 64, 67, 64, 64, 67, 64, 67});    // Brahms
        // Every tune starts and ends on the tonic C.
        for (size_t i = 0; i < BuiltinMelodyCount(); ++i) {
            const Melody& m = BuiltinMelodyAt(i).melody;
            assert(m.notes[m.count - 1].pitch % 12 == 0);
        }
        // Brahms: 4 ticks of pickup, then bars of 12 ticks (a phrase may end in
        // a short bar that the next pickup of 4 completes).
        for (size_t i = 0; i < BuiltinMelodyCount(); ++i) {
            const BuiltinMelody& b = BuiltinMelodyAt(i);
            if (!b.lullaby) continue;
            // Pickup, 3 whole bars, a short bar of 8; again; then pickup + 2 bars.
            const unsigned bars[] = {4, 12, 12, 12, 8, 4, 12, 12, 12, 8, 4, 12, 12};
            size_t j = 0;
            unsigned acc = 0;
            for (size_t k = 0; k < b.melody.count; ++k) {
                acc += b.melody.notes[k].ticks;
                assert(j < sizeof(bars) / sizeof(bars[0]) && acc <= bars[j]);  // no note crosses a bar
                if (acc == bars[j]) { acc = 0; ++j; }
            }
            assert(acc == 0 && j == sizeof(bars) / sizeof(bars[0]));
        }
    }

    // The built-in songs: 4..6 tunes, valid ids, public-domain source noted,
    // audible, not clipped, a sensible length, at least one lullaby.
    {
        const size_t n = BuiltinMelodyCount();
        assert(n >= 4 && n <= kMaxBuiltinSongs);
        int lullabies = 0;
        SongCatalog catalog;
        for (size_t i = 0; i < n; ++i) {
            const BuiltinMelody& b = BuiltinMelodyAt(i);
            assert(b.id && b.source && std::strstr(b.source, "public domain"));
            assert(catalog.OfferBuiltin(static_cast<uint8_t>(i), b.id));  // id rule
            assert(std::strncmp(b.id, b.lullaby ? "lullaby_" : "song_", b.lullaby ? 8 : 5) == 0);
            lullabies += b.lullaby ? 1 : 0;
            const uint32_t total = MelodySynth::TotalSamples(b.melody);
            assert(total >= 12 * kSynthSampleRate && total <= 60 * kSynthSampleRate);
            const auto v = RenderAll(b.melody, b.timbre, 100, 480);
            assert(v.size() == total && Peak(v) > 3000 && Peak(v) < 16000);
        }
        assert(lullabies >= 1);
        assert(catalog.Count() == n);  // ids are unique
        // Lullabies only (quiet hours) keeps the built-in index.
        SongPlayer p;
        SongContext quiet;
        quiet.quiet_hours = true;
        assert(p.Open(0, quiet, catalog, nullptr));
        assert(p.View(0).count == lullabies);
        assert(p.Chosen().builtin != 0 && p.Chosen().kind == SongKind::Lullaby);
        assert(BuiltinMelodyAt(p.Chosen().builtin - 1).lullaby);
    }

    std::cout << "melody_synth ok\n";
    return 0;
}
