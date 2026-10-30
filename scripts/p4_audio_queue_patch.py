"""Pinned XiaoZhi AudioService patch: local story PCM enters the official output task.

Never create a second I2S writer. A session token invalidates queued old-page
audio and a transition away from Idle cancels narration before voice playback.
"""
from __future__ import annotations

def once(source: str, before: str, after: str, label: str) -> str:
    matches = source.count(before)
    if matches != 1:
        raise RuntimeError(f"{label}: expected 1 anchor; found {matches}")
    return source.replace(before, after, 1)


def patch_audio_h(source: str) -> str:
    source = once(source,
        "    kAudioTaskTypeDecodeToPlaybackQueue,\n",
        "    kAudioTaskTypeDecodeToPlaybackQueue,\n    kAudioTaskTypeLocalStoryPcm,\n",
        "AudioTaskType enum")
    source = once(source, "    uint32_t media_position_ms = 0;\n};",
        "    uint32_t media_position_ms = 0;\n    uint64_t story_session = 0;\n};",
        "AudioTask session field")
    source = once(source,
        "    bool PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet, bool wait = false);",
        """    bool PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet, bool wait = false);
    // Reviewed offline book, 24 kHz mono PCM16. All I2S writes remain
    // exclusively inside the original AudioOutputTask.
    bool BeginLocalStoryPcm(uint64_t session);
    bool TryQueueLocalStoryPcm(uint64_t session, const int16_t* pcm, size_t samples);
    void CancelLocalStoryPcm();""",
        "AudioService public story audio methods")
    source = once(source,"    uint32_t playback_generation_ = 0;",
        """    uint32_t playback_generation_ = 0;
    // Guarded by audio_queue_mutex_, not a second audio device.
    uint64_t local_story_session_ = 0;""",
        "AudioService story audio state")
    return source


PCM_IMPL = r"""
bool AudioService::BeginLocalStoryPcm(uint64_t session) {
    if (!session || !codec_ || codec_->output_sample_rate() != 24000)
        return false;
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (service_stopped_.load() || !audio_decode_queue_.empty() ||
        decode_in_flight_) return false;
    // Never start a book over a queued or in-flight voice/previous page
    // packet. The SD feeder retries after the existing output drains.
    if (!audio_playback_queue_.empty() || output_in_flight_)
        return false;
    local_story_session_ = session;
    return true;
}

bool AudioService::TryQueueLocalStoryPcm(
        uint64_t session, const int16_t* pcm, size_t samples) {
    if (!session || !pcm || samples == 0 || samples > 480)
        return false;
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (service_stopped_.load() || !codec_ ||
        codec_->output_sample_rate() != 24000 ||
        local_story_session_ != session ||
        !audio_decode_queue_.empty() || decode_in_flight_ ||
        audio_playback_queue_.full())
        return false;
    AudioTask task{};
    task.type = kAudioTaskTypeLocalStoryPcm;
    task.story_session = session;
    task.pcm.assign(pcm, pcm + samples);
    audio_playback_queue_.push_back(std::move(task));
    audio_queue_cv_.notify_all();
    return true;
}

void AudioService::CancelLocalStoryPcm() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    local_story_session_ = 0;
    audio_queue_cv_.notify_all();
}
"""


def patch_audio_cc(source: str) -> str:
    source = once(source, "void AudioService::AudioOutputTask() {",
                  PCM_IMPL + "\nvoid AudioService::AudioOutputTask() {",
                  "AudioService PCM implementation")
    source = once(source, "        if (!codec_->output_enabled()) {\n",
        """        // A canceled/offline story page must never leak into resumed voice.
        bool may_output = true;
        if (task.type == kAudioTaskTypeLocalStoryPcm) {
            std::lock_guard<std::mutex> story_lock(audio_queue_mutex_);
            may_output = local_story_session_ != 0 &&
                         local_story_session_ == task.story_session;
        }
        if (may_output && !codec_->output_enabled()) {
""",
        "AudioService single output path guard")
    source = once(source, "        codec_->OutputData(task.pcm);",
                  "        if (may_output) codec_->OutputData(task.pcm);",
                  "AudioService sole I2S write")
    # Decoded network speech can arrive before Application's state switch
    # is dispatched. Invalidate book audio while holding the SAME queue
    # mutex, as soon as a network packet is accepted for decoding.
    source = once(source,
        "    playback_drained_notified_ = false;\n"
        "    audio_decode_queue_.push_back(std::move(packet));",
        "    local_story_session_ = 0;\n"
        "    playback_drained_notified_ = false;\n"
        "    audio_decode_queue_.push_back(std::move(packet));",
        "server voice PCM preemption at packet ingress")
    return source


def patch_application_cc(source: str) -> str:
    return once(source,
        "bool Application::SetDeviceState(DeviceState state) { return state_machine_.TransitionTo(state); }",
        """bool Application::SetDeviceState(DeviceState state) {
    const bool changed = state_machine_.TransitionTo(state);
    // Interrupt offline narration on voice/listening and all non-idle states.
    // Canceling only invalidates queued story frames; the existing output
    // task remains the only path that writes to the ES8311 codec.
    if (changed && state != kDeviceStateIdle)
        audio_service_.CancelLocalStoryPcm();
    return changed;
}""",
        "Application voice preemption")
