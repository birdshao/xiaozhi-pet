#include "kid_app/audio_coordinator.h"

namespace kid_app {

void AudioCoordinator::Add(AudioPlan* plan, AudioVerb verb,
                           AudioStream stream, uint64_t token) {
    if (plan->count < plan->actions.size()) {
        plan->actions[plan->count++] = {verb, stream, token};
    }
}

AudioPlan AudioCoordinator::PlayCue(uint64_t token) {
    AudioPlan plan{};
    if (!token || quiet_ || active_ != AudioStream::None) return plan;
    active_ = AudioStream::PetCue;
    active_token_ = token;
    plan.accepted = true;
    Add(&plan, AudioVerb::Start, active_, token);
    return plan;
}

AudioPlan AudioCoordinator::StartStory(uint64_t token) {
    AudioPlan plan{};
    if (!token || quiet_ || active_ == AudioStream::Voice) return plan;
    plan.accepted = true;
    if (active_ == AudioStream::Story && active_token_ == token) return plan;
    if (active_ != AudioStream::None)
        Add(&plan, AudioVerb::Stop, active_, active_token_);
    active_ = AudioStream::Story;
    active_token_ = token;
    paused_story_ = 0;
    Add(&plan, AudioVerb::Start, active_, token);
    return plan;
}

AudioPlan AudioCoordinator::BeginVoice(uint64_t token) {
    AudioPlan plan{};
    if (!token || quiet_) return plan;
    if (active_ == AudioStream::Voice) {
        plan.accepted = (active_token_ == token); // no duplicate playback.
        return plan;
    }
    plan.accepted = true;
    paused_story_ = active_ == AudioStream::Story ? active_token_ : 0;
    if (active_ != AudioStream::None)
        Add(&plan, AudioVerb::Stop, active_, active_token_);
    active_ = AudioStream::Voice;
    active_token_ = token;
    Add(&plan, AudioVerb::Start, active_, token);
    return plan;
}

AudioPlan AudioCoordinator::EndVoice(uint64_t token) {
    AudioPlan plan{};
    if (active_ != AudioStream::Voice || token != active_token_ || token == 0)
        return plan;
    plan.accepted = true;
    Add(&plan, AudioVerb::Stop, AudioStream::Voice, active_token_);
    if (paused_story_ != 0) {
        active_ = AudioStream::Story;
        active_token_ = paused_story_;
        paused_story_ = 0;
        Add(&plan, AudioVerb::Resume, active_, active_token_);
    } else {
        active_ = AudioStream::None;
        active_token_ = 0;
    }
    return plan;
}

AudioPlan AudioCoordinator::Finished(uint64_t token) {
    AudioPlan plan{};
    if (active_ == AudioStream::None || active_token_ != token || token == 0)
        return plan; // ignore late completion from preempted stream.
    plan.accepted = true;
    active_ = AudioStream::None;
    active_token_ = 0;
    paused_story_ = 0;
    return plan;
}

AudioPlan AudioCoordinator::SetQuiet(bool enabled) {
    AudioPlan plan{};
    plan.accepted = true;
    if (quiet_ == enabled) return plan;
    quiet_ = enabled;
    if (enabled) {
        if (active_ != AudioStream::None)
            Add(&plan, AudioVerb::Stop, active_, active_token_);
        active_ = AudioStream::None;
        active_token_ = 0;
        paused_story_ = 0; // Do not silently resume audio after quiet ends.
    }
    return plan;
}

}  // namespace kid_app
