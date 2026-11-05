"""Pinned XiaoZhi AudioService patch: a loudness-only microphone tap (V4-08).

The microphone games need one number per 10 ms: the RMS of the frame the
AudioInputTask has just read. This patch adds exactly that and nothing
else. PRIVACY (ADR-011, PRD section 2 item 7):

* the tap never copies, queues, encodes, stores or sends samples: the sink
  gets a single uint16 and the frame goes on (or is dropped) as before;
* AudioInputTask stays the only reader of the codec (no second I2S owner);
* voice processing switching on clears the tap bit, so the microphone is
  never shared with a voice session (the board also ends the game).

tests/test_v4_mic_games.py applies it to fixtures and to the real pinned
source when available, and checks the added text for any buffer, file or
network use.
"""
from __future__ import annotations


def once(source: str, before: str, after: str, label: str) -> str:
    matches = source.count(before)
    if matches != 1:
        raise RuntimeError(f"{label}: expected 1 anchor; found {matches}")
    return source.replace(before, after, 1)


# Everything this patch adds to audio_service.h / .cc, for the privacy test.
RMS_HELPER = r"""
// V4-08: loudness of one input frame (channel 0). Only this number leaves
// the input task; the samples are not kept.
static uint16_t MicFrameRms(const std::vector<int16_t>& data, int channels) {
    if (channels < 1 || data.empty()) return 0;
    uint64_t sum = 0;
    size_t n = 0;
    for (size_t i = 0; i < data.size(); i += channels, ++n) {
        const int32_t v = data[i];
        sum += static_cast<uint64_t>(v * v);
    }
    const float rms = n ? __builtin_sqrtf(static_cast<float>(sum) / static_cast<float>(n)) : 0.0f;
    return rms > 65535.0f ? 65535 : static_cast<uint16_t>(rms);
}

void AudioService::SetMicEnvelopeTap(bool enable, MicEnvelopeSink sink, void* context) {
    if (enable && sink) {
        mic_envelope_sink_ = sink;
        mic_envelope_context_ = context;
        xEventGroupSetBits(event_group_, AS_EVENT_MIC_ENVELOPE);
    } else {
        xEventGroupClearBits(event_group_, AS_EVENT_MIC_ENVELOPE);
    }
}
"""

# The two lines in AudioInputTask that call the sink.
TAP_AFTER_ENGINE_READ = (
    "            if (ReadAudioData(data, 16000, samples)) {\n"
    "                if ((bits & AS_EVENT_MIC_ENVELOPE) && mic_envelope_sink_)\n"
    "                    mic_envelope_sink_(mic_envelope_context_,\n"
    "                                       MicFrameRms(data, codec_->input_channels()));\n"
    "                audio_engine_->Feed(std::move(data));\n"
)
TAP_ALONE = (
    "        /* V4-08: microphone games only need the loudness of each 10 ms frame. */\n"
    "        if ((bits & AS_EVENT_MIC_ENVELOPE) && mic_envelope_sink_) {\n"
    "            std::vector<int16_t> data;\n"
    "            if (ReadAudioData(data, 16000, 160)) {\n"
    "                mic_envelope_sink_(mic_envelope_context_,\n"
    "                                   MicFrameRms(data, codec_->input_channels()));\n"
    "                continue;\n"
    "            }\n"
    "        }\n\n"
)


def _fresh(source: str, label: str) -> None:
    if "MIC_ENVELOPE" in source or "MicEnvelope" in source:
        raise RuntimeError(f"{label}: expected 1 anchor; already patched")


def patch_audio_h(source: str) -> str:
    _fresh(source, "AudioService header")
    source = once(source,
        "#define AS_EVENT_AUDIO_INPUT_STOP_REQUEST (1 << 4)\n",
        "#define AS_EVENT_AUDIO_INPUT_STOP_REQUEST (1 << 4)\n"
        "#define AS_EVENT_MIC_ENVELOPE (1 << 5)\n",
        "AudioService event bits")
    source = once(source,
        "    void EnableDeviceAec(bool enable);\n",
        "    void EnableDeviceAec(bool enable);\n"
        "    // V4-08: loudness (RMS of 10 ms) of the input frames, nothing else.\n"
        "    using MicEnvelopeSink = void (*)(void* context, uint16_t rms);\n"
        "    void SetMicEnvelopeTap(bool enable, MicEnvelopeSink sink, void* context);\n",
        "AudioService public tap method")
    source = once(source,
        "    uint32_t playback_generation_ = 0;",
        "    uint32_t playback_generation_ = 0;\n"
        "    MicEnvelopeSink mic_envelope_sink_ = nullptr;\n"
        "    void* mic_envelope_context_ = nullptr;",
        "AudioService tap state")
    return source


def patch_audio_cc(source: str) -> str:
    _fresh(source, "AudioService source")
    source = once(source,
        "void AudioService::AudioInputTask() {",
        RMS_HELPER.lstrip("\n") + "\nvoid AudioService::AudioInputTask() {",
        "AudioService tap implementation")
    source = once(source,
        "                                                  AS_EVENT_AUDIO_PROCESSOR_RUNNING;\n",
        "                                                  AS_EVENT_AUDIO_PROCESSOR_RUNNING |\n"
        "                                                  AS_EVENT_MIC_ENVELOPE;\n",
        "AudioInputTask active bits")
    source = once(source,
        "            if (ReadAudioData(data, 16000, samples)) {\n"
        "                audio_engine_->Feed(std::move(data));\n",
        TAP_AFTER_ENGINE_READ,
        "AudioInputTask engine read")
    source = once(source,
        "        // Read timeout/error should not terminate the input task.\n",
        TAP_ALONE + "        // Read timeout/error should not terminate the input task.\n",
        "AudioInputTask tap read")
    # Never share the microphone with a voice session.
    source = once(source,
        "        audio_input_need_warmup_ = true;\n",
        "        audio_input_need_warmup_ = true;\n"
        "        xEventGroupClearBits(event_group_, AS_EVENT_MIC_ENVELOPE);  // V4-08\n",
        "AudioService voice clears the tap")
    return source
