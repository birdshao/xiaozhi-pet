#include "kid_app/audio_coordinator.h"
#include "kid_app/parrot_session.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <vector>

using namespace kid_app;

namespace {
std::vector<int16_t> g_buffer(ParrotSession::kMaxSamples);

// A deterministic "voice": a triangle wave of the given peak.
std::vector<int16_t> Voice(size_t samples, int peak) {
    std::vector<int16_t> v(samples);
    for (size_t i = 0; i < samples; ++i) {
        const int phase = static_cast<int>(i % 80);
        const int tri = phase < 40 ? phase : 80 - phase;  // 0..40
        v[i] = static_cast<int16_t>((tri - 20) * peak / 20);
    }
    return v;
}

bool AllZero(const std::vector<int16_t>& v) {
    for (int16_t s : v)
        if (s != 0) return false;
    return true;
}


// Open + hold + speak `ms` of voice + release.
ParrotOutput Take(ParrotSession& p, uint32_t t0, size_t samples, int peak, uint32_t seed = 0) {
    ParrotContext ctx;
    ParrotOutput o = p.Hold(true, t0, ctx, seed);
    assert(o.start_mic && p.Phase() == ParrotPhase::Recording);
    const auto voice = Voice(samples, peak);
    assert(p.Feed(voice.data(), voice.size()) == std::min(samples, ParrotSession::kMaxSamples));
    return p.Hold(false, t0 + 1000, ctx, seed);
}

size_t Drain(ParrotSession& p, std::vector<int16_t>* all) {
    const size_t total = p.PlaybackSamples();
    std::array<int16_t, 480> block{};
    size_t off = 0;
    while (off < total) {
        const size_t n = p.Render(off, block.data(), block.size());
        assert(n > 0);
        all->insert(all->end(), block.begin(), block.begin() + n);
        off += n;
    }
    return total;
}
}  // namespace

