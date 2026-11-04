"""KPS1 sound pack tool (V2-08): format, review gate, catalog, fallbacks.

Every WAV here is a synthetic signal, not content for children.
"""
import array
import json
import struct
import tempfile
import unittest
import wave
import zlib
from pathlib import Path

from tools import make_prompt_audio
from tools.sound_pack import (HEADER, MAX_CLIPS, PLAYED, RATE, RECORD, catalog, checklist,
                              decode, demo, encode, ffmpeg_command, main, make_pack,
                              phrase_table)

ROOT = Path(__file__).resolve().parents[1]


def write_wav(path: Path, samples: int, *, channels: int = 1, rate: int = RATE,
              value: int = 1200) -> None:
    pcm = array.array("h", [value if i % 2 else -value for i in range(samples * channels)])
    with wave.open(str(path), "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm.tobytes())


class FormatTests(unittest.TestCase):
    def test_layout_matches_the_firmware_reader(self):
        clips = {2: b"\x01\x00" * 301, 4: b"\x02\x00" * 240, 211: b"\x03\x00" * 4800}
        blob = encode(clips, revision=9)
        self.assertEqual((HEADER.size, RECORD.size), (32, 16))
        magic, version, count, rate, revision, data, total, table_crc, crc = \
            HEADER.unpack_from(blob)
        self.assertEqual((magic, version, count, rate, revision), (b"KPS1", 1, 3, 24000, 9))
        self.assertEqual((data, total), (32 + 3 * 16, len(blob)))
        self.assertEqual(crc, zlib.crc32(blob[:28]))
        self.assertEqual(table_crc, zlib.crc32(blob[32:data]))
        ids, end = [], data
        for i in range(count):
            cid, reserved, offset, size, ccrc = RECORD.unpack_from(blob, 32 + 16 * i)
            self.assertEqual((reserved, offset % 4), (0, 0))
            self.assertEqual(offset, (end + 3) & ~3)
            self.assertEqual(ccrc, zlib.crc32(blob[offset:offset + size]))
            ids.append(cid)
            end = offset + size
        self.assertEqual(ids, [2, 4, 211])
        self.assertEqual(end, total)
        self.assertEqual(decode(blob), (9, clips))
        self.assertEqual(encode(clips, revision=9), blob, "deterministic")

    def test_damage_is_refused(self):
        blob = bytearray(encode({1: b"\x10\x00" * 300, 111: b"\x20\x00" * 900}))

        def bad(data: bytes, message: str) -> None:
            with self.assertRaisesRegex(ValueError, message):
                decode(bytes(data))

        flipped = bytearray(blob)
        flipped[-1] ^= 1
        bad(flipped, "clip 111: CRC")
        flipped = bytearray(blob)
        flipped[32] ^= 1
        bad(flipped, "table CRC")
        flipped = bytearray(blob)
        flipped[12] ^= 1
        bad(flipped, "header CRC")
        bad(blob[:-2], "header sizes")
        bad(blob + b"\0\0\0\0", "header sizes")
        bad(b"KPB1" + bytes(blob[4:]), "not a KPS1")

    def test_encoder_limits(self):
        with self.assertRaisesRegex(ValueError, "1..96"):
            encode({})
        with self.assertRaisesRegex(ValueError, "1..96"):
            encode({i: b"\0\0" for i in range(1, MAX_CLIPS + 2)})
        with self.assertRaisesRegex(ValueError, "bad id"):
            encode({1000: b"\0\0"})
        with self.assertRaisesRegex(ValueError, "bad id"):
            encode({1: b"\0"})


class CatalogTests(unittest.TestCase):
    def test_option_ids_follow_the_home_rail(self):
        options = [c for c in catalog() if c.kind == "option"]
        self.assertEqual([c.name for c in options], list(make_prompt_audio.ITEMS))
        self.assertEqual([c.id for c in options], [1, 2, 3, 4, 5])

    def test_phrases_come_from_the_reviewed_table(self):
        source = (ROOT / "firmware/components/kid_app/src/personality.cc").read_text(
            encoding="utf-8")
        phrases = phrase_table()
        ids = [c.id for c in phrases]
        self.assertEqual(len(ids), len(set(ids)))
        for clip in phrases:
            self.assertIn(f'{{{clip.id}, T::', source)
            self.assertIn(f'"{clip.text}"', source)
            self.assertNotIn("{", clip.text, "slot lines cannot be recorded")
            self.assertEqual(clip.id // 100, ("curious", "gentle", "lively")
                             .index(clip.temperament) + 1)
        # Every spoken moment has a line for every temperament.
        for temperament in ("curious", "gentle", "lively"):
            have = {c.context for c in phrases if c.temperament == temperament}
            self.assertEqual(have, set(PLAYED))
        self.assertIn(271, ids)  # giggle (V5-04)
        self.assertNotIn(122, ids)  # "{book}" morning recap

    def test_checklist_names_every_file(self):
        text = checklist()
        for clip in catalog():
            self.assertIn(f"| {clip.name} |", text)
        gentle = checklist("gentle")
        self.assertIn("| 211 |", gentle)
        self.assertNotIn("| 311 |", gentle)

    def test_recordings_are_converted_to_the_clip_format(self):
        cmd = ffmpeg_command(Path("in.m4a"), Path("211.wav"), 6)
        for flag in ("-ac", "1", "-ar", "24000", "-c:a", "pcm_s16le", "-t", "6"):
            self.assertIn(flag, cmd)
        self.assertIn("2", make_prompt_audio.ffmpeg_command(Path("a"), Path("b")))


class PackTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.base = Path(self.tmp.name)
        self.clips = self.base / "clips"
        self.clips.mkdir()
        write_wav(self.clips / "chat.wav", 12000)
        write_wav(self.clips / "211.wav", 36000)
        self.out = self.base / "out" / "default.kps"

    def tearDown(self):
        self.tmp.cleanup()

    def test_review_gate_and_manifest(self):
        with self.assertRaisesRegex(ValueError, "--reviewed"):
            make_pack(self.clips, self.out, reviewed=False)
        self.assertFalse(self.out.exists())
        manifest = make_pack(self.clips, self.out, reviewed=True, revision=3)
        revision, clips = decode(self.out.read_bytes())
        self.assertEqual((revision, sorted(clips)), (3, [1, 211]))
        saved = json.loads(self.out.with_suffix(".manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(saved, manifest)
        self.assertEqual(saved["sd_path"], "/sounds/default.kps")
        self.assertTrue(saved["reviewed"])
        self.assertFalse(saved["placeholder"])
        self.assertEqual([c["id"] for c in saved["clips"]], [1, 211])
        self.assertEqual(saved["clips"][1]["duration_ms"], 1500)
        self.assertIn("story", saved["missing_fall_back_to_builtin"])
        self.assertIn("212", saved["missing_fall_back_to_builtin"])

    def test_bad_clips_are_refused(self):
        cases = [("stereo", lambda p: write_wav(p, 1000, channels=2), "mono"),
                 ("rate", lambda p: write_wav(p, 1000, rate=16000), "24000"),
                 ("short", lambda p: write_wav(p, 100), "0.1 s"),
                 ("long", lambda p: write_wav(p, 6 * RATE + 1), "6 s")]
        for label, make, message in cases:
            with self.subTest(label):
                make(self.clips / "211.wav")
                with self.assertRaisesRegex(ValueError, message):
                    make_pack(self.clips, self.out, reviewed=True)
        write_wav(self.clips / "211.wav", 3000)
        write_wav(self.clips / "chat.wav", 2 * RATE + 1)  # option names <= 2 s
        with self.assertRaisesRegex(ValueError, "2 s"):
            make_pack(self.clips, self.out, reviewed=True)
        write_wav(self.clips / "chat.wav", 3000)
        write_wav(self.clips / "122.wav", 3000)  # a {book} line
        with self.assertRaisesRegex(ValueError, "not in the checklist"):
            make_pack(self.clips, self.out, reviewed=True)
        (self.clips / "122.wav").unlink()
        with self.assertRaisesRegex(ValueError, "pack_id"):
            make_pack(self.clips, self.base / "Bad Name.kps", reviewed=True)
        self.assertFalse(self.out.exists())

    def test_demo_pack_holds_every_spoken_clip(self):
        manifest = demo(self.base / "smoke")
        blob = (self.base / "smoke/sounds/default.kps").read_bytes()
        _, clips = decode(blob)
        self.assertTrue(manifest["placeholder"])
        self.assertEqual(manifest["missing_fall_back_to_builtin"], [])
        self.assertEqual(sorted(clips), sorted(c.id for c in catalog()))
        # Option names are the embedded placeholder melodies.
        with wave.open(str(make_prompt_audio.PROMPT_DIR / "prompt_game.wav")) as w:
            self.assertEqual(clips[3], w.readframes(w.getnframes()))
        self.assertTrue((self.base / "smoke/README-SMOKE-ONLY.txt").exists())
        self.assertEqual(main(["verify", str(self.base / "smoke/sounds/default.kps")]), 0)

    def test_cli_pack_needs_review(self):
        self.assertEqual(main(["pack", str(self.clips), "--output", str(self.out)]), 1)
        self.assertEqual(main(["pack", str(self.clips), "--output", str(self.out),
                               "--reviewed"]), 0)


class WiringTests(unittest.TestCase):
    def test_reader_is_built_and_host_tested(self):
        cmake = (ROOT / "firmware/components/kid_app/CMakeLists.txt").read_text(encoding="utf-8")
        for src in ("sound_pack_reader.cc", "sound_bank.cc", "pet_phrases.cc"):
            self.assertIn(f'"src/{src}"', cmake)
        p2 = (ROOT / "scripts/p2_test.sh").read_text(encoding="utf-8")
        self.assertIn("tools.sound_pack demo", p2)
        self.assertIn('"$BUILD/test_sound_pack" "$BUILD/sound-demo/sounds"', p2)


if __name__ == "__main__":
    unittest.main()
