#include "kid_app/pet_animation.h"

#include <cassert>
#include <string>
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
    for (Animation a : {Animation::Blink, Animation::Greet, Animation::Feed, Animation::SleepIn,
                        Animation::Giggle, Animation::Dizzy, Animation::Charge,
                        Animation::Yawn, Animation::TipHot, Animation::TipCold}) {
        const AnimationFrame* frames = nullptr;
        const uint8_t n = AnimationFrames(a, &frames);
        assert(n >= 2 && n <= 3 && frames[n - 1].pose == ScenePose::Still);
    }
    {
        const AnimationFrame* frames = nullptr;
        assert(AnimationFrames(Animation::None, &frames) == 0 && frames == nullptr);
        // Good morning ends on the morning picture, not the plain home.
        assert(AnimationFrames(Animation::Morning, &frames) == 2);
        assert(frames[0].pose == ScenePose::Sunrise && frames[0].hold_ms >= 1000 &&
               frames[1].pose == ScenePose::Morning);
        assert(std::string(AnimationName(Animation::Morning)) == "morning");
        // V3-05: one long-held battery frame each, then the resting picture.
        assert(AnimationFrames(Animation::Charge, &frames) == 2);
        assert(frames[0].pose == ScenePose::Charging && frames[0].hold_ms >= 2000);
        assert(AnimationFrames(Animation::Yawn, &frames) == 2);
        assert(frames[0].pose == ScenePose::Yawn && frames[0].hold_ms >= 3000);
        assert(std::string(AnimationName(Animation::Charge)) == "charge");
        assert(std::string(AnimationName(Animation::Yawn)) == "yawn");
        // V4-03: a life-tip bubble held long enough to read, then rest.
        assert(AnimationFrames(Animation::TipHot, &frames) == 2);
        assert(frames[0].pose == ScenePose::TipHot && frames[0].hold_ms >= 4000);
        assert(AnimationFrames(Animation::TipCold, &frames) == 2);
        assert(frames[0].pose == ScenePose::TipCold && frames[0].hold_ms >= 4000);
        assert(std::string(AnimationName(Animation::TipHot)) == "tip_hot");
        assert(std::string(AnimationName(Animation::TipCold)) == "tip_cold");
    }

    // Battery reactions play on the home scene only (never on the reader
    // or the sleeping picture) and end back at rest.
    {
        PetAnimationDirector d;
        uint32_t now = 0;
        d.SceneShown(neutral, now);
        d.Request(Animation::Yawn, neutral, now);
        ScenePose poses[4] = {};
        assert(Play(d, neutral, &now, poses, 4) == 2);
        assert(poses[0] == ScenePose::Yawn && poses[1] == ScenePose::Still && !d.Active());
        const SceneModel reader{SceneKind::Reader, Emotion::Neutral, 3, 1};
        d.Request(Animation::Charge, reader, now);
        d.SceneShown(reader, now += 100);
        assert(d.Idle(reader, now += 100, true, false).kind == Kind::None);
        const SceneModel sleep{SceneKind::Sleep, Emotion::Sleepy, 1, 1};
        d.Request(Animation::Yawn, sleep, now);
        d.SceneShown(sleep, now += 100);
        assert(d.Idle(sleep, now += 100, true, false).kind == Kind::None);
    }

    // Morning (V3-04): plays once the Sunrise home scene is on the panel,
    // keeps the highlight, then no idle blink on the morning picture.
    {
        PetAnimationDirector d;
        uint32_t now = 0;
        const SceneModel sunrise{SceneKind::Home, Emotion::Happy, 1, 1, ScenePose::Sunrise};
        d.Request(Animation::Morning, sunrise, now);
        assert(d.Idle(neutral, now += 100, true, false).kind == Kind::None);
        d.SceneShown(sunrise, now += 4300);
        ScenePose poses[4] = {};
        assert(Play(d, sunrise, &now, poses, 4) == 2);
        assert(poses[0] == ScenePose::Sunrise && poses[1] == ScenePose::Morning);
        assert(!d.Active());
        assert(d.Idle(sunrise, now += 200000, true, false).kind == Kind::None);
        // Not on the sleep scene.
        const SceneModel sleep{SceneKind::Sleep, Emotion::Sleepy, 1, 1, ScenePose::Sunrise};
        d.Request(Animation::Morning, sleep, now);
        d.SceneShown(sleep, now += 100);
        assert(d.Idle(sleep, now += 100, true, false).kind == Kind::None);
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
        // ... and the status band: a new clock value is not a new base, so
        // it neither cancels the animation nor blocks a pending request.
        d.FrameShown(now += 620);
        SceneModel ticked = moved;
        ticked.status.hour = 7;
        ticked.status.minute = 43;
        const DirectorStep next = d.Idle(ticked, now += 300, true, false);
        assert(next.kind == Kind::Frame && next.scene.pose == ScenePose::Hearts2 &&
               next.scene.status == ticked.status && d.Active());
        PetAnimationDirector fresh;
        fresh.SceneShown(happy, now);
        fresh.Request(Animation::Greet, happy, now);
        assert(fresh.Idle(ticked, now += 100, true, false).scene.pose == ScenePose::Wave);
    }

    // Shake reactions play on the home screen: giggle, then dizzy.
    {
        PetAnimationDirector d;
        uint32_t now = 50;
        d.SceneShown(neutral, now);
        d.Request(Animation::Giggle, neutral, now);
        ScenePose poses[4] = {};
        assert(Play(d, neutral, &now, poses, 4) == 2);
        assert(poses[0] == ScenePose::Giggle && poses[1] == ScenePose::Still);
        d.Request(Animation::Dizzy, neutral, now);
        assert(Play(d, neutral, &now, poses, 4) == 3);
        assert(poses[0] == ScenePose::Dizzy1 && poses[1] == ScenePose::Dizzy2 &&
               poses[2] == ScenePose::Still);
        assert(std::string(AnimationName(Animation::Dizzy)) == "dizzy");
        const SceneModel reader{SceneKind::Reader, Emotion::Happy, 2, 1};
        d.Request(Animation::Dizzy, reader, now);  // ignored off the home screen
        d.SceneShown(reader, now += 100);
        assert(d.Idle(reader, now += 100, true, false).kind == Kind::None);
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
