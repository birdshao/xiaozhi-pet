#include "kid_app/voice_face.h"

namespace kid_app {

const char* VoiceFaceName(VoiceFace face) {
    switch (face) {
        case VoiceFace::Listening: return "listening";
        case VoiceFace::Thinking: return "thinking";
        case VoiceFace::Speaking: return "speaking";
        case VoiceFace::None: break;
    }
    return "none";
}

SceneModel ApplyVoiceFace(SceneModel scene, VoiceFace face) {
    if (scene.kind != SceneKind::Home) return scene;
    switch (face) {
        case VoiceFace::Listening: scene.emotion = Emotion::Listening; break;
        case VoiceFace::Thinking: scene.emotion = Emotion::Thinking; break;
        case VoiceFace::Speaking: scene.emotion = Emotion::Speaking; break;
        case VoiceFace::None: break;
    }
    return scene;
}

bool VoiceFaceTracker::Update(VoiceLink link, uint32_t heard, uint32_t now_ms,
                              VoiceFace* face) {
    if (!started_) {
        started_ = true;
        heard_ = heard;
        wanted_since_ms_ = now_ms;
    }
    // A new turn starts listening afresh; a sentence heard while listening
    // means XiaoZhi is working on the answer.
    if (link != VoiceLink::Listening) thinking_ = false;
    if (link == VoiceLink::Listening && link_ != VoiceLink::Listening) thinking_ = false;
    if (link == VoiceLink::Listening && heard != heard_) {
        thinking_ = true;
        thinking_since_ms_ = now_ms;
    }
    if (thinking_ && Elapsed(now_ms, thinking_since_ms_, kThinkingTimeoutMs)) thinking_ = false;
    link_ = link;
    heard_ = heard;

    VoiceFace want = VoiceFace::None;
    if (link == VoiceLink::Speaking) want = VoiceFace::Speaking;
    else if (link == VoiceLink::Listening) want = thinking_ ? VoiceFace::Thinking : VoiceFace::Listening;
    if (want != wanted_) {
        wanted_ = want;
        wanted_since_ms_ = now_ms;
    }

    if (wanted_ == shown_ || !Elapsed(now_ms, wanted_since_ms_, kSettleMs)) return false;
    if (changed_once_ && !Elapsed(now_ms, shown_since_ms_, kMinDwellMs)) return false;
    shown_ = wanted_;
    shown_since_ms_ = now_ms;
    changed_once_ = true;
    if (face) *face = shown_;
    return true;
}

}  // namespace kid_app
