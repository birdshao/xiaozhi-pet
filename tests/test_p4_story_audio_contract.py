"""Pinned upstream source contracts for ePaper reader speech output.

Only XiaoZhi's AudioOutputTask may write ES8311/I2S. Real firmware build
compiles the patch against locked upstream; these tests reject source drift.
"""
from pathlib import Path
import unittest

from scripts.p4_audio_queue_patch import (
    patch_audio_h, patch_audio_cc, patch_application_cc,
)
from scripts.p1_integrate import BOARD_WORKER

ROOT=Path(__file__).resolve().parents[1]

class StoryAudioContract(unittest.TestCase):
    def test_pinned_audio_header_patch(self):
        source=(
            "    kAudioTaskTypeDecodeToPlaybackQueue,\n"
            "    uint32_t media_position_ms = 0;\n};\n"
            "    bool PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet, bool wait = false);\n"
            "    uint32_t playback_generation_ = 0;\n"
        )
        new=patch_audio_h(source)
        self.assertIn("kAudioTaskTypeLocalStoryPcm",new)
        self.assertIn("BeginLocalStoryPcm",new)
        self.assertIn("TryQueueLocalStoryPcm",new)
        self.assertIn("CancelLocalStoryPcm",new)
        self.assertIn("uint64_t story_session",new)
        with self.assertRaisesRegex(RuntimeError,"expected 1 anchor"):
            patch_audio_h(new)

    def test_only_original_output_task_writes_codec(self):
        source=(
            "void AudioService::AudioOutputTask() {\n"
            "        if (!codec_->output_enabled()) {\n"
            "        codec_->OutputData(task.pcm);\n"
        )
        new=patch_audio_cc(source)
        self.assertEqual(new.count("codec_->OutputData(task.pcm)"),1)
        self.assertIn("if (may_output) codec_->OutputData(task.pcm);",new)
        self.assertIn("audio_playback_queue_.push_back(std::move(task))",new)
        self.assertIn("local_story_session_ == task.story_session",new)
        self.assertIn("audio_playback_queue_.full()",new)
        self.assertIn("samples > 480",new)
        self.assertIn("decode_in_flight_",new)
        with self.assertRaisesRegex(RuntimeError,"expected 1 anchor"):
            patch_audio_cc(new)

    def test_voice_state_always_invalidates_story(self):
        source="bool Application::SetDeviceState(DeviceState state) { return state_machine_.TransitionTo(state); }\n"
        new=patch_application_cc(source)
        self.assertIn("state != kDeviceStateIdle",new)
        self.assertIn("audio_service_.CancelLocalStoryPcm();",new)
        with self.assertRaisesRegex(RuntimeError,"expected 1 anchor"):
            patch_application_cc(new)

    def test_real_board_queues_audio_after_screen_and_stops_on_exit(self):
        self.assertIn("StoryAudioBegin", BOARD_WORKER)
        self.assertIn("StoryAudioPush", BOARD_WORKER)
        self.assertIn("StoryAudioCancel", BOARD_WORKER)
        self.assertIn("GetAudioService().TryQueueLocalStoryPcm",BOARD_WORKER)
        self.assertIn("if (okay && self->story_pcm_.Ready())",BOARD_WORKER)
        self.assertIn("story_pcm_.RequestPage(scene.book_page)",BOARD_WORKER)
        self.assertIn("story_pcm_.Stop()",BOARD_WORKER)
        src=(ROOT/"firmware/components/kid_app/src/freertos_story_pcm_worker.cc").read_text()
        self.assertIn("kFrameSamples = 480",src)
        self.assertIn("reader_.ReadPcm",src)
        self.assertIn("hooks_.cancel",src)
        self.assertNotIn("OutputData",src)
        self.assertNotIn("i2s_channel_write",src)

if __name__=="__main__":
    unittest.main()
