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


// V9: the child picks the voice with the dial before holding (index 5 = home).
void Choose(ParrotSession& p, uint8_t choice) {
    for (int guard = 0; p.View().choice != choice && guard < 8; ++guard)
        p.Input(GameInput::Down, 0);
    assert(p.View().choice == choice);
}

// Choose the voice, hold, speak `samples` of voice, release.
ParrotOutput Take(ParrotSession& p, uint32_t t0, size_t samples, int peak, uint8_t which = 0) {
    ParrotContext ctx;
    Choose(p, which);
    ParrotOutput o = p.Hold(true, t0, ctx, 0);
    assert(o.start_mic && p.Phase() == ParrotPhase::Recording);
    const auto voice = Voice(samples, peak);
    assert(p.Feed(voice.data(), voice.size()) == std::min(samples, ParrotSession::kMaxSamples));
    return p.Hold(false, t0 + 1000, ctx, 0);
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
            const ParrotOutput o = p.Input(GameInput::Down, 5);
            assert(o.stop_audio && !o.leave && p.Phase() == ParrotPhase::Ready);
        });
        expect_clean_after([&](ParrotSession& p) {  // the arbiter refused the playback
            Take(p, 0, 16000, 9000);
            p.PlaybackRefused();
            assert(p.Phase() == ParrotPhase::Ready);
        });
        expect_clean_after([&](ParrotSession& p) {  // Long while ready leaves
            assert(p.Input(GameInput::Long, 5).leave);
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

    // ---- V9 S1: the child chooses the voice -------------------------------
    // Turning the dial in Ready cycles parrot, chipmunk, bear, robot, echo,
    // then the home slot; Hold on the home slot leaves instead of recording.
    {
        g_buffer.assign(g_buffer.size(), 0);
        ParrotSession p;
        p.Bind(g_buffer.data(), g_buffer.size());
        assert(p.Open(0, ok, nullptr));
        assert(p.View().choice == 0 && p.Selected() == ParrotVoice::Parrot);
        ParrotOutput o = p.Input(GameInput::Down, 1);
        assert(o.redraw && !o.leave && p.View().choice == 1 && p.Phase() == ParrotPhase::Ready);
        for (uint8_t want : {2, 3, 4, 5, 0}) {  // 5 = home, then it wraps
            assert(p.Input(GameInput::Down, 1).redraw && p.View().choice == want);
        }
        assert(p.Input(GameInput::Up, 1).redraw && p.View().choice == ParrotSession::kHomeChoice);
        assert(p.Input(GameInput::Up, 1).redraw && p.View().choice == 4);
        assert(p.Selected() == ParrotVoice::Echo);
        // Re-opening starts on the parrot again.
        p.Interrupt();
        assert(p.Open(2, ok, nullptr) && p.View().choice == 0);
        // The selection survives a take (no need to pick again for a second go).
        Take(p, 10, 16000, 9000, 3);
        std::vector<int16_t> out;
        Drain(p, &out);
        assert(p.Phase() == ParrotPhase::Ready && p.Selected() == ParrotVoice::Robot);
        assert(AllZero(g_buffer));
    }

    // The home slot leaves with a press: nothing is recorded, nothing to wipe.
    {
        g_buffer.assign(g_buffer.size(), 0);
        ParrotSession p;
        p.Bind(g_buffer.data(), g_buffer.size());
        assert(p.Open(0, ok, nullptr));
        Choose(p, ParrotSession::kHomeChoice);
        const ParrotOutput o = p.Hold(true, 5, ok, 0);
        assert(o.leave && !o.start_mic && !p.Active() && AllZero(g_buffer));
    }

    // A turn while recording changes nothing.
    {
        g_buffer.assign(g_buffer.size(), 0);
        ParrotSession p;
        p.Bind(g_buffer.data(), g_buffer.size());
        assert(p.Open(0, ok, nullptr));
        Choose(p, 2);
        assert(p.Hold(true, 0, ok, 0).start_mic);
        assert(!p.Input(GameInput::Up, 1).redraw && p.View().choice == 2);
        p.Interrupt();
    }

    // Every voice: length, level cap, privacy wipe. Speed factors order the
    // lengths; the quiet input is boosted at most 3x; nothing is left in RAM.
    {
        const size_t spoken = 2 * ParrotSession::kInRate;
        size_t len[5] = {};
        for (uint8_t v = 0; v < 5; ++v) {
            std::vector<int16_t> buf(ParrotSession::kMaxSamples);
            ParrotSession p;
            p.Bind(buf.data(), buf.size());
            assert(p.Open(0, ok, nullptr));
            assert(Take(p, 0, spoken, 30000, v).play);
            len[v] = p.PlaybackSamples();
            std::vector<int16_t> out;
            assert(Drain(p, &out) == len[v] && out.size() == len[v]);
            int peak = 0;
            for (int16_t s : out) peak = std::max(peak, std::abs(static_cast<int>(s)));
            assert(peak > 3000 && peak <= ParrotSession::kOutPeak);
            assert(AllZero(buf) && p.BufferClean() && p.Phase() == ParrotPhase::Ready);
        }
        const size_t plain = spoken * ParrotSession::kOutRate / ParrotSession::kInRate;
        assert(len[1] < len[0] && len[0] < plain);             // chipmunk < parrot < plain
        assert(len[3] + 2 >= plain && len[3] <= plain + 2);    // robot keeps the speed
        assert(len[2] > plain * 5 / 4);                        // bear: slower, deeper
        assert(len[4] > plain);                                // echo: the tail
        assert(len[4] - plain + 4 >= ParrotSession::kEchoDelay2 &&
               len[4] - plain <= ParrotSession::kEchoDelay2 + 4);
    }

    // Echo: first the voice, then two quieter copies after kEchoDelay1/2 and
    // nothing beyond the last copy. Computed on the fly from the same
    // recording: no second buffer.
    {
        std::vector<int16_t> buf(ParrotSession::kMaxSamples);
        ParrotSession p;
        p.Bind(buf.data(), buf.size());
        assert(p.Open(0, ok, nullptr));
        const size_t loud = 2000;  // 3000 output samples, shorter than the delay
        const size_t burst = ParrotSession::kMinSamples;  // then silence up to a take
        ParrotContext c;
        Choose(p, 4);
        assert(p.Hold(true, 0, c, 0).start_mic);
        std::vector<int16_t> v(burst, 0);
        std::fill(v.begin(), v.begin() + loud, static_cast<int16_t>(8000));
        p.Feed(v.data(), v.size());
        assert(p.Hold(false, 500, c, 0).play);
        std::vector<int16_t> out;
        Drain(p, &out);
        const size_t body = burst * ParrotSession::kOutRate / ParrotSession::kInRate;
        auto level = [&](size_t at) { return std::abs(static_cast<int>(out[at])); };
        const int a = level(1500);                                   // voice only
        const int b = level(ParrotSession::kEchoDelay1 + 1500);      // 1st copy, quieter
        const int d = level(ParrotSession::kEchoDelay2 + 1500);      // 2nd copy, quieter still
        assert(a > 0 && b > 0 && d > 0 && b < a && d < b);
        assert(level(3500) == 0);  // silence between the voice and its first copy
        assert(out.size() + 4 >= body + ParrotSession::kEchoDelay2 && out.size() <= body + ParrotSession::kEchoDelay2);
        assert(AllZero(buf));
    }

    // Robot: a ring-modulated copy at the plain speed. Deterministic (same
    // take, same sound), and the loud stretch has the carrier's breathing.
    {
        std::vector<int16_t> b1(ParrotSession::kMaxSamples), b2(ParrotSession::kMaxSamples);
        ParrotSession robot, again;
        robot.Bind(b1.data(), b1.size());
        again.Bind(b2.data(), b2.size());
        assert(robot.Open(0, ok, nullptr) && again.Open(0, ok, nullptr));
        const size_t n = ParrotSession::kInRate;
        std::vector<int16_t> flat(n, 9000);  // constant input: any wobble is the carrier
        for (ParrotSession* s : {&robot, &again}) {
            ParrotContext c;
            Choose(*s, 3);
            assert(s->Hold(true, 0, c, 0).start_mic);
            s->Feed(flat.data(), flat.size());
            assert(s->Hold(false, 100, c, 0).play);
        }
        std::vector<int16_t> r1, r2;
        Drain(robot, &r1);
        Drain(again, &r2);
        assert(r1 == r2 && r1.size() > 10000);
        int lo = 0, hi = 0;
        for (size_t i = 1000; i < 9000; ++i) {
            const int a = std::abs(static_cast<int>(r1[i]));
            lo += a < 3000;
            hi += a > 9000;
        }
        assert(lo > 500 && hi > 500);
    }

    // RAM: five voices live in the same small object (no extra buffer).
    static_assert(sizeof(ParrotSession) <= 96, "voices must not add buffers to ParrotSession");
    static_assert(ParrotSession::kChoices == 6 && ParrotSession::kHomeChoice == 5, "5 voices + home");

    // The staging block Render() fills is scrubbed on request (it is a copy of the voice).
    {
        std::array<int16_t, 64> block;
        block.fill(1234);
        ParrotSession::Scrub(block.data(), 40);
        for (size_t i = 0; i < 40; ++i) assert(block[i] == 0);
        assert(block[40] == 1234);  // only what was asked for
        ParrotSession::Scrub(nullptr, 10);  // harmless
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
