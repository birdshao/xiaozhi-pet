"""V8 sound activities on the board: the raw microphone frame tap and the wiring.

Behaviour of the cores is in tests/cpp/test_parrot_session.cc, test_song_player.cc,
test_routine_timer.cc and test_sound_view.cc. These tests keep the privacy and
arbitration rules of the board glue (scripts/p1_integrate.py) and of the pinned
AudioService patch (scripts/v8_mic_frame_tap_patch.py) from being loosened.
"""
import os
import re
import unittest
from pathlib import Path

from scripts import v4_mic_tap_patch as v4
from scripts import v8_mic_frame_tap_patch as tap
from scripts.p1_integrate import BOARD_WORKER

ROOT = Path(__file__).resolve().parents[1]
KID = ROOT / "firmware/components/kid_app"


def code_only(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^" + chr(10) + "]*", "", text)
    return re.sub(r'"(?:' + chr(92) * 2 + r'.|[^"' + chr(92) * 2 + r'])*"', '""', text)


HEADER = (
    "#define AS_EVENT_AUDIO_INPUT_STOP_REQUEST (1 << 4)\n"
    "    void EnableDeviceAec(bool enable);\n"
    "    uint32_t playback_generation_ = 0;\n"
)
SOURCE = (
    "void AudioService::AudioInputTask() {\n"
    "    constexpr EventBits_t kAudioInputActiveBits = AS_EVENT_AUDIO_TESTING_RUNNING |\n"
    "                                                  AS_EVENT_WAKE_WORD_RUNNING |\n"
    "                                                  AS_EVENT_AUDIO_PROCESSOR_RUNNING;\n"
    "            if (ReadAudioData(data, 16000, samples)) {\n"
    "                audio_engine_->Feed(std::move(data));\n"
    "        // Read timeout/error should not terminate the input task.\n"
    "        audio_input_need_warmup_ = true;\n"
)


class FrameTapPatch(unittest.TestCase):
    def test_header_patch(self):
        base = v4.patch_audio_h(HEADER)
        new = tap.patch_audio_h(base)
        self.assertIn("#define AS_EVENT_MIC_FRAMES (1 << 6)", new)
        self.assertIn("using MicFrameSink = void (*)(void* context, const int16_t* mono, size_t samples);", new)
        with self.assertRaisesRegex(RuntimeError, "already patched"):
            tap.patch_audio_h(new)

    def test_source_patch_hands_out_one_frame_and_never_shares_with_voice(self):
        new = tap.patch_audio_cc(v4.patch_audio_cc(SOURCE))
        self.assertEqual(new.count("MicFrameDeliver(data, codec_->input_channels(), mic_frame_sink_,"), 1)
        # Voice processing starting takes the microphone back, for both taps.
        self.assertIn("xEventGroupClearBits(event_group_, AS_EVENT_MIC_FRAMES);    // V8 parrot", new)
        self.assertIn("AS_EVENT_MIC_ENVELOPE |\n                                                  AS_EVENT_MIC_FRAMES;", new)
        with self.assertRaisesRegex(RuntimeError, "already patched"):
            tap.patch_audio_cc(new)

    def test_patch_keeps_no_copy_queue_or_storage(self):
        added = code_only("".join(tap.ADDED_TEXT))
        for pattern in (r"push_back", r"Push", r"Queue", r"queue", r"\bopen\b", r"\bfopen\b", r"\bnvs",
                        r"\bsend\b", r"\bwrite\b", r"\bEncode", r"memcpy", r"std::vector<int16_t> \w+ ="):
            self.assertIsNone(re.search(pattern, added), pattern)
        # A 10 ms mono frame on the stack, valid only during the sink call.
        self.assertIn("int16_t mono[160];", tap.FRAME_HELPER)
        self.assertNotIn("audio_testing_queue_", added)
        self.assertNotIn("audio_send_queue_", added)

    @unittest.skipUnless(os.environ.get("XIAOZHI_SOURCE"),
                         "set XIAOZHI_SOURCE to a pinned upstream checkout to run")
    def test_applies_to_the_pinned_upstream_source(self):
        from scripts.p4_audio_queue_patch import patch_audio_cc, patch_audio_h
        audio = Path(os.environ["XIAOZHI_SOURCE"]) / "main/audio"
        header = v4.patch_audio_h(patch_audio_h((audio / "audio_service.h").read_text(encoding="utf-8")))
        source = v4.patch_audio_cc(patch_audio_cc((audio / "audio_service.cc").read_text(encoding="utf-8")))
        tap.patch_audio_h(header)
        tap.patch_audio_cc(source)


START = BOARD_WORKER.index("// ---- V8 sound activities")
END = BOARD_WORKER.index("// ---- V4-08 microphone games")
SOUND = BOARD_WORKER[START:END]


