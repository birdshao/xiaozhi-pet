#pragma once

#include "kid_app/scene_model.h"

#include <cstdint>

namespace kid_app {

// V6-01: TuanTuan's face while the child talks to XiaoZhi (BOOT).
//
// What XiaoZhi reports, reduced to three link states (the board maps
// kDeviceStateConnecting/Listening to Listening, Speaking to Speaking and
// everything else to Off). "Thinking" is not a XiaoZhi state: it is the
// stretch between the recognised sentence (the server's "stt" message) and
// the first reply audio ("tts start"), so the tracker also gets a counter of
// recognised utterances.
enum class VoiceLink : uint8_t { Off, Listening, Speaking };
enum class VoiceFace : uint8_t { None, Listening, Thinking, Speaking };

const char* VoiceFaceName(VoiceFace face);

// The voice face is a UI-only overlay on the home picture (like the rail
// highlight): no NVS, never part of PetSnapshot. Home scenes get the face as
// their emotion; reader and sleep scenes are returned unchanged. None leaves
// the scene as it is, so the normal home picture comes back on its own.
SceneModel ApplyVoiceFace(SceneModel scene, VoiceFace face);

// Turns fast-flipping XiaoZhi states into a calm sequence of faces for the
// ePaper (each face change is one ~0.62 s partial refresh):
//  - a new face must be wanted for kSettleMs without interruption (a
//    Speaking -> Listening -> Speaking blip between two replies is ignored);
//  - two face changes are at least kMinDwellMs apart, so the pet area never
//    gets more than one voice partial per kMinDwellMs;
//  - Thinking ends when the reply starts, the link drops, or after
//    kThinkingTimeoutMs without a reply (back to Listening).
// Single owner (the board's pet_input task); times are wrap-safe ms.
class VoiceFaceTracker {
public:
    static constexpr uint32_t kSettleMs = 300;
    static constexpr uint32_t kMinDwellMs = 1500;
    static constexpr uint32_t kThinkingTimeoutMs = 12000;

    // Call often (every input loop pass). heard is a free-running count of
    // recognised utterances (wraps). Returns true when the face to show
    // changed; *face is then the new one.
    bool Update(VoiceLink link, uint32_t heard, uint32_t now_ms, VoiceFace* face);
    VoiceFace Shown() const { return shown_; }
    // Wanted right now, before settling and dwell (diagnostics/tests).
    VoiceFace Wanted() const { return wanted_; }

private:
    static bool Elapsed(uint32_t now_ms, uint32_t since_ms, uint32_t span_ms) {
        return static_cast<uint32_t>(now_ms - since_ms) >= span_ms;
    }

    bool started_ = false;
    VoiceLink link_ = VoiceLink::Off;
    uint32_t heard_ = 0;
    bool thinking_ = false;
    uint32_t thinking_since_ms_ = 0;

    VoiceFace wanted_ = VoiceFace::None;
    uint32_t wanted_since_ms_ = 0;
    VoiceFace shown_ = VoiceFace::None;
    bool changed_once_ = false;
    uint32_t shown_since_ms_ = 0;
};

}  // namespace kid_app
