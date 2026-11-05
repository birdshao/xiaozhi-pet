"""V4-08 microphone games: privacy contract and board wiring.

The games only ever see a loudness number (RMS per 10 ms frame): no audio is
recorded, stored or sent. These tests fail the build if the modules that
decide blow/clap/game logic ever gain a file, flash, network or sample-buffer
dependency, if the AudioService tap does more than hand out one uint16, or if
the board wiring arms the microphone outside a mic game / in a voice session.
Behaviour of the classifier and the sessions is in tests/cpp/test_mic_games.cc.
"""
import os
import re
import unittest
from pathlib import Path

from scripts import v4_mic_tap_patch as tap
from scripts.p1_integrate import BOARD_WORKER

ROOT = Path(__file__).resolve().parents[1]
KID = ROOT / "firmware/components/kid_app"
MIC_SOURCES = [
    KID / "include/kid_app/mic_envelope.h",
    KID / "src/mic_envelope.cc",
    KID / "include/kid_app/mic_game.h",
    KID / "src/mic_game.cc",
]

# Anything that could keep or send what the microphone hears.
FORBIDDEN = [
    r"\bfopen\b", r"\bfwrite\b", r"\bfread\b", r"\bFILE\b", r"\bofstream\b", r"\bfstream\b",
    r"\bstd::filesystem\b", r"\bopen\s*\(", r"\bwrite\s*\(", r"\bsend\s*\(", r"\bsocket\b",
    r"\bnvs_", r"\bNvs", r"\bpartition\b", r"\bhttp", r"\bHttp", r"\bwifi\b", r"\bWiFi\b",
    r"\blwip\b", r"\bmqtt\b", r"\bwebsocket\b", r"\bopus\b", r"\bOpus\b", r"\bupload\b",
    r"\bheap_caps", r"\bmalloc\b", r"\bnew\s+\w", r"\bstd::vector\b", r"\bint16_t\s*\*",
    r"\bpcm\b", r"\bPcm\b", r"\bPCM\b", r"\bfwrite\b", r"\bprintf\b", r"\bESP_LOG",
]
ALLOWED_STD_INCLUDES = {
    "algorithm", "array", "cstddef", "cstdint", "cstdlib", "cstring", "initializer_list",
}


