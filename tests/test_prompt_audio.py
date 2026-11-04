"""Home-rail prompt clips: format the firmware accepts, and the tool."""
import array
import tempfile
import unittest
import wave
from pathlib import Path

from tools.make_prompt_audio import ITEMS, PROMPT_DIR, ffmpeg_command, tones

MAX_SAMPLES = 2 * 24000


class PromptAudioTests(unittest.TestCase):
    def check_clip(self, path: Path) -> None:
        with wave.open(str(path), "rb") as w:
            self.assertEqual((w.getnchannels(), w.getsampwidth(), w.getframerate()), (1, 2, 24000))
            self.assertTrue(0 < w.getnframes() <= MAX_SAMPLES, path)
            pcm = array.array("h", w.readframes(w.getnframes()))
        self.assertLess(max(abs(s) for s in pcm), 32767 * 0.5, "prompts stay quiet")

    def test_committed_clips_match_firmware_format(self):
        names = [p.name for p in sorted(PROMPT_DIR.glob("*.wav"))]
        self.assertEqual(names, sorted(f"prompt_{item}.wav" for item in ITEMS))
        for name in names:
            self.check_clip(PROMPT_DIR / name)

    def test_tone_generator_is_deterministic(self):
        with tempfile.TemporaryDirectory() as a, tempfile.TemporaryDirectory() as b:
            tones(Path(a))
            tones(Path(b))
            for item in ITEMS:
                name = f"prompt_{item}.wav"
                self.check_clip(Path(a) / name)
                self.assertEqual((Path(a) / name).read_bytes(), (Path(b) / name).read_bytes())

    def test_clips_are_embedded_in_rail_order(self):
        component = PROMPT_DIR.parents[1]
        cmake = (component / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("EMBED_FILES", cmake)
        for item in ITEMS:
            self.assertIn(f'"assets/prompts/prompt_{item}.wav"', cmake)
        clips = (component / "src/prompt_clips.cc").read_text(encoding="utf-8")
        starts = [clips.index(f'"_binary_prompt_{item}_wav_start"') for item in ITEMS]
        self.assertEqual(starts, sorted(starts), "symbols must follow HomeItem order")
        for item in ITEMS:
            self.assertIn(f'"_binary_prompt_{item}_wav_end"', clips)
        self.assertIn("ParseWavClip(", clips)

    def test_recordings_are_converted_to_the_firmware_format(self):
        cmd = ffmpeg_command(Path("in.m4a"), Path("out.wav"))
        self.assertEqual(cmd[0], "ffmpeg")
        for flag in ("-ac", "1", "-ar", "24000", "-c:a", "pcm_s16le", "-t", "2"):
            self.assertIn(flag, cmd)
        self.assertEqual(cmd[-1], "out.wav")


if __name__ == "__main__":
    unittest.main()
