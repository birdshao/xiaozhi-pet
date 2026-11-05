// V4-08 microphone games: the envelope classifier (blow / clap / speech /
// noise from synthetic RMS sequences only), the candle and clap-rhythm
// sessions, the growth memory rule and the game scene's refresh regions.
// No audio anywhere: the classifier takes one RMS number per 10 ms.
#include "kid_app/memory_store.h"
#include "kid_app/mic_envelope.h"
#include "kid_app/mic_game.h"
#include "kid_app/scene_model.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

using namespace kid_app;

namespace {

struct Heard {
    int claps = 0;
    int blow_frames = 0;
    int first_blow_ms = -1;
    int last_blow_ms = -1;
    uint16_t floor = 0;
};

// Feeds `ms` of a synthetic sound, one frame per 10 ms, onto `c`.
void Play(MicClassifier& c, Heard& h, MicSynth kind, uint32_t ms, uint32_t* clock_ms) {
    for (uint32_t t = 0; t < ms; t += MicClassifier::kFrameMs) {
        const MicDecision d = c.Feed(SynthEnvelopeRms(kind, t));
        if (d.clap) ++h.claps;
        if (d.blowing) {
            ++h.blow_frames;
            if (h.first_blow_ms < 0) h.first_blow_ms = static_cast<int>(*clock_ms + t);
            h.last_blow_ms = static_cast<int>(*clock_ms + t);
        }
    }
    *clock_ms += ms;
    h.floor = c.Floor();
}

struct Rig {
    MicClassifier c;
    Heard h;
    uint32_t clock = 0;
    Rig() { Play(c, h, MicSynth::Silence, 600, &clock); }  // learn the room
    void Do(MicSynth kind, uint32_t ms) { Play(c, h, kind, ms, &clock); }
};

void TestClassifier() {
    {  // Nothing before the room is learned; silence and a steady room never trigger.
        MicClassifier c;
        for (uint32_t i = 0; i < MicClassifier::kWarmFrames; ++i) {
            const auto d = c.Feed(20000);  // even a loud start decides nothing
            assert(!d.clap && !d.blowing);
        }
        assert(c.Warm());
    }
    {
        Rig r;
        r.Do(MicSynth::Silence, 3000);
        r.Do(MicSynth::Noise, 5000);
        r.Do(MicSynth::Silence, 1000);
        assert(r.h.claps == 0 && r.h.blow_frames == 0);
    }
    {  // One clap is one clap, not a blow.
        Rig r;
        r.Do(MicSynth::Clap, 300);
        r.Do(MicSynth::Silence, 500);
        assert(r.h.claps == 1 && r.h.blow_frames == 0);
    }
    {  // Two claps half a second apart, three at the demo's spacing.
        Rig r;
        r.Do(MicSynth::Clap, 500);
        r.Do(MicSynth::Clap, 500);
        r.Do(MicSynth::Silence, 300);
        assert(r.h.claps == 2);
        Rig q;
        for (int i = 0; i < 3; ++i) q.Do(MicSynth::Clap, 700);
        assert(q.h.claps == 3 && q.h.blow_frames == 0);
    }
    {  // Speech: syllables never become a clap or a blow.
        Rig r;
        r.Do(MicSynth::Speech, 5000);
        r.Do(MicSynth::Silence, 300);
        assert(r.h.claps == 0 && r.h.blow_frames == 0);
    }
    {  // A blow: flagged after ~250 ms, steady until it ends, no clap at the end.
        Rig r;
        const uint32_t start = r.clock;
        r.Do(MicSynth::Blow, 1500);
        r.Do(MicSynth::Silence, 500);
        assert(r.h.claps == 0);
        assert(r.h.blow_frames > 100);
        assert(r.h.first_blow_ms - static_cast<int>(start) >= 240);
        assert(r.h.first_blow_ms - static_cast<int>(start) <= 400);
        assert(r.h.last_blow_ms - static_cast<int>(start) <= 1540);  // + the 30 ms quiet run
        // The flag drops with the sound: no blowing in the silence after it.
        const MicDecision after = r.c.Feed(SynthEnvelopeRms(MicSynth::Silence, 0));
        assert(!after.blowing && !after.clap);
    }
    {  // Too short a puff, and a quiet one, are not blows.
        Rig r;
        r.Do(MicSynth::Blow, 200);
        r.Do(MicSynth::Silence, 500);
        assert(r.h.blow_frames == 0);
    }
    {  // A loud drone becomes the new room after 6 s: not a blow for ever, and
       // a clap still works once the room is quiet again.
        Rig r;
        r.Do(MicSynth::Blow, 9000);
        assert(r.h.last_blow_ms - 600 < 6200);
        r.Do(MicSynth::Silence, 4000);
        r.Do(MicSynth::Clap, 300);
        r.Do(MicSynth::Silence, 300);
        assert(r.h.claps == 1);
    }
    {  // Mumbling: a long burst that keeps dipping is speech, not a blow.
        Rig r;
        for (int k = 0; k < 4; ++k) {
            for (int i = 0; i < 12; ++i) r.c.Feed(2600);
            for (int i = 0; i < 2; ++i) r.c.Feed(500);  // under half the mean, above the end level
        }
        MicDecision d{};
        for (int i = 0; i < 10; ++i) {
            d = r.c.Feed(2600);
            assert(!d.blowing);
        }
    }
    {  // A loud room from the very start is just the floor.
        MicClassifier c;
        for (int i = 0; i < 600; ++i) {
            const auto d = c.Feed(static_cast<uint16_t>(2000 + (i * 37) % 80));
            assert(!d.clap && !d.blowing);
        }
    }
    {  // Deterministic: the same frames, the same decisions.
        Rig a, b;
        for (Rig* r : {&a, &b}) {
            r->Do(MicSynth::Speech, 1000);
            r->Do(MicSynth::Clap, 400);
            r->Do(MicSynth::Blow, 900);
            r->Do(MicSynth::Silence, 400);
        }
        assert(a.h.claps == b.h.claps && a.h.blow_frames == b.h.blow_frames &&
               a.h.first_blow_ms == b.h.first_blow_ms && a.h.floor == b.h.floor);
        assert(a.h.claps == 1 && a.h.blow_frames > 30);
    }
    {  // Step names (the self-test `envelope` step).
        MicSynth kind{};
        for (const char* name : {"blow", "clap", "speech", "noise"}) {
            assert(MicSynthFromName(name, &kind) && std::strcmp(MicSynthName(kind), name) == 0);
        }
        assert(!MicSynthFromName("silence", &kind) && !MicSynthFromName("shout", &kind));
        assert(!MicSynthFromName(nullptr, &kind));
    }
}

// ---- Sessions ----

constexpr MicDecision kBlow{true, false};
constexpr MicDecision kClap{false, true};
constexpr MicDecision kNothing{};

struct Log {
    std::vector<std::pair<uint32_t, uint16_t>> clips;
    bool right = false, finished = false;
    int redraws = 0;
};

void Take(Log* log, const GameOutput& out, uint32_t now) {
    if (out.clip) log->clips.push_back({now, out.clip});
    log->right = log->right || out.right;
    log->finished = log->finished || out.finished;
    if (out.redraw) ++log->redraws;
}

int Count(const Log& log, uint16_t clip) {
    int n = 0;
    for (const auto& c : log.clips) n += c.second == clip;
    return n;
}

void TestCandle() {
    {  // Three steady blows put out three candles; only then the reward.
        MicGameSession s;
        Log log;
        GameOutput out{};
        assert(s.Start(MicGameKind::Candle, 1000, &out));
        Take(&log, out, 1000);
        assert(out.redraw && out.clip == kMicClipStart && s.Active() && s.MicWanted());
        assert(s.LitCandles() == 3 && s.Result().game_id == kMicCandleGameId);
        const GameView first = s.View();
        assert(first.question_art == static_cast<uint16_t>(MicArt::Blow) && first.option_count == 3);
        for (uint8_t i = 0; i < 3; ++i)
            assert(first.options[i] == static_cast<uint16_t>(MicArt::CandleLit));
        // Our own start tone is not a blow: the first 700 ms are deaf.
        uint32_t t = 1000;
        for (; t < 1000 + MicGameSession::kEchoGuardMs - 20; t += 20) Take(&log, s.Tick(t, kBlow), t);
        assert(s.LitCandles() == 3);
        int outs = 0;
        for (; t < 10000 && !log.right; t += 20) {
            const uint8_t before = s.LitCandles();
            Take(&log, s.Tick(t, kBlow), t);
            if (s.LitCandles() != before) ++outs;
        }
        assert(outs == 3 && log.right && s.LitCandles() == 0);
        assert(Count(log, kMicClipOut) == 2 && Count(log, kGameClipRight) == 1);
        const GameView done = s.View();
        assert(done.feedback == GameFeedback::Right);
        for (uint8_t i = 0; i < 3; ++i)
            assert(done.options[i] == static_cast<uint16_t>(MicArt::CandleOut));
        // Candle outs are only possible after a steady 700 ms each (plus the guards).
        assert(t - 1000 >= 3 * MicGameSession::kBlowMs);
        for (; !log.finished && t < 20000; t += 20) Take(&log, s.Tick(t, kNothing), t);
        assert(log.finished && !s.Active() && s.Result().end == GameEnd::Completed);
        assert(s.Result().correct == 1 && s.Result().played == 1);
        MemoryEvent e{};
        assert(GameCompletedMemoryEvent(s.Result(), 7, 3, &e) && e.option == kMicCandleGameId);
        assert(Count(log, kGameClipBye) == 1);
    }
    {  // Claps, speech and silence never put a candle out; gaps in blowing cost progress.
        MicGameSession s;
        GameOutput out{};
        assert(s.Start(MicGameKind::Candle, 0, &out));
        for (uint32_t t = 0; t < 20000; t += 20) {
            const GameOutput o = s.Tick(t, t % 500 == 0 ? kClap : kNothing);
            assert(!o.right && !o.finished);
        }
        assert(s.LitCandles() == 3);
        // 400 ms of blowing, a 400 ms gasp, 400 ms again: no candle.
        MicGameSession g;
        assert(g.Start(MicGameKind::Candle, 0, &out));
        uint32_t t = 1000;
        for (int burst = 0; burst < 3; ++burst) {
            for (uint32_t i = 0; i < 400; i += 20, t += 20) g.Tick(t, kBlow);
            for (uint32_t i = 0; i < 400; i += 20, t += 20) g.Tick(t, kNothing);
        }
        assert(g.LitCandles() == 3);
    }
    {  // Nobody blows: home after 30 s, no growth. A long press goes home at once.
        MicGameSession s;
        GameOutput out{};
        assert(s.Start(MicGameKind::Candle, 5000, &out));
        bool finished = false;
        uint32_t t = 5000;
        for (; t < 5000 + 40000 && !finished; t += 20) finished = s.Tick(t, kNothing).finished;
        assert(finished && s.Result().end == GameEnd::Idle);
        assert(t - 5000 >= MicGameSession::kIdleExitMs);
        MemoryEvent e{};
        assert(!GameCompletedMemoryEvent(s.Result(), 1, 1, &e));
        MicGameSession h;
        assert(h.Start(MicGameKind::Candle, 0, &out));
        out = h.Input(GameInput::Press, 100);  // the dial does nothing else
        assert(!out.redraw && !out.finished && h.Active());
        out = h.Input(GameInput::Long, 200);
        assert(out.finished && h.Result().end == GameEnd::Home);
        assert(!GameCompletedMemoryEvent(h.Result(), 1, 1, &e));
        assert(!h.MicWanted() && !h.Tick(300, kBlow).redraw);
    }
    {  // Interrupted (voice, sleep): no growth, microphone off.
        MicGameSession s;
        GameOutput out{};
        assert(s.Start(MicGameKind::Candle, 0, &out));
        s.Interrupt();
        assert(!s.Active() && !s.MicWanted() && s.Result().end == GameEnd::Interrupted);
        MemoryEvent e{};
        assert(!GameCompletedMemoryEvent(s.Result(), 1, 1, &e));
    }
}

// Runs a clap session on a 20 ms loop. `claps` are the instants the child
// claps (the session only hears them while it listens, like the board).
struct ClapRun {
    MicGameSession s;
    Log log;
    uint32_t t = 0;
    size_t next_clap = 0;
    std::vector<uint32_t> claps;
    uint32_t listen_from = 0;