def code_only(text: str) -> str:
    """Source without comments and string literals (the privacy text itself says
    'recorded', 'stored', 'sent')."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return re.sub(r'"(?:\\.|[^"\\])*"', '""', text)


class PrivacyContract(unittest.TestCase):
    def test_mic_modules_have_no_storage_network_or_buffer_access(self):
        for path in MIC_SOURCES:
            code = code_only(path.read_text(encoding="utf-8"))
            for pattern in FORBIDDEN:
                self.assertIsNone(re.search(pattern, code),
                                  f"{path.name}: forbidden {pattern!r}")

    def test_mic_modules_only_include_kid_app_and_plain_std_headers(self):
        for path in MIC_SOURCES:
            for inc in re.findall(r'#include\s+[<"]([^>"]+)[>"]', path.read_text(encoding="utf-8")):
                if "/" in inc:
                    self.assertTrue(inc.startswith("kid_app/"), f"{path.name}: {inc}")
                else:
                    self.assertIn(inc, ALLOWED_STD_INCLUDES, f"{path.name}: {inc}")

    def test_classifier_only_takes_a_loudness_number(self):
        header = code_only((KID / "include/kid_app/mic_envelope.h").read_text(encoding="utf-8"))
        self.assertIn("MicDecision Feed(uint16_t rms);", header)
        game = code_only((KID / "include/kid_app/mic_game.h").read_text(encoding="utf-8"))
        self.assertIn("const MicDecision& mic", game)  # a decision, never samples

    def test_privacy_is_stated_in_code_and_docs(self):
        for name in ("include/kid_app/mic_envelope.h", "include/kid_app/mic_game.h"):
            text = (KID / name).read_text(encoding="utf-8")
            self.assertIn("PRIVACY", text, name)
        docs = (ROOT / "docs/V4_MINI_GAMES.md").read_text(encoding="utf-8")
        self.assertIn("不录音", docs)
        self.assertIn("不落盘", docs)
        self.assertIn("不上传", docs)
        self.assertIn("RMS", docs)


# ---- the AudioService tap ----

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


class MicTapPatch(unittest.TestCase):
    def test_header_patch(self):
        new = tap.patch_audio_h(HEADER)
        self.assertIn("#define AS_EVENT_MIC_ENVELOPE (1 << 5)", new)
        self.assertIn("using MicEnvelopeSink = void (*)(void* context, uint16_t rms);", new)
        self.assertIn("void SetMicEnvelopeTap(bool enable, MicEnvelopeSink sink, void* context);", new)
        with self.assertRaisesRegex(RuntimeError, "expected 1 anchor"):
            tap.patch_audio_h(new)

    def test_source_patch_hands_out_one_number_per_frame(self):
        new = tap.patch_audio_cc(SOURCE)
        # Both reads (with and without the voice engine) end in the same sink call.
        self.assertEqual(new.count("mic_envelope_sink_(mic_envelope_context_,"), 2)
        self.assertEqual(new.count("MicFrameRms(data, codec_->input_channels())"), 2)
        self.assertIn("AS_EVENT_PROCESSOR_RUNNING".replace("PROCESSOR", "AUDIO_PROCESSOR") + " |\n", new)
        # Voice processing starting takes the microphone back.
        self.assertIn("audio_input_need_warmup_ = true;\n"
                      "        xEventGroupClearBits(event_group_, AS_EVENT_MIC_ENVELOPE);", new)
        with self.assertRaisesRegex(RuntimeError, "expected 1 anchor"):
            tap.patch_audio_cc(new)

    def test_patch_adds_no_copy_queue_or_storage(self):
        added = code_only(tap.RMS_HELPER + tap.TAP_AFTER_ENGINE_READ + tap.TAP_ALONE)
        for pattern in (r"push_back", r"Push", r"Queue", r"queue", r"\bopen\b", r"\bfopen\b",
                        r"\bnvs", r"\bsend\b", r"\bwrite\b", r"\bEncode", r"\bstd::move\(data\);\s*\n\s*mic"):
            self.assertIsNone(re.search(pattern, added), pattern)
        # The tap branch reads and drops: its frame is a local that goes out of scope.
        self.assertNotIn("audio_testing_queue_", added)
        self.assertNotIn("audio_send_queue_", added)
        # The RMS helper takes a const reference and returns one number.
        self.assertIn("static uint16_t MicFrameRms(const std::vector<int16_t>& data, int channels)",
                      tap.RMS_HELPER)

    @unittest.skipUnless(os.environ.get("XIAOZHI_SOURCE"),
                         "set XIAOZHI_SOURCE to a pinned upstream checkout to run")
    def test_applies_to_the_pinned_upstream_source(self):
        from scripts.p4_audio_queue_patch import patch_audio_cc, patch_audio_h
        audio = Path(os.environ["XIAOZHI_SOURCE"]) / "main/audio"
        header = patch_audio_h((audio / "audio_service.h").read_text(encoding="utf-8"))
        source = patch_audio_cc((audio / "audio_service.cc").read_text(encoding="utf-8"))
        tap.patch_audio_h(header)
        tap.patch_audio_cc(source)


# ---- the board wiring ----

START = BOARD_WORKER.index("// ---- V4-08 microphone games")
END = BOARD_WORKER.index("// ---- Host-scripted self-test")
MIC_BOARD = BOARD_WORKER[START:END]


class BoardWiring(unittest.TestCase):
    def test_board_section_has_no_storage_or_network(self):
        code = code_only(MIC_BOARD)
        for pattern in (r"\bfopen\b", r"\bofstream\b", r"\bnvs_", r"\bhttp", r"\bsocket\b",
                        r"\bPostMemory\b", r"\bRecordMemory\b", r"\bheap_caps", r"\bstd::vector\b",
                        r"\bpcm\b", r"\bPCM\b"):
            self.assertIsNone(re.search(pattern, code), pattern)

    def test_tap_is_armed_only_for_a_mic_game_in_idle(self):
        self.assertEqual(BOARD_WORKER.count("SetMicEnvelopeTap(true,"), 1)
        arm = MIC_BOARD[MIC_BOARD.index("void ArmMic()"):MIC_BOARD.index("void DisarmMic()")]
        self.assertIn("SetMicEnvelopeTap(true, &MicSink, this)", arm)
        self.assertIn("if (!inject)", arm)  # a script drives the microphone: real tap stays off
        self.assertIn("src=%s\", inject ? \"script\" : \"audio\"", arm)
        update = MIC_BOARD[MIC_BOARD.index("void UpdateMicGame("):MIC_BOARD.index("void ApplyMicOutput(")]
        self.assertIn("state == kDeviceStateIdle && mic_game_.MicWanted()", update)
        self.assertIn("if (want) ArmMic();\n        else DisarmMic();", update)
        # Decisions only count while the game wants the microphone.
        self.assertIn("if (want) {\n            decision.blowing", update)

    def test_voice_and_end_of_game_turn_the_microphone_off(self):
        game_update = BOARD_WORKER[BOARD_WORKER.index("void UpdateGame(DeviceState state)"):]
        game_update = game_update[:game_update.index("    // pet_input. Back to the pet's own")]
        self.assertLess(game_update.index('EndGame("voice")'), game_update.index("UpdateMicGame(state)"))
        end = BOARD_WORKER[BOARD_WORKER.index("void EndGame(const char* reason"):]
        end = end[:end.index("PET_GAME_END")]
        self.assertIn("DisarmMic();", end)
        self.assertLess(end.index("DisarmMic();"), end.index("mic_session_ = false;"))
        # Never starts over a voice session.
        self.assertIn("!= kDeviceStateIdle ? \"voice\"", BOARD_WORKER)

    def test_mic_game_uses_the_mini_game_frame(self):
        start = MIC_BOARD[MIC_BOARD.index("void StartMicGame("):MIC_BOARD.index("void ArmMic()")]
        for needle in ("game_active_.store(true)", "pet_mailbox_.game_only.store(true)",
                       "PublishGameSceneLocked()", "PostGameClip(out.clip)"):
            self.assertIn(needle, start)
        self.assertIn("mic_session_ ? mic_game_.View() : game_.View()", BOARD_WORKER)
        self.assertIn("const kid_app::GameResult result = mic_session_ ? mic_game_.Result() : game_.Result();",
                      BOARD_WORKER)
        self.assertIn("GameCompletedMemoryEvent(result", BOARD_WORKER)  # the one growth path

    def test_game_selection_keeps_the_quiz_for_scripted_seeds(self):
        pick = MIC_BOARD[MIC_BOARD.index("bool PickMicGame("):MIC_BOARD.index("void StartMicGame(")]
        self.assertIn("if (game_seed_.load() != 0) return false;", pick)
        self.assertIn("launch % 4 == 1", pick)
        self.assertIn("launch % 4 == 3", pick)
        self.assertLess(BOARD_WORKER.index("PickMicGame(&mic_kind)"),
                        BOARD_WORKER.index("kid_app::GamePackReader* pack = GamePackNow();\n        const kid_app::GameRound* pool"))

    def test_selftest_steps_take_over_the_microphone(self):
        self.assertIn("case kid_app::SelfTestOp::MicGame:", BOARD_WORKER)
        self.assertIn("case kid_app::SelfTestOp::Envelope: {", BOARD_WORKER)
        micgame = BOARD_WORKER[BOARD_WORKER.index("case kid_app::SelfTestOp::MicGame:"):]
        micgame = micgame[:micgame.index("case kid_app::SelfTestOp::Envelope")]
        self.assertIn("mic_inject_.store(true);", micgame)
        done = BOARD_WORKER.index('PET_SELFTEST_DONE steps=')
        self.assertIn("mic_inject_.store(false);", BOARD_WORKER[done - 200:done])

    def test_integrator_applies_the_tap_patch_after_the_story_audio_patch(self):
        script = (ROOT / "scripts/p1_integrate.py").read_text(encoding="utf-8")
        self.assertIn("from scripts import v4_mic_tap_patch", script)
        self.assertIn("v4_mic_tap_patch.patch_audio_cc(\n            patch_audio_cc(", script)
        self.assertIn('#include "kid_app/mic_game.h"', script)


class Catalogue(unittest.TestCase):
    def test_mic_art_ids_are_apart_from_the_pack_catalogue(self):
        view = (KID / "include/kid_app/game_view.h").read_text(encoding="utf-8")
        enum = view[view.index("enum class MicArt"):]
        enum = enum[:enum.index("};")]
        ids = {name: int(v) for name, v in re.findall(r"(\w+) = (\d+)", enum)}
        self.assertEqual(ids, {"CandleLit": 101, "CandleOut": 102, "Hand": 103, "HandHit": 104, "Blow": 105})
        from tools import game_pack as gp
        taken = set(gp.ART.values()) | set(gp.QUESTION_ONLY_ART.values())
        self.assertFalse(taken & set(ids.values()))  # KPG1 packs cannot name them
        scene = (KID / "src/scene_model.cc").read_text(encoding="utf-8")
        for name in ids:
            self.assertIn(f"MicArt::{name}", scene)

    def test_selftest_ops_and_build_lists(self):
        script = (KID / "src/selftest_script.cc").read_text(encoding="utf-8")
        for needle in ('"envelope"', '"micgame"', "MicSynthFromName"):
            self.assertIn(needle, script)
        cmake = (KID / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn('"src/mic_envelope.cc" "src/mic_game.cc"', cmake)
        p2 = (ROOT / "scripts/p2_test.sh").read_text(encoding="utf-8")
        self.assertIn("test_mic_games.cc", p2)

    def test_scenario_exists_and_stays_off_the_real_microphone(self):
        text = (ROOT / "tools/selftest/v4_mic_games.pettest").read_text(encoding="ascii")
        self.assertIn("# expect none:src=audio 0", text)
        self.assertIn("volume 15", text)
        self.assertIn("micgame clap", text)
        self.assertIn("micgame candle", text)


if __name__ == "__main__":
    unittest.main()