class BoardWiring(unittest.TestCase):
    def test_section_has_no_storage_network_or_memory_writes(self):
        code = code_only(SOUND)
        for pattern in (r"\bfopen\b", r"\bofstream\b", r"\bnvs_", r"\bhttp", r"\bsocket\b",
                        r"\bPostMemory\b", r"\bRecordMemory\b", r"\bMemorySource\b", r"\bstd::vector\b",
                        r"\bPostPrompt\b"):
            self.assertIsNone(re.search(pattern, code), pattern)

    def test_the_frame_tap_is_armed_only_when_the_parrot_records(self):
        self.assertEqual(BOARD_WORKER.count("SetMicFrameTap(true,"), 1)
        apply = SOUND[SOUND.index("void ApplyParrotOutput("):SOUND.index("void StartParrotPlayback()")]
        self.assertIn("if (out.start_mic) {\n            audio.SetMicFrameTap(true, &ParrotFrameSink, this);", apply)
        self.assertIn("if (out.stop_mic) {\n            audio.SetMicFrameTap(false, nullptr, nullptr);", apply)
        stop = SOUND[SOUND.index("void StopSound()"):SOUND.index("// ---- parrot ----")]
        self.assertIn("SetMicFrameTap(false, nullptr, nullptr)", stop)
        # The sink only feeds the parrot buffer.
        sink = SOUND[SOUND.index("static void ParrotFrameSink"):SOUND.index("kid_app::ParrotContext ParrotCtx()")]
        self.assertIn("self->parrot_.Feed(mono, samples);", sink)
        self.assertEqual(sink.count(";"), sink.count("self->parrot_.Feed") + 2)  # self, lock, Feed

    def test_recording_lives_in_psram_and_is_wiped_and_freed_on_every_exit(self):
        self.assertIn("MALLOC_CAP_SPIRAM", SOUND)
        stop = SOUND[SOUND.index("void StopSound()"):SOUND.index("// ---- parrot ----")]
        self.assertIn("parrot_.Interrupt();", stop)
        self.assertIn("parrot_.Bind(nullptr, 0);  // wipes the buffer once more", stop)
        self.assertLess(stop.index("parrot_.Bind(nullptr, 0)"), stop.index("heap_caps_free(parrot_buffer_)"))
        end = BOARD_WORKER[BOARD_WORKER.index("void EndGame(const char* reason"):]
        end = end[:end.index("PET_GAME_END")]
        self.assertIn("case GameMode::Sound:\n                StopSound();", end)
        # BOOT (voice) and PWR (sleep) end every game, sound activities included.
        game_update = BOARD_WORKER[BOARD_WORKER.index("void UpdateGame(DeviceState state)"):]
        self.assertLess(game_update.index('EndGame("voice")'), game_update.index("UpdateSound();"))
        self.assertIn('EndGame("sleep", false)', BOARD_WORKER)

    def test_playback_scrubs_its_staging_block_and_stops_when_interrupted(self):
        loop = SOUND[SOUND.index("void ParrotPlayLoop()"):SOUND.index("// ---- songs ----")]
        self.assertGreaterEqual(loop.count("kid_app::ParrotSession::Scrub("), 2)
        self.assertIn("parrot_play_gen_.load() != gen", loop)
        self.assertIn("PlaybackRefused()", loop)
        self.assertIn("PromptAllowed(false)", loop)

    def test_dial_centre_goes_to_the_parrot_not_the_hub_while_it_is_open(self):
        loop = BOARD_WORKER[BOARD_WORKER.index("void InputTaskLoop()"):]
        loop = loop[:loop.index("input_hub_.Tick(NowMs());")]
        self.assertLess(loop.index("parrot_holding_.load()"), loop.index("input_hub_.Press(key, edge.at_ms)"))
        self.assertIn("HandleParrotEdge(edge.down);\n                    continue;", loop)

    def test_lullaby_end_goes_through_the_one_sleep_flow(self):
        songs = SOUND[SOUND.index("void ApplySongOutput("):SOUND.index("// ---- brushing / washing ----")]
        self.assertIn("HandleSleepAction(sleep_flow_.Request(NowMs()));", songs)
        self.assertLess(songs.index("out.stop_audio"), songs.index("out.sleep_now"))

    def test_builtin_melodies_are_always_listed_and_opened_without_the_card(self):
        # V9-3A: offered after the card scan (a card book with the same id wins).
        start = SOUND[SOUND.index("case SoundKind::Songs: {"):]
        start = start[:start.index("songs_.Open(")]
        self.assertLess(start.index("ScanSongs(&catalog)"), start.index("AddBuiltinSongs(&catalog)"))
        self.assertIn("OfferBuiltin(", SOUND)
        songs = SOUND[SOUND.index("void ApplySongOutput("):SOUND.index("// ---- brushing / washing ----")]
        self.assertIn("song_worker_.OpenBuiltin(", songs)
        self.assertIn("song_worker_.Open(id)", songs)
        # The synthesised stream goes through the same volume path as the card songs.
        worker = (KID / "src/freertos_song_worker.cc").read_text(encoding="utf-8")
        self.assertIn("hooks_.push(token,", worker)
        self.assertIn("BuiltinMelodyAt(builtin_ - 1)", worker)

    def test_routine_record_is_device_sourced_and_never_from_voice(self):
        routine = SOUND[SOUND.index("void ApplyRoutineOutput("):SOUND.index("// pet_input: the dial while a sound activity runs.")]
        self.assertIn("source=device", routine)
        # Voice only opens the Ready screen: the proposal handler starts no timer.
        voice = BOARD_WORKER[BOARD_WORKER.index("switch (decision.proposal)"):]
        voice = voice[:voice.index("PET_VOICE_CMD name=")]
        self.assertNotIn("routine_.Input", voice)
        self.assertNotIn("TakeRecord", voice)
        core = (KID / "src/routine_timer.cc").read_text(encoding="utf-8")
        self.assertIn("if (input != GameInput::Press || phase_ != RoutinePhase::Ready) return out;", core)

    def test_activity_rows_have_dispatch_branches(self):
        menu = BOARD_WORKER[BOARD_WORKER.index("void HandleMenuOutput("):BOARD_WORKER.index("void LogMotionRound()")]
        for choice, kind in (("Parrot", "Parrot"), ("Songs", "Songs"), ("ToothBrush", "Brush"), ("HandWash", "Wash")):
            self.assertIn(f"GameChoice::{choice}) {{", menu.replace("  // V8 sound", ""))
            self.assertIn(f"StartSound(SoundKind::{kind});", menu)


if __name__ == "__main__":
    unittest.main()
