"""Pinned XiaoZhi AudioService patch: a raw microphone frame tap for the parrot (V8).

Applied AFTER scripts/v4_mic_tap_patch.py (it anchors on nothing of that
patch except the line that clears the loudness tap on a voice session).

The parrot (kid_app/parrot_session.h) records at most 5 s of the child's
voice into one RAM buffer owned by the board, plays it back higher, and wipes
it. The tap hands the sink ONE 10 ms mono frame at a time, as a pointer that
is valid only during the call. PRIVACY (ADR-011, PRD section 2 item 7):

* the patch never copies a frame anywhere itself, queues, encodes, stores or
  sends it: the sink (the parrot buffer on the board) is the only consumer;
* AudioInputTask stays the only reader of the codec (no second I2S owner);
* voice processing switching on clears the tap bit, so the microphone is
  never shared with a voice session (the board also ends the activity);
* the board arms the tap only while the child holds the dial in the parrot.

tests/test_v8_parrot_privacy.py applies it to fixtures and to the real pinned
source when available, and checks the added text for buffer, file or network
use.
"""
from __future__ import annotations

from scripts.v4_mic_tap_patch import once

# Everything this patch adds to audio_service.h / .cc, for the privacy test.
FRAME_HELPER = r"""
// V8 parrot: channel 0 of one 10 ms input frame, handed to the sink as a
// pointer that is valid only during the call. Nothing is kept here.
static void MicFrameDeliver(const std::vector<int16_t>& data, int channels,
                            AudioService::MicFrameSink sink, void* context) {
    if (channels < 1 || data.empty() || !sink) return;
    int16_t mono[160];
    size_t n = 0;
    for (size_t i = 0; i < data.size() && n < 160; i += channels) mono[n++] = data[i];
    sink(context, mono, n);
}

void AudioService::SetMicFrameTap(bool enable, MicFrameSink sink, void* context) {
    if (enable && sink) {
        mic_frame_sink_ = sink;
        mic_frame_context_ = context;
        xEventGroupSetBits(event_group_, AS_EVENT_MIC_FRAMES);
    } else {
        xEventGroupClearBits(event_group_, AS_EVENT_MIC_FRAMES);
    }
}
"""

TAP_FRAMES_ALONE = (
    "        /* V8 parrot: raw 10 ms mono frames, only while the board armed the tap. */\n"
    "        if ((bits & AS_EVENT_MIC_FRAMES) && mic_frame_sink_) {\n"
    "            std::vector<int16_t> data;\n"
    "            if (ReadAudioData(data, 16000, 160)) {\n"
    "                MicFrameDeliver(data, codec_->input_channels(), mic_frame_sink_,\n"
    "                                mic_frame_context_);\n"
    "                continue;\n"
    "            }\n"
    "        }\n\n"
)

ADDED_TEXT = (FRAME_HELPER, TAP_FRAMES_ALONE)


def _fresh(source: str, label: str) -> None:
    if "MIC_FRAMES" in source or "MicFrameDeliver" in source:
        raise RuntimeError(f"{label}: expected 1 anchor; already patched")


def patch_audio_h(source: str) -> str:
    _fresh(source, "AudioService header")
    source = once(source,
        "#define AS_EVENT_MIC_ENVELOPE (1 << 5)\n",
        "#define AS_EVENT_MIC_ENVELOPE (1 << 5)\n"
        "#define AS_EVENT_MIC_FRAMES (1 << 6)\n",
        "AudioService frame event bit")
    source = once(source,
        "    void SetMicEnvelopeTap(bool enable, MicEnvelopeSink sink, void* context);\n",
        "    void SetMicEnvelopeTap(bool enable, MicEnvelopeSink sink, void* context);\n"
        "    // V8 parrot: one 10 ms mono frame at a time, pointer valid during the call.\n"
        "    using MicFrameSink = void (*)(void* context, const int16_t* mono, size_t samples);\n"
        "    void SetMicFrameTap(bool enable, MicFrameSink sink, void* context);\n",
        "AudioService public frame tap method")
    source = once(source,
        "    void* mic_envelope_context_ = nullptr;",
        "    void* mic_envelope_context_ = nullptr;\n"
        "    MicFrameSink mic_frame_sink_ = nullptr;\n"
        "    void* mic_frame_context_ = nullptr;",
        "AudioService frame tap state")
    return source


def patch_audio_cc(source: str) -> str:
    _fresh(source, "AudioService source")
    source = once(source,
        "void AudioService::AudioInputTask() {",
        FRAME_HELPER.lstrip("\n") + "\nvoid AudioService::AudioInputTask() {",
        "AudioService frame tap implementation")
    source = once(source,
        "                                                  AS_EVENT_MIC_ENVELOPE;\n",
        "                                                  AS_EVENT_MIC_ENVELOPE |\n"
        "                                                  AS_EVENT_MIC_FRAMES;\n",
        "AudioInputTask active bits")
    source = once(source,
        "        /* V4-08: microphone games only need the loudness of each 10 ms frame. */\n",
        TAP_FRAMES_ALONE +
        "        /* V4-08: microphone games only need the loudness of each 10 ms frame. */\n",
        "AudioInputTask frame read")
    # Never share the microphone with a voice session.
    source = once(source,
        "        xEventGroupClearBits(event_group_, AS_EVENT_MIC_ENVELOPE);  // V4-08\n",
        "        xEventGroupClearBits(event_group_, AS_EVENT_MIC_ENVELOPE);  // V4-08\n"
        "        xEventGroupClearBits(event_group_, AS_EVENT_MIC_FRAMES);    // V8 parrot\n",
        "AudioService voice clears the frame tap")
    return source