    void Start(uint32_t at) {
        GameOutput out{};
        assert(s.Start(MicGameKind::ClapRhythm, at, &out));
        Take(&log, out, at);
        t = at;
    }
    // Advances to `until`; claps scheduled before then are delivered when the
    // microphone is wanted, dropped (not heard) otherwise.
    void RunTo(uint32_t until) {
        for (; t < until; t += 20) {
            MicDecision mic = kNothing;
            while (next_clap < claps.size() && claps[next_clap] <= t) {
                if (claps[next_clap] + 20 > t) mic = s.MicWanted() ? kClap : kNothing;
                ++next_clap;
            }
            Take(&log, s.Tick(t, s.MicWanted() ? mic : kNothing), t);
        }
    }
};

void TestClapRhythm() {
    {  // The demo plays three beats at the pattern's gaps, then "your turn", then listens.
        ClapRun r;
        r.Start(1000);
        assert(r.log.clips.size() == 1 && r.log.clips[0].second == kMicClipStart);
        assert(!r.s.MicWanted() && r.s.Result().game_id == kMicClapGameId);
        r.RunTo(5900);
        std::vector<uint32_t> beats;
        uint32_t turn = 0;
        for (const auto& c : r.log.clips) {
            if (c.second == kMicClipBeat) beats.push_back(c.first);
            if (c.second == kMicClipYourTurn) turn = c.first;
        }
        assert(beats.size() == 3);
        assert(beats[0] >= 2500 && beats[0] < 2540);
        assert(beats[1] - beats[0] >= 700 && beats[1] - beats[0] <= 740);
        assert(beats[2] - beats[1] >= 700 && beats[2] - beats[1] <= 740);
        assert(turn >= beats[2] + MicGameSession::kTurnGapMs);
        assert(r.s.MicWanted());  // only now
    }
    {  // Clap it back right, twice: two rewards, then finished with growth.
        ClapRun r;
        r.claps = {6000, 6700, 7400};  // round 1: 700 / 700
        r.Start(1000);
        r.RunTo(7500);
        assert(r.log.right && r.s.Result().correct == 1);
        assert(r.s.View().feedback == GameFeedback::Right);
        assert(r.s.View().options[2] == static_cast<uint16_t>(MicArt::HandHit));
        // Round 2 starts 2.5 s later: beats at +1.5 s, then 500 / 1000 gaps.
        r.RunTo(7400 + 2600);
        assert(r.s.Round() == 1 && !r.s.MicWanted());
        const uint32_t r2 = 7400 + 2500;  // round 2 starts here (20 ms loop)
        const uint32_t listen = r2 + 1500 + 500 + 1000 + 900 + 800;
        r.claps.push_back(listen + 500);
        r.claps.push_back(listen + 1000);
        r.claps.push_back(listen + 2000);
        r.RunTo(listen + 2100);
        assert(r.s.Result().correct == 2);
        assert(Count(r.log, kGameClipRight) == 2);
        r.RunTo(listen + 2100 + 2600);
        assert(r.log.finished && r.s.Result().end == GameEnd::Completed);
        assert(r.s.Result().played == 2);
        MemoryEvent e{};
        assert(GameCompletedMemoryEvent(r.s.Result(), 1, 1, &e) && e.option == kMicClapGameId);
    }
    {  // Claps while the demo or the tones play are not heard (the speaker).
        ClapRun r;
        r.claps = {2400, 2500, 2600, 3000, 4800, 5000};
        r.Start(1000);
        r.RunTo(5100);  // demo, "your turn" tone: microphone off
        assert(r.s.ClapsHeard() == 0 && !r.s.MicWanted());
        r.RunTo(5500);  // then open but still learning the room
        assert(r.s.MicWanted() && r.s.ClapsHeard() == 0);
    }
    {  // Wrong rhythm: try again (same pattern), then shown once more, then on; no reward.
        ClapRun r;
        r.Start(1000);
        // Fast claps 200 ms apart, twice per round, every round.
        for (int round = 0; round < 2; ++round)
            for (int tries = 0; tries < 2; ++tries) {
                // run until listening, then clap too fast
                while (!r.s.MicWanted() && r.t < 80000) r.RunTo(r.t + 20);
                assert(r.s.MicWanted());
                r.RunTo(r.t + MicGameSession::kWarmMs + 60);  // learning the room first
                const uint32_t at = r.t + 100;
                r.claps = {at, at + 200, at + 400};
                r.next_clap = 0;
                r.RunTo(at + 500);
                assert(!r.log.right);
                assert(r.s.View().feedback ==
                       (tries == 0 ? GameFeedback::TryAgain : GameFeedback::Reveal));
                r.RunTo(r.t + 2600);
            }
        assert(r.log.finished && r.s.Result().end == GameEnd::Completed);
        assert(r.s.Result().correct == 0 && r.s.Result().played == 2);
        assert(Count(r.log, kGameClipTryAgain) == 2 && Count(r.log, kGameClipReveal) == 2);
        MemoryEvent e{};
        assert(!GameCompletedMemoryEvent(r.s.Result(), 1, 1, &e));  // no success: no growth
    }
    {  // Silence: the listening window times out; a lone clap stalls out too.
        ClapRun r;
        r.Start(0);
        while (!r.s.MicWanted() && r.t < 20000) r.RunTo(r.t + 20);
        const uint32_t opened = r.t;
        r.RunTo(opened + MicGameSession::kWarmMs + MicGameSession::kListenMs + 100);
        assert(!r.s.MicWanted() && r.s.View().feedback == GameFeedback::TryAgain);
        ClapRun q;
        q.Start(0);
        while (!q.s.MicWanted() && q.t < 20000) q.RunTo(q.t + 20);
        q.RunTo(q.t + MicGameSession::kWarmMs + 60);
        q.claps = {q.t + 100};
        q.RunTo(q.t + 100 + MicGameSession::kStallMs + 100);
        assert(q.s.View().feedback == GameFeedback::TryAgain);
    }
    {  // Long press: home, no growth, microphone off.
        ClapRun r;
        r.Start(0);
        r.RunTo(300);
        const GameOutput out = r.s.Input(GameInput::Long, 400);
        assert(out.finished && r.s.Result().end == GameEnd::Home && !r.s.MicWanted());
        MemoryEvent e{};
        assert(!GameCompletedMemoryEvent(r.s.Result(), 1, 1, &e));
    }
    {  // Interrupted while listening.
        ClapRun r;
        r.Start(0);
        while (!r.s.MicWanted() && r.t < 20000) r.RunTo(r.t + 20);
        r.s.Interrupt();
        assert(!r.s.MicWanted() && !r.s.Active());
    }
}

// ---- Scene ----

int DiffRows(const SceneModel& a, const SceneModel& b, int* top, int* bottom) {
    MonoFrame fa, fb;
    SceneComposer::Render(a, fa);
    SceneComposer::Render(b, fb);
    int count = 0;
    *top = MonoFrame::kHeight;
    *bottom = -1;
    for (int y = 0; y < MonoFrame::kHeight; ++y)
        for (int x = 0; x < MonoFrame::kWidth; ++x)
            if (fa.IsBlack(x, y) != fb.IsBlack(x, y)) {
                ++count;
                if (y < *top) *top = y;
                if (y > *bottom) *bottom = y;
            }
    return count;
}

SceneModel GameScene(const GameView& view) {
    SceneModel scene{};
    scene.kind = SceneKind::Game;
    scene.game = view;
    return scene;
}

int Ink(const SceneModel& scene, int x0, int y0, int x1, int y1) {
    MonoFrame f;
    SceneComposer::Render(scene, f);
    int n = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) n += f.IsBlack(x, y);
    return n;
}

