#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace kid_app {

// Pure priority/arbitration model, not an audio codec or peripheral driver.
// Single-owner task only; hardware-specific AudioPort executes returned plan.
enum class AudioStream : uint8_t { None, PetCue, Story, Voice };
enum class AudioVerb : uint8_t { Start, Stop, Resume };

struct AudioInstruction {
    AudioVerb verb = AudioVerb::Start;
    AudioStream stream = AudioStream::None;
    uint64_t token = 0;  // Unique source playback/session token.
};

struct AudioPlan {
    bool accepted = false;
    std::array<AudioInstruction, 2> actions{};
    size_t count = 0;
};

// Priority: Voice > Story > PetCue. Quiet mode overrides everything.
// Do not emit pet reward sound on late/stale events while voice is playing.
class AudioCoordinator {
public:
    AudioPlan PlayCue(uint64_t token);
    AudioPlan StartStory(uint64_t token);
    // V8: a lullaby (Story-class stream) is the one sound allowed in quiet
    // hours; the caller caps its volume (SongPlayer::VolumePct). Voice still
    // wins, and SetQuiet(true) leaves a running lullaby alone.
    AudioPlan StartLullaby(uint64_t token);
    AudioPlan BeginVoice(uint64_t token);
    AudioPlan EndVoice(uint64_t token);
    AudioPlan Finished(uint64_t token);
    AudioPlan SetQuiet(bool enabled);
    // V8: PWR (sleep flow) and leaving a sound activity silence whatever is
    // playing, voice included, without touching the quiet flag and without
    // resuming a paused story afterwards.
    AudioPlan StopAll();

    AudioStream Active() const { return active_; }
    uint64_t ActiveToken() const { return active_token_; }
    bool Quiet() const { return quiet_; }

private:
    static void Add(AudioPlan* plan, AudioVerb verb, AudioStream stream, uint64_t token);
    AudioStream active_ = AudioStream::None;
    uint64_t active_token_ = 0;
    uint64_t paused_story_ = 0;
    bool lullaby_ = false;  // the active Story-class stream is a lullaby
    bool quiet_ = false;
};

}  // namespace kid_app
