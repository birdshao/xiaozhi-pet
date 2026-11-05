#pragma once

#include "kid_app/scene_model.h"

#include <cstdint>

namespace kid_app {

// Short ePaper animations: every frame is one partial refresh (~0.62 s on
// V1 hardware), so each runs 2-3 frames and ends on the resting picture.
// Giggle: gentle shake. Dizzy: hard shake (a reaction, never a reward).
// Morning (V3-04): from a Sunrise home scene, hold the stretch, then the
// sun is up and TuanTuan waves; it ends on ScenePose::Morning, the picture
// kept until the child touches the dial or the pet goes back to sleep.
// Charge / Yawn (V3-05): plugged in, TuanTuan eats electricity; on a low
// battery it yawns and shows the "charge me" bubble for a while.
// TipHot / TipCold (V4-03): a drink / coat bubble while the pet says a life tip.
enum class Animation : uint8_t {
    None, Blink, Greet, Feed, SleepIn, Giggle, Dizzy, Morning, Charge, Yawn,
    TipHot, TipCold
};

struct AnimationFrame {
    ScenePose pose;
    uint16_t hold_ms;  // wait after this frame is on the panel
};

// Frames shown after the base scene; the last one is the resting pose
// (Still, except Morning). SleepIn starts from a Sleep scene shown with
// ScenePose::Zzz1.
// Returns the frame count (0 for None).
uint8_t AnimationFrames(Animation animation, const AnimationFrame** frames);
const char* AnimationName(Animation animation);

// What the ePaper task should draw while it has nothing else to do.
struct DirectorStep {
    enum class Kind : uint8_t { None, Frame, Cleanup };
    Kind kind = Kind::None;
    SceneModel scene{};
    Animation animation = Animation::None;  // Frame only
    bool last = false;                       // Frame only: back at rest
};

// Plays requested animations, idle blinks and the deferred ghosting cleanup.
// Pure logic for the single ePaper task; the board serializes Request()
// from other tasks with a mutex. Timestamps are milliseconds (wrap-safe).
class PetAnimationDirector {
public:
    static constexpr uint32_t kRequestTimeoutMs = 10000;  // target never shown
    static constexpr uint32_t kCleanupIdleMs = 20000;
    static constexpr uint32_t kBlinkMinMs = 45000;
    static constexpr uint32_t kBlinkSpanMs = 45000;  // 45..90 s apart

    // Play `animation` once `target` (kind, emotion, page) is on the panel.
    // A newer request replaces an older one that has not started. SleepIn
    // needs a Sleep target, the others a Home one (on the reader every
    // frame would be a full refresh); other requests are ignored.
    void Request(Animation animation, const SceneModel& target, uint32_t now_ms);
    // The gate drew a new base scene (also after a Cleanup step).
    void SceneShown(const SceneModel& scene, uint32_t now_ms);
    // A Frame step was drawn. ok=false (EPD error): skip to the resting
    // frame 1 s later so the panel never stays on a pose the gate does not
    // know about; a failed resting frame ends the animation.
    void FrameShown(uint32_t now_ms, bool ok = true);
    // displayed: the gate's base scene. ambient: idle blink and cleanup are
    // allowed (Pet home, awake, voice idle). needs_cleanup: planner budget.
    DirectorStep Idle(const SceneModel& displayed, uint32_t now_ms, bool ambient,
                      bool needs_cleanup);
    // Pet mode left or the panel was taken over: forget everything.
    void Reset(uint32_t now_ms);

    bool Active() const { return active_ != Animation::None; }
    Animation Playing() const { return active_; }

private:
    static bool SameBase(const SceneModel& a, const SceneModel& b);
    static bool Reached(uint32_t now_ms, uint32_t at_ms) {
        return static_cast<int32_t>(now_ms - at_ms) >= 0;
    }
    void ScheduleBlink(uint32_t now_ms);

    Animation pending_ = Animation::None;
    SceneModel pending_target_{};
    uint32_t pending_since_ms_ = 0;

    Animation active_ = Animation::None;
    SceneModel base_{};
    uint8_t index_ = 0;
    uint32_t next_frame_ms_ = 0;

    bool started_ = false;  // timers below are valid
    uint32_t last_shown_ms_ = 0;
    uint32_t next_blink_ms_ = 0;
    uint32_t rng_ = 0x2545F491u;
};

}  // namespace kid_app