void TestScene() {
    {  // A candle going out only changes the answer rail; the reaction only the question area.
        MicGameSession s;
        GameOutput out{};
        assert(s.Start(MicGameKind::Candle, 0, &out));
        const GameView lit = s.View();
        GameView one_out = lit;
        one_out.options[0] = static_cast<uint16_t>(MicArt::CandleOut);
        assert(lit.RailDiffers(one_out) && !lit.QuestionDiffers(one_out));
        int top = 0, bottom = 0;
        assert(DiffRows(GameScene(lit), GameScene(one_out), &top, &bottom) > 0);
        assert(top >= SceneComposer::kGameRailTop);
        GameView happy = lit;
        happy.feedback = GameFeedback::Right;
        assert(!lit.RailDiffers(happy) && lit.QuestionDiffers(happy));
        assert(DiffRows(GameScene(lit), GameScene(happy), &top, &bottom) > 0);
        assert(bottom < SceneComposer::kGameRailTop);
    }
    {  // Every mic picture draws something in its card, none in the other cards,
       // and the dial highlight (none) adds no frame.
        for (uint16_t art = 101; art <= 105; ++art) {
            assert(MicArtValid(art));
            GameView v{};
            v.round = 1;
            v.rounds = 2;
            v.option_count = 3;
            v.highlight = 0xFF;
            v.question_art = static_cast<uint16_t>(MicArt::Blow);
            v.options = {art, art, art};
            GameView empty = v;
            empty.options = {0, 0, 0};
            const int with = Ink(GameScene(v), 0, SceneComposer::kGameRailTop, 800, 480);
            const int without = Ink(GameScene(empty), 0, SceneComposer::kGameRailTop, 800, 480);
            assert(with > without + 200);
        }
        assert(!MicArtValid(100) && !MicArtValid(106));
        // Not part of the pack catalogue.
        for (uint16_t art = 101; art <= 105; ++art) assert(!GameArtIsQuestion(art));
        // The clap rail: a hit hand has more ink than the waiting one.
        GameView wait{};
        wait.round = 1;
        wait.rounds = 2;
        wait.option_count = 3;
        wait.highlight = 0xFF;
        wait.question_art = static_cast<uint16_t>(MicArt::Hand);
        wait.options = {103, 103, 103};
        GameView hit = wait;
        hit.options[1] = static_cast<uint16_t>(MicArt::HandHit);
        assert(Ink(GameScene(hit), 0, 296, 800, 480) > Ink(GameScene(wait), 0, 296, 800, 480));
    }
}

}  // namespace

int main() {
    TestClassifier();
    TestCandle();
    TestClapRhythm();
    TestScene();
    std::cout << "mic games: ok\n";
    return 0;
}
