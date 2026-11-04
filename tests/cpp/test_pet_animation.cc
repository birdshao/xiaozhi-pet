#include "kid_app/pet_animation.h"

#include <cassert>
#include <iostream>

using namespace kid_app;
using Kind = DirectorStep::Kind;

namespace {

// Drives Idle() like the ePaper task: every frame step is "drawn" at once.
// Returns the poses drawn until the director goes quiet.
int Play(PetAnimationDirector& d, const SceneModel& displayed, uint32_t* now, ScenePose* out,
         int max_frames) {
    int n = 0;
    for (int tick = 0; tick < 400 && n < max_frames; ++tick, *now += 100) {
        const DirectorStep step = d.Idle(displayed, *now, true, false);
        if (step.kind == Kind::Frame) {
            out[n++] = step.scene.pose;
            *now += 620;  // partial refresh time
            d.FrameShown(*now);
            if (step.last) break;
        }
    }
    return n;
}

}  // namespace

int main() {
    const SceneModel happy{SceneKind::Home, Emotion::Happy, 1, 3};
    const SceneModel neutral{SceneKind::Home, Emotion::Neutral, 1, 1};

    // Every animation ends on the resting picture of its scene.
    for (Animation a : {Animation::Blink, Animation::Greet, Animation::Feed, Animation::SleepIn}) {
        const AnimationFrame* frames = nullptr;
        const uint8_t n = AnimationFrames(a, &frames);
        assert(n >= 2 && n <= 3 && frames[n - 1].pose == ScenePose::Still);
    }
    {
        const AnimationFrame* frames = nullptr;
        assert(AnimationFrames(Animation::None, &frames) == 0 && frames == nullptr);
    }

    // Feed: waits until the happy scene is on the panel, then hearts.
    {
        PetAnimationDirector d;
        uint32_t now = 1000;
        d.SceneShown(neutral, now);
        d.Request(Animation::Feed, happy, now);
        assert(d.Idle(neutral, now += 100, true, false).kind == Kind::None);  // not yet
        assert(!d.Active());
        d.SceneShown(happy, now += 620);
        ScenePose poses[4] = {};
        assert(Play(d, happy, &now, poses, 4) == 3);
        assert(poses[0] == ScenePose::Hearts1 && poses[1] == ScenePose::Hearts2 &&
               poses[2] == ScenePose::Still);
        assert(!d.Active());
        // Frames keep the highlight of the displayed scene.
        d.Request(Animation::Feed, happy, now);
        SceneModel moved = happy;
        moved.home_selection = 4;
        const DirectorStep step = d.Idle(moved, now += 100, true, false);
        assert(step.kind == Kind::Frame && step.scene.home_selection == 4 &&
               step.scene.pose == ScenePose::Hearts1 && step.animation == Animation::Feed);
    }

    // Home animations never play on the reader or the sleep scene.
    {
        PetAnimationDirector d;
        uint32_t now = 0;
        const SceneModel reader{SceneKind::Reader, Emotion::Happy, 2, 1};
        const SceneModel sleep{SceneKind::Sleep, Emotion::Sleepy, 2, 1};
        d.SceneShown(reader, now);
        d.Request(Animation::Greet, reader, now);
        d.Request(Animation::SleepIn, happy, now);
        assert(d.Idle(reader, now += 100, true, false).kind == Kind::None);
        d.Request(Animation::Feed, sleep, now);
        d.SceneShown(sleep, now);
        assert(d.Idle(sleep, now += 100, false, false).kind == Kind::None);
    }

    // A request whose scene never appears expires; a new base cancels.
    {
        PetAnimationDirector d;
        uint32_t now = 0;
        d.SceneShown(neutral, now);
        d.Request(Animation::Feed, happy, now);
        now += PetAnimationDirector::kRequestTimeoutMs + 1;
        d.SceneShown(happy, now);
        assert(d.Idle(happy, now += 100, true, false).kind == Kind::None);
        d.Request(Animation::Greet, happy, now);
        assert(d.Idle(happy, now += 100, true, false).kind == Kind::Frame);
        d.FrameShown(now += 620);
        assert(d.Active());
        const SceneModel reader{SceneKind::Reader, Emotion::Happy, 1, 1};
        d.SceneShown(reader, now += 100);
        assert(!d.Active());
        assert(d.Idle(reader, now += 1000, true, false).kind == Kind::None);
    }

    // Sleep: starts from Zzz1 and runs even without ambient permission.
    {
        PetAnimationDirector d;
        uint32_t now = 500;
        const SceneModel zzz1{SceneKind::Sleep, Emotion::Sleepy, 3, 1, ScenePose::Zzz1};
        d.Request(Animation::SleepIn, zzz1, now);
        d.SceneShown(zzz1, now += 4300);
        DirectorStep step = d.Idle(zzz1, now += 100, false, true);
        assert(step.kind == Kind::Frame && step.scene.pose == ScenePose::Zzz2 && !step.last);
        d.FrameShown(now += 620);
        assert(d.Idle(zzz1, now += 100, false, true).kind == Kind::None);  // 300 ms hold
        step = d.Idle(zzz1, now += 300, false, true);
        assert(step.kind == Kind::Frame && step.scene.pose == ScenePose::Still && step.last &&
               step.scene.kind == SceneKind::Sleep);
        d.FrameShown(now += 620);
        assert(!d.Active());
        // No blink or cleanup while not ambient (asleep).
        assert(d.Idle(zzz1, now += 200000, false, true).kind == Kind::None);
    }

    // Idle blink: only on the resting home picture, 45..90 s after the last
    // frame; cleanup waits 20 s of quiet and goes before a blink.
    {
        PetAnimationDirector d;
        uint32_t now = 10000;
        d.SceneShown(neutral, now);
        assert(d.Idle(neutral, now + PetAnimationDirector::kBlinkMinMs - 1, true, false).kind ==
               Kind::None);
        assert(d.Idle(neutral, now + PetAnimationDirector::kCleanupIdleMs - 1, true, true).kind ==
               Kind::None);
        DirectorStep step = d.Idle(neutral, now + PetAnimationDirector::kCleanupIdleMs, true, true);
        assert(step.kind == Kind::Cleanup && step.scene.home_selection == 1);
        now += PetAnimationDirector::kCleanupIdleMs;
        d.SceneShown(neutral, now += 4300);  // cleanup drawn
        uint32_t blink_at = 0;
        for (uint32_t t = now; t < now + 100000; t += 100) {
            step = d.Idle(neutral, t, true, false);
            if (step.kind != Kind::None) {
                blink_at = t;
                break;
            }
        }
        assert(step.kind == Kind::Frame && step.animation == Animation::Blink &&
               step.scene.pose == ScenePose::Blink);
        assert(blink_at - now >= PetAnimationDirector::kBlinkMinMs &&
               blink_at - now < PetAnimationDirector::kBlinkMinMs + PetAnimationDirector::kBlinkSpanMs);
        d.FrameShown(blink_at + 620);
        step = d.Idle(neutral, blink_at + 720, true, false);
        assert(step.kind == Kind::Frame && step.scene.pose == ScenePose::Still && step.last);
        d.FrameShown(blink_at + 1340);
        assert(!d.Active());
        // Not on the reader, not on a non-resting picture, not when not ambient.
        const SceneModel reader{SceneKind::Reader, Emotion::Neutral, 1, 1};
        d.SceneShown(reader, blink_at + 2000);
        assert(d.Idle(reader, blink_at + 200000, true, false).kind == Kind::None);
        d.SceneShown(neutral, blink_at + 300000);
        assert(d.Idle(neutral, blink_at + 500000, false, false).kind == Kind::None);
        // Reset (pet mode left) drops pending requests.
        d.Request(Animation::Feed, neutral, blink_at + 500000);
        d.Reset(blink_at + 500000);
        const DirectorStep after = d.Idle(neutral, blink_at + 500100, true, false);
        assert(after.kind == Kind::None);
    }

    // A failed frame jumps to the resting frame (after 1 s), never leaving
    // a pose on the panel; a failed resting frame ends the animation.
    {
        PetAnimationDirector d;
        uint32_t now = 100;
        d.SceneShown(happy, now);
        d.Request(Animation::Feed, happy, now);
        DirectorStep step = d.Idle(happy, now += 100, true, false);
        assert(step.kind == Kind::Frame && step.scene.pose == ScenePose::Hearts1);
        d.FrameShown(now += 620, false);
        assert(d.Idle(happy, now += 500, true, false).kind == Kind::None);
        step = d.Idle(happy, now += 600, true, false);
        assert(step.kind == Kind::Frame && step.scene.pose == ScenePose::Still && step.last);
        d.FrameShown(now += 620, false);
        assert(!d.Active());
    }

    // Wrap-safe timers around the 32-bit millisecond rollover.
    {
        PetAnimationDirector d;
        uint32_t now = 0xFFFFFF00u;
        d.SceneShown(happy, now);
        d.Request(Animation::Greet, happy, now);
        ScenePose poses[3] = {};
        assert(Play(d, happy, &now, poses, 3) == 2);
        assert(poses[0] == ScenePose::Wave && poses[1] == ScenePose::Still);
    }

    std::cout << "PASS: pet animations, idle blink and deferred cleanup\n";
}