int main() {
    ParrotContext ok;

    // Open needs a bound buffer; refusals say why and start nothing.
    {
        ParrotSession p;
        ParrotOutput o;
        assert(!p.Open(0, ok, &o));  // no buffer yet
        p.Bind(g_buffer.data(), g_buffer.size());
        ParrotContext c = ok;
        c.quiet_hours = true;
        assert(!p.Open(0, c, &o) && o.hint == ParrotHint::Quiet && !p.Active());
        c = ok;
        c.low_battery = true;
        assert(!p.Open(0, c, &o) && o.hint == ParrotHint::LowBattery);
        c = ok;
        c.voice_active = true;
        assert(!p.Open(0, c, &o) && o.hint == ParrotHint::Busy);
        assert(p.Open(0, ok, &o) && o.redraw && p.Phase() == ParrotPhase::Ready);
        assert(!p.Open(0, ok, &o));  // already open
        p.Bind(g_buffer.data(), 100);  // too small
        assert(!p.Open(0, ok, &o));
    }

    // Record -> release -> playback in a higher/faster voice, then the
    // recording is gone from RAM.
    {
        g_buffer.assign(g_buffer.size(), 0);
        ParrotSession p;
        p.Bind(g_buffer.data(), g_buffer.size());
        assert(p.Open(0, ok, nullptr));
        const size_t spoken = 2 * ParrotSession::kInRate;  // 2 s
        ParrotOutput o = Take(p, 100, spoken, 6000, 0);
        assert(o.play && o.stop_mic && p.Phase() == ParrotPhase::Playing);
        assert(!AllZero(g_buffer));  // sanity: it IS in RAM while playing
        // Higher speed: shorter than a plain 16k -> 24k conversion.
        const size_t plain = spoken * ParrotSession::kOutRate / ParrotSession::kInRate;
        assert(p.PlaybackSamples() < plain);
        assert(p.PlaybackSamples() > plain * 7 / 10);
        std::vector<int16_t> out;
        Drain(p, &out);
        // Quiet child boosted at most 3x, peak never above the cap.
        int peak = 0;
        for (int16_t s : out) peak = std::max(peak, std::abs(static_cast<int>(s)));
        assert(peak > 6000 && peak <= ParrotSession::kOutPeak);
        // Privacy: the last block wiped the buffer, the session is ready again.
        assert(AllZero(g_buffer) && p.BufferClean());
        assert(p.Phase() == ParrotPhase::Ready && p.Recorded() == 0);
        assert(p.Render(0, g_buffer.data(), 1) == 0);
    }

    // The second voice variant is faster still (chipmunk), and louder input is capped.
    {
        std::vector<int16_t> bufa(ParrotSession::kMaxSamples), bufb(ParrotSession::kMaxSamples);
        ParrotSession a, b;  // the buffers must outlive the sessions (they wipe on destruction)
        a.Bind(bufa.data(), bufa.size());
        b.Bind(bufb.data(), bufb.size());
        assert(a.Open(0, ok, nullptr) && b.Open(0, ok, nullptr));
        Take(a, 0, ParrotSession::kInRate, 32000, 0);
        Take(b, 0, ParrotSession::kInRate, 32000, 1);
        assert(b.PlaybackSamples() < a.PlaybackSamples());
        std::vector<int16_t> out;
        Drain(a, &out);
        for (int16_t s : out) assert(std::abs(static_cast<int>(s)) <= ParrotSession::kOutPeak);
        out.clear();
        Drain(b, &out);
        for (int16_t s : out) assert(std::abs(static_cast<int>(s)) <= ParrotSession::kOutPeak);
        assert(AllZero(bufa) && AllZero(bufb));
    }

    // A tap is too short: wiped, ready again, no playback.
    {
        g_buffer.assign(g_buffer.size(), 0);
        ParrotSession p;
        p.Bind(g_buffer.data(), g_buffer.size());
        assert(p.Open(0, ok, nullptr));
        ParrotOutput o = Take(p, 0, ParrotSession::kMinSamples - 1, 8000);
        assert(!o.play && o.hint == ParrotHint::TooShort && p.Phase() == ParrotPhase::Ready);
        assert(AllZero(g_buffer));
    }

    // 5 s cap: more samples are dropped, and Tick releases by itself.
    {
        g_buffer.assign(g_buffer.size(), 0);
        ParrotSession p;
        p.Bind(g_buffer.data(), g_buffer.size());
        assert(p.Open(0, ok, nullptr));
        assert(p.Hold(true, 0, ok, 0).start_mic);
        const auto voice = Voice(ParrotSession::kMaxSamples + 5000, 5000);
        assert(p.Feed(voice.data(), voice.size()) == ParrotSession::kMaxSamples);
        assert(p.Feed(voice.data(), 10) == 0);
        assert(!p.Tick(4999).play);
        const ParrotOutput o = p.Tick(5000);
        assert(o.play && o.stop_mic && p.Phase() == ParrotPhase::Playing);
        std::vector<int16_t> out;
        Drain(p, &out);
        assert(AllZero(g_buffer));
    }

    // Every way out wipes: Interrupt (BOOT / PWR / voice / quiet) while
    // recording and while playing, a dial turn, the audio arbiter refusing.
    {
        auto expect_clean_after = [&](auto act) {
            g_buffer.assign(g_buffer.size(), 0);
            ParrotSession p;
            p.Bind(g_buffer.data(), g_buffer.size());
            assert(p.Open(0, ok, nullptr));
            act(p);
            assert(AllZero(g_buffer) && p.BufferClean());
        };
        expect_clean_after([&](ParrotSession& p) {  // BOOT while recording
            assert(p.Hold(true, 0, ok, 0).start_mic);
            const auto v = Voice(8000, 9000);
            p.Feed(v.data(), v.size());
            assert(!AllZero(g_buffer));
            const ParrotOutput o = p.Interrupt();
            assert(o.stop_mic && o.leave && !p.Active());
        });
        expect_clean_after([&](ParrotSession& p) {  // PWR while playing
            Take(p, 0, 16000, 9000);
            assert(p.Phase() == ParrotPhase::Playing && !AllZero(g_buffer));
            std::array<int16_t, 100> blk{};
            p.Render(0, blk.data(), blk.size());
            const ParrotOutput o = p.Interrupt();
            assert(o.leave && !o.stop_mic && !p.Active());
        });
        expect_clean_after([&](ParrotSession& p) {  // dial turn while playing
            Take(p, 0, 16000, 9000);
            assert(p.Input(GameInput::Down, 5).leave);
        });
        expect_clean_after([&](ParrotSession& p) {  // the arbiter refused the playback
            Take(p, 0, 16000, 9000);
            p.PlaybackRefused();
            assert(p.Phase() == ParrotPhase::Ready);
        });
        expect_clean_after([&](ParrotSession& p) {  // dial turn while ready
            assert(p.Input(GameInput::Up, 5).leave);
        });
        expect_clean_after([&](ParrotSession& p) {  // idle for a minute
            assert(!p.Tick(ParrotSession::kIdleExitMs - 1).leave);
            assert(p.Tick(ParrotSession::kIdleExitMs).leave);
        });
        // Destruction wipes too.
        g_buffer.assign(g_buffer.size(), 7);
        {
            ParrotSession p;
            p.Bind(g_buffer.data(), g_buffer.size());
        }
        assert(AllZero(g_buffer));
    }

    // Dial turns are ignored while the key is held (no accidental leave).
    {
        ParrotSession p;
        p.Bind(g_buffer.data(), g_buffer.size());
        assert(p.Open(0, ok, nullptr));
        assert(p.Hold(true, 0, ok, 0).start_mic);
        assert(!p.Input(GameInput::Down, 1).leave && p.Phase() == ParrotPhase::Recording);
        assert(!p.Input(GameInput::Long, 1).leave);
        p.Interrupt();
    }

    // Context is re-checked at every press: quiet hours / low battery leave,
    // a busy speaker only shows the hint; Feed outside Recording is ignored.
    {
        g_buffer.assign(g_buffer.size(), 0);
        ParrotSession p;
        p.Bind(g_buffer.data(), g_buffer.size());
        assert(p.Open(0, ok, nullptr));
        const int16_t junk[4] = {1, 2, 3, 4};
        assert(p.Feed(junk, 4) == 0 && AllZero(g_buffer));
        ParrotContext c = ok;
        c.audio_busy = true;
        ParrotOutput o = p.Hold(true, 1, c, 0);
        assert(!o.start_mic && o.hint == ParrotHint::Busy && p.Phase() == ParrotPhase::Ready);
        c = ok;
        c.quiet_hours = true;
        o = p.Hold(true, 2, c, 0);
        assert(!o.start_mic && o.leave && o.hint == ParrotHint::Quiet && !p.Active());
        assert(p.Open(3, ok, nullptr));
        c = ok;
        c.voice_active = true;
        assert(!p.Hold(true, 4, c, 0).start_mic);
        assert(!p.Hold(false, 5, ok, 0).play);  // release without a press does nothing
    }

    // Audio preemption: the parrot plays as a pet cue, so BOOT (voice) and
    // PWR (StopAll) cut it, and it never starts over a story or a voice.
    {
        AudioCoordinator audio;
        assert(audio.PlayCue(1).accepted);  // parrot playback
        AudioPlan plan = audio.BeginVoice(2);  // BOOT: XiaoZhi talks
        assert(plan.accepted && plan.actions[0].verb == AudioVerb::Stop &&
               plan.actions[0].stream == AudioStream::PetCue);
        assert(!audio.PlayCue(3).accepted);  // not while voice is active
        audio.EndVoice(2);
        assert(audio.PlayCue(4).accepted);
        plan = audio.StopAll();  // PWR: sleep flow
        assert(plan.accepted && plan.count == 1 && plan.actions[0].verb == AudioVerb::Stop &&
               audio.Active() == AudioStream::None);
        audio.StartStory(5);
        assert(!audio.PlayCue(6).accepted);  // a song/story is playing: parrot waits
    }

    std::cout << "parrot session tests passed\n";
    return 0;
}
