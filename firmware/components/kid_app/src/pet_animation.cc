#include "kid_app/pet_animation.h"

#include <cstddef>

namespace kid_app {
namespace {

constexpr AnimationFrame kBlink[] = {{ScenePose::Blink, 0}, {ScenePose::Still, 0}};
constexpr AnimationFrame kGreet[] = {{ScenePose::Wave, 400}, {ScenePose::Still, 0}};
constexpr AnimationFrame kFeed[] = {
    {ScenePose::Hearts1, 200}, {ScenePose::Hearts2, 700}, {ScenePose::Still, 0}};
constexpr AnimationFrame kSleepIn[] = {{ScenePose::Zzz2, 300}, {ScenePose::Still, 0}};
constexpr AnimationFrame kGiggle[] = {{ScenePose::Giggle, 600}, {ScenePose::Still, 0}};
constexpr AnimationFrame kDizzy[] = {
    {ScenePose::Dizzy1, 200}, {ScenePose::Dizzy2, 500}, {ScenePose::Still, 0}};

template <size_t N>
uint8_t Use(const AnimationFrame (&table)[N], const AnimationFrame** frames) {
    *frames = table;
    return static_cast<uint8_t>(N);
}

}  // namespace

uint8_t AnimationFrames(Animation animation, const AnimationFrame** frames) {
    switch (animation) {
        case Animation::Blink: return Use(kBlink, frames);
        case Animation::Greet: return Use(kGreet, frames);
        case Animation::Feed: return Use(kFeed, frames);
        case Animation::SleepIn: return Use(kSleepIn, frames);
        case Animation::Giggle: return Use(kGiggle, frames);
        case Animation::Dizzy: return Use(kDizzy, frames);
        case Animation::None: break;
    }
    *frames = nullptr;
    return 0;
}

const char* AnimationName(Animation animation) {
    switch (animation) {
        case Animation::Blink: return "blink";
        case Animation::Greet: return "greet";
        case Animation::Feed: return "feed";
        case Animation::SleepIn: return "sleep_in";
        case Animation::Giggle: return "giggle";
        case Animation::Dizzy: return "dizzy";
        case Animation::None: break;
    }
    return "none";
}

bool PetAnimationDirector::SameBase(const SceneModel& a, const SceneModel& b) {
    // The highlight and the status band may change during an animation;
    // frames follow them.
    return a.kind == b.kind && a.emotion == b.emotion && a.book_page == b.book_page &&
           a.pose == b.pose;
}

void PetAnimationDirector::ScheduleBlink(uint32_t now_ms) {
    rng_ = rng_ * 1664525u + 1013904223u;  // deterministic jitter
    next_blink_ms_ = now_ms + kBlinkMinMs + (rng_ >> 8) % kBlinkSpanMs;
}

void PetAnimationDirector::Request(Animation animation, const SceneModel& target,
                                   uint32_t now_ms) {
    const SceneKind needs = animation == Animation::SleepIn ? SceneKind::Sleep : SceneKind::Home;
    if (animation == Animation::None || target.kind != needs) return;
    pending_ = animation;
    pending_target_ = target;
    pending_since_ms_ = now_ms;
}

void PetAnimationDirector::SceneShown(const SceneModel& scene, uint32_t now_ms) {
    started_ = true;
    last_shown_ms_ = now_ms;
    ScheduleBlink(now_ms);
    // A new base picture replaces whatever pose was on the panel.
    if (active_ != Animation::None && !SameBase(scene, base_)) active_ = Animation::None;
}

void PetAnimationDirector::FrameShown(uint32_t now_ms, bool ok) {
    last_shown_ms_ = now_ms;
    if (active_ == Animation::None) return;
    const AnimationFrame* frames = nullptr;
    const uint8_t count = AnimationFrames(active_, &frames);
    if (!ok && index_ + 1 < count) {
        index_ = static_cast<uint8_t>(count - 1);
        next_frame_ms_ = now_ms + 1000;
        return;
    }
    const uint16_t hold = index_ < count ? frames[index_].hold_ms : 0;
    if (++index_ >= count) {
        active_ = Animation::None;
        ScheduleBlink(now_ms);
        return;
    }
    next_frame_ms_ = now_ms + hold;
}

DirectorStep PetAnimationDirector::Idle(const SceneModel& displayed, uint32_t now_ms,
                                        bool ambient, bool needs_cleanup) {
    DirectorStep step{};
    if (!started_) {  // first base scene not reported yet
        started_ = true;
        last_shown_ms_ = now_ms;
        ScheduleBlink(now_ms);
    }
    if (pending_ != Animation::None &&
        static_cast<uint32_t>(now_ms - pending_since_ms_) > kRequestTimeoutMs)
        pending_ = Animation::None;
    if (pending_ != Animation::None && SameBase(displayed, pending_target_)) {
        active_ = pending_;  // replaces a running one
        pending_ = Animation::None;
        base_ = displayed;
        index_ = 0;
        next_frame_ms_ = now_ms;
    }
    if (active_ != Animation::None && !SameBase(displayed, base_)) active_ = Animation::None;

    if (active_ != Animation::None) {
        if (!Reached(now_ms, next_frame_ms_)) return step;
        const AnimationFrame* frames = nullptr;
        const uint8_t count = AnimationFrames(active_, &frames);
        step.kind = DirectorStep::Kind::Frame;
        step.scene = displayed;
        step.scene.pose = frames[index_].pose;
        step.animation = active_;
        step.last = index_ + 1 >= count;
        return step;
    }
    if (pending_ != Animation::None || !ambient) return step;

    if (needs_cleanup && static_cast<uint32_t>(now_ms - last_shown_ms_) >= kCleanupIdleMs) {
        step.kind = DirectorStep::Kind::Cleanup;
        step.scene = displayed;
        return step;
    }
    if (displayed.kind == SceneKind::Home && displayed.pose == ScenePose::Still &&
        Reached(now_ms, next_blink_ms_)) {
        active_ = Animation::Blink;
        base_ = displayed;
        index_ = 0;
        step.kind = DirectorStep::Kind::Frame;
        step.scene = displayed;
        step.scene.pose = ScenePose::Blink;
        step.animation = Animation::Blink;
        return step;
    }
    return step;
}

void PetAnimationDirector::Reset(uint32_t now_ms) {
    pending_ = Animation::None;
    active_ = Animation::None;
    started_ = true;
    last_shown_ms_ = now_ms;
    ScheduleBlink(now_ms);
}

}  // namespace kid_app
