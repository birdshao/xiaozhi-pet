#include "kid_app/voice_face.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using kid_app::ApplyVoiceFace;
using kid_app::Emotion;
using kid_app::SceneKind;
using kid_app::SceneModel;
using kid_app::VoiceFace;
using kid_app::VoiceFaceTracker;
using kid_app::VoiceLink;

namespace {

constexpr uint32_t kSettle = VoiceFaceTracker::kSettleMs;
constexpr uint32_t kDwell = VoiceFaceTracker::kMinDwellMs;

// Drives the tracker like the board's 20 ms input loop and records every
// face change with its time.
struct Sim {
    explicit Sim(uint32_t start_ms = 1000) : now(start_ms) {}
    VoiceFaceTracker tracker;
    uint32_t now;
    uint32_t heard = 0;
    std::vector<std::pair<uint32_t, VoiceFace>> changes;

    void Run(VoiceLink link, uint32_t ms) {
        for (uint32_t t = 0; t < ms; t += 20) {
            VoiceFace face{};
            if (tracker.Update(link, heard, now, &face)) changes.push_back({now, face});
            now += 20;
        }
    }
    VoiceFace Last() const { return changes.empty() ? VoiceFace::None : changes.back().second; }
};

}  // namespace

int main() {
    // Names for the PET_VOICE_FACE log line.
    assert(std::string(kid_app::VoiceFaceName(VoiceFace::None)) == "none");
    assert(std::string(kid_app::VoiceFaceName(VoiceFace::Listening)) == "listening");
    assert(std::string(kid_app::VoiceFaceName(VoiceFace::Thinking)) == "thinking");
    assert(std::string(kid_app::VoiceFaceName(VoiceFace::Speaking)) == "speaking");

    // Overlay: home scenes take the face as their emotion; everything else
    // (selection, pose, page) is kept. Reader and sleep scenes never change.
    {
        const SceneModel home{SceneKind::Home, Emotion::Happy, 3, 4};
        assert(ApplyVoiceFace(home, VoiceFace::Listening).emotion == Emotion::Listening);
        assert(ApplyVoiceFace(home, VoiceFace::Thinking).emotion == Emotion::Thinking);
        const SceneModel speaking = ApplyVoiceFace(home, VoiceFace::Speaking);
        assert(speaking.emotion == Emotion::Speaking && speaking.home_selection == 4 &&
               speaking.book_page == 3 && speaking.kind == SceneKind::Home);
        assert(ApplyVoiceFace(home, VoiceFace::None).emotion == Emotion::Happy);
        for (SceneKind kind : {SceneKind::Reader, SceneKind::Sleep}) {
            const SceneModel other{kind, Emotion::Curious, 2, 1};
            for (VoiceFace face : {VoiceFace::Listening, VoiceFace::Thinking, VoiceFace::Speaking})
                assert(ApplyVoiceFace(other, face).emotion == Emotion::Curious);
        }
    }

    // Idle XiaoZhi: no face, nothing to draw.
    {
        Sim sim;
        sim.Run(VoiceLink::Off, 5000);
        assert(sim.changes.empty());
        assert(sim.tracker.Shown() == VoiceFace::None);
    }

    // One full turn: listen, the sentence is recognised (thinking), reply,
    // back to listening (XiaoZhi auto mode), then the session ends.
    {
        Sim sim;
        sim.Run(VoiceLink::Off, 500);
        const uint32_t pressed = sim.now;
        sim.Run(VoiceLink::Listening, kSettle - 40);
        assert(sim.changes.empty());  // not settled yet
        sim.Run(VoiceLink::Listening, 3000);
        assert(sim.changes.size() == 1 && sim.Last() == VoiceFace::Listening);
        assert(sim.changes[0].first - pressed >= kSettle && sim.changes[0].first - pressed < kSettle + 60);

        ++sim.heard;  // "stt": the child's sentence was recognised
        const uint32_t heard_at = sim.now;
        sim.Run(VoiceLink::Listening, 2000);
        assert(sim.changes.size() == 2 && sim.Last() == VoiceFace::Thinking);
        assert(sim.changes[1].first - heard_at < kSettle + 60);  // dwell long over

        sim.Run(VoiceLink::Speaking, 6000);
        assert(sim.changes.size() == 3 && sim.Last() == VoiceFace::Speaking);
        sim.Run(VoiceLink::Listening, 4000);
        assert(sim.changes.size() == 4 && sim.Last() == VoiceFace::Listening);
        sim.Run(VoiceLink::Off, 2000);
        assert(sim.changes.size() == 5 && sim.Last() == VoiceFace::None);
        assert(sim.tracker.Shown() == VoiceFace::None);
    }

    // Fast reply: stt and "tts start" 200 ms apart. Thinking is only wanted
    // for 200 ms (< settle), so the pet goes straight to speaking.
    {
        Sim sim;
        sim.Run(VoiceLink::Listening, 3000);
        ++sim.heard;
        sim.Run(VoiceLink::Listening, 200);
        sim.Run(VoiceLink::Speaking, 3000);
        assert(sim.changes.size() == 2);
        assert(sim.changes[0].second == VoiceFace::Listening);
        assert(sim.changes[1].second == VoiceFace::Speaking);
    }

    // Minimum dwell: a face that settled is held at least kMinDwellMs even
    // when XiaoZhi moves on quickly; the newest wanted face follows after.
    {
        Sim sim;
        sim.Run(VoiceLink::Listening, 400);
        assert(sim.Last() == VoiceFace::Listening);
        const uint32_t listening_at = sim.changes.back().first;
        ++sim.heard;
        sim.Run(VoiceLink::Listening, 500);  // thinking wanted, dwell blocks
        assert(sim.changes.size() == 1);
        sim.Run(VoiceLink::Speaking, 2000);
        assert(sim.changes.size() == 2 && sim.Last() == VoiceFace::Speaking);
        assert(sim.changes[1].first - listening_at >= kDwell);
    }

    // A short Speaking -> Listening -> Speaking blip between two replies
    // (tts stop / start) never reaches the panel.
    {
        Sim sim;
        sim.Run(VoiceLink::Speaking, 3000);
        assert(sim.changes.size() == 1);
        sim.Run(VoiceLink::Listening, kSettle - 60);
        sim.Run(VoiceLink::Speaking, 3000);
        assert(sim.changes.size() == 1 && sim.Last() == VoiceFace::Speaking);
    }

    // No reply after a recognised sentence: thinking gives up and the pet
    // listens again. A new session never starts in "thinking".
    {
        Sim sim;
        sim.Run(VoiceLink::Listening, 2000);
        ++sim.heard;
        sim.Run(VoiceLink::Listening, VoiceFaceTracker::kThinkingTimeoutMs + 1000);
        assert(sim.changes.size() == 3);
        assert(sim.changes[1].second == VoiceFace::Thinking);
        assert(sim.changes[2].second == VoiceFace::Listening);
        assert(sim.changes[2].first - sim.changes[1].first >= VoiceFaceTracker::kThinkingTimeoutMs);

        ++sim.heard;
        sim.Run(VoiceLink::Listening, 2000);
        assert(sim.Last() == VoiceFace::Thinking);
        sim.Run(VoiceLink::Off, 2000);
        assert(sim.Last() == VoiceFace::None);
        sim.Run(VoiceLink::Listening, 2000);
        assert(sim.Last() == VoiceFace::Listening);
    }

    // Utterances counted outside listening (or before the first update)
    // do not trigger thinking later.
    {
        Sim sim;
        sim.heard = 41;
        sim.Run(VoiceLink::Off, 500);
        ++sim.heard;
        sim.Run(VoiceLink::Speaking, 2000);
        ++sim.heard;
        sim.Run(VoiceLink::Off, 2000);
        sim.Run(VoiceLink::Listening, 2000);
        assert(sim.Last() == VoiceFace::Listening);
        assert(sim.tracker.Wanted() == VoiceFace::Listening);
    }

    // Storm: XiaoZhi flipping every 20..700 ms for two minutes. Face changes
    // stay at least kMinDwellMs apart (so never more than one voice partial
    // per ~0.6 s refresh), and the last face follows the final state.
    {
        Sim sim;
        uint32_t rng = 12345;
        auto next = [&rng]() {
            rng = rng * 1664525u + 1013904223u;
            return rng >> 8;
        };
        for (int i = 0; i < 400; ++i) {
            const VoiceLink link = static_cast<VoiceLink>(next() % 3);
            if (next() % 4 == 0) ++sim.heard;
            sim.Run(link, 20 + (next() % 35) * 20);
        }
        assert(!sim.changes.empty());
        for (size_t i = 1; i < sim.changes.size(); ++i) {
            assert(sim.changes[i].first - sim.changes[i - 1].first >= kDwell);
            assert(sim.changes[i].second != sim.changes[i - 1].second);
        }
        sim.Run(VoiceLink::Off, 3000);
        assert(sim.tracker.Shown() == VoiceFace::None);
    }

    // The millisecond clock wraps (~49.7 days of uptime).
    {
        Sim sim(0xFFFFFFFFu - 700);
        sim.Run(VoiceLink::Listening, 1000);
        assert(sim.Last() == VoiceFace::Listening);
        ++sim.heard;
        sim.Run(VoiceLink::Listening, 2500);
        assert(sim.Last() == VoiceFace::Thinking);
        sim.Run(VoiceLink::Speaking, 2500);
        assert(sim.Last() == VoiceFace::Speaking);
        for (size_t i = 1; i < sim.changes.size(); ++i)
            assert(static_cast<uint32_t>(sim.changes[i].first - sim.changes[i - 1].first) >= kDwell);
    }

    std::cout << "PASS: voice faces follow XiaoZhi (listen/think/speak), settle, dwell, timeout, wrap\n";
    return 0;
}
