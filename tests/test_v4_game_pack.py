"""V4-06 KPG1 mini-game packs: source checks, binary rules, golden fixture.

The golden pack is synthetic test content (tones), NOT child content.
Regenerate after an intended format change:
    python tests/test_v4_game_pack.py --write-golden
"""
from __future__ import annotations

import io
import json
import re
import struct
import sys
import tempfile
import unittest
import zlib
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from tools import game_pack as gp  # noqa: E402
from tools import kpb2  # noqa: E402

GOLDEN = ROOT / "tests/fixtures/kpg1/golden_game.kpg"
MINI_GAME_H = ROOT / "firmware/components/kid_app/include/kid_app/mini_game.h"


def golden_bytes() -> bytes:
    with tempfile.TemporaryDirectory() as tmp:
        source = gp.demo_source(Path(tmp), content_id=900, note_s=0.04)
        return gp.encode(gp.load_source(source))


def reseal(blob: bytearray) -> bytes:
    """Fix the table and header CRCs after editing a table."""
    nrounds, nclips = struct.unpack_from("<HH", blob, 6)
    end = gp.HEADER.size + gp.ROUND.size * nrounds + gp.CLIP.size * nclips
    struct.pack_into("<I", blob, 24, zlib.crc32(bytes(blob[32:end])))
    struct.pack_into("<I", blob, 28, zlib.crc32(bytes(blob[:28])))
    return bytes(blob)


def round_at(i: int) -> int:
    return gp.HEADER.size + gp.ROUND.size * i


class Source(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.source = gp.demo_source(self.dir, note_s=0.04)
        self.game = json.loads(self.source.read_text(encoding="utf-8"))

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def edited(self, change) -> Path:
        game = json.loads(json.dumps(self.game))
        change(game)
        self.source.write_text(json.dumps(game), encoding="utf-8")
        return self.source

    def rejected(self, change, words: str) -> None:
        with self.assertRaisesRegex(ValueError, words):
            gp.load_source(self.edited(change))

    def test_demo_source_compiles_and_parses(self) -> None:
        game = gp.load_source(self.source)
        parsed = gp.parse(gp.encode(game))
        self.assertEqual(len(parsed["rounds"]), 5)
        # cat.wav is used by rounds 1 and 5 and stored once.
        self.assertEqual(parsed["rounds"][0]["option_clips"][0],
                         parsed["rounds"][4]["option_clips"][1])
        self.assertIn(gp.SESSION_CLIPS["right"], parsed["clips"])
        self.assertEqual(gp.encode(game), gp.encode(gp.load_source(self.source)))

    def test_source_errors(self) -> None:
        def round0(key, value):
            return lambda g: g["rounds"][0].__setitem__(key, value)
        cases = [
            (lambda g: g.__setitem__("approved", False), "not approved"),
            (round0("approved", False), "not approved"),
            (lambda g: g["rounds"][0].pop("approved"), "missing or unknown"),
            (lambda g: g.__setitem__("extra", 1), "missing or unknown"),
            (lambda g: g.__setitem__("content_id", 99), "content_id"),
            (lambda g: g.__setitem__("schema_version", 2), "schema_version"),
            (lambda g: g.__setitem__("package_type", "book"), "schema_version"),
            (lambda g: g.__setitem__("game_id", "../x"), "game_id"),
            (round0("kind", "maths"), "unknown kind"),
            (round0("question_art", "robot"), "unknown question_art"),
            (round0("answer", "fish"), "answer"),
            (round0("prompt", ""), "prompt"),
            (round0("prompt_audio", "../q1.wav"), "relative"),
            (round0("prompt_audio", "missing.wav"), "missing"),
            (lambda g: g["rounds"][0]["options"].pop(), None),  # 2 options are fine
            (lambda g: g["rounds"][0]["options"].__setitem__(slice(1, 3), []), "2 or 3"),
            (lambda g: g["rounds"][0]["options"].append(g["rounds"][0]["options"][0]),
             "2 or 3"),
            (lambda g: g["rounds"][0]["options"][1].__setitem__("art", "cat"), "same picture"),
            (lambda g: g["rounds"][0]["options"][1].__setitem__("art", "listen"),
             "unknown art"),
            (lambda g: g["rounds"][0]["options"][1].__setitem__("label", "x" * 25), "label"),
            (lambda g: g.__setitem__("session_audio", {"cheer": "right.wav"}),
             "missing or unknown"),
            (lambda g: g.__setitem__("rounds", []), "1..64"),
        ]
        for change, words in cases:
            with self.subTest(words=words):
                if words is None:
                    gp.load_source(self.edited(change))
                else:
                    self.rejected(change, words)

    def test_audio_rules(self) -> None:
        gp.write_wav(self.dir / "q1.wav", b"\0\0" * (gp.RATE * 11))
        with self.assertRaisesRegex(ValueError, "0.1..10 s"):
            gp.load_source(self.source)
        import wave
        with wave.open(str(self.dir / "q1.wav"), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(16000)
            w.writeframes(b"\0\0" * 8000)
        with self.assertRaisesRegex(ValueError, "24 kHz"):
            gp.load_source(self.source)

    def test_pack_needs_review_and_writes_manifest(self) -> None:
        out = self.dir / "out" / "default.kpg"
        with self.assertRaisesRegex(ValueError, "--reviewed"):
            gp.make_pack(self.source, out, reviewed=False)
        self.assertFalse(out.exists())
        with self.assertRaisesRegex(ValueError, "pack_id"):
            gp.make_pack(self.source, self.dir / "Bad Name.kpg", reviewed=True)
        manifest = gp.make_pack(self.source, out, reviewed=True)
        written = json.loads(out.with_suffix(".manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest, written)
        self.assertEqual(written["sd_path"], "/games/default.kpg")
        self.assertEqual(written["rounds"][0]["prompt"], "smoke round 1")
        self.assertEqual(written["session_lines_built_in"], ["bye", "reveal", "try_again"])
        # No text in the pack itself: prompts and labels stay in the manifest.
        self.assertNotIn(b"smoke round", out.read_bytes())


class Binary(unittest.TestCase):
    def test_golden_is_reproducible(self) -> None:
        self.assertEqual(GOLDEN.read_bytes(), golden_bytes(),
                         "format changed: run tests/test_v4_game_pack.py --write-golden")

    def test_golden_fields(self) -> None:
        p = gp.parse(GOLDEN.read_bytes())
        self.assertEqual((p["content_id"], p["revision"]), (900, 1))
        r = p["rounds"][0]
        self.assertEqual(r["kind"], gp.KINDS["animal_sound"])
        self.assertEqual(r["arts"], [41, 42, 43])
        self.assertEqual(r["question_art"], 90)
        self.assertEqual(r["correct"], 1)
        self.assertEqual((r["prompt_clip"], r["option_clips"]), (16, [17, 18, 19]))
        self.assertEqual(sorted(p["clips"])[0], gp.SESSION_CLIPS["right"])

    def test_tampering_is_rejected(self) -> None:
        good = GOLDEN.read_bytes()
        nrounds, nclips = struct.unpack_from("<HH", good, 6)
        clip_table = gp.HEADER.size + gp.ROUND.size * nrounds

        def flip(at: int) -> bytes:
            b = bytearray(good)
            b[at] ^= 0x01
            return bytes(b)

        def table(edit) -> bytes:
            b = bytearray(good)
            edit(b)
            return reseal(b)

        last_clip = clip_table + gp.CLIP.size * (nclips - 1)
        cases = {
            "magic": good.replace(b"KPG1", b"KPG2", 1),
            "header crc": flip(10),
            "table crc": flip(round_at(0) + 4),
            "clip crc": flip(len(good) - 1),
            "trailing": good + b"\0\0\0\0",
            "truncated": good[:-2],
            "correct 0": table(lambda b: b.__setitem__(round_at(0) + 2, 0)),
            "correct 4": table(lambda b: b.__setitem__(round_at(0) + 2, 4)),
            "kind 9": table(lambda b: b.__setitem__(round_at(0), 9)),
            "flags": table(lambda b: b.__setitem__(round_at(0) + 3, 1)),
            "same art": table(lambda b: struct.pack_into("<H", b, round_at(0) + 8, 41)),
            "listen option": table(lambda b: struct.pack_into("<H", b, round_at(0) + 6, 90)),
            "unknown question": table(lambda b: struct.pack_into("<H", b, round_at(0) + 4, 7)),
            "missing clip": table(lambda b: struct.pack_into("<H", b, round_at(0) + 12, 998)),
            "tone clip": table(lambda b: struct.pack_into("<H", b, round_at(0) + 12, 0x4000)),
            "unused clip": table(lambda b: struct.pack_into("<H", b, last_clip, 999)),
            "reserved": table(lambda b: struct.pack_into("<I", b, round_at(0) + 20, 1)),
            "content id": table(lambda b: struct.pack_into("<H", b, 10, 42)),
        }
        for name, blob in cases.items():
            with self.subTest(name=name):
                with self.assertRaises(ValueError):
                    gp.parse(blob)

    def test_padding_must_be_zero(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            source = gp.demo_source(Path(tmp), note_s=0.04)
            gp.write_wav(Path(tmp) / "q1.wav", b"\x01\x00" * 2401)  # odd sample count
            good = bytearray(gp.encode(gp.load_source(source)))
        gp.parse(bytes(good))
        nrounds, nclips = struct.unpack_from("<HH", good, 6)
        clip_table = gp.HEADER.size + gp.ROUND.size * nrounds
        end = clip_table + gp.CLIP.size * nclips
        for i in range(nclips):
            _, _, offset, size, _ = gp.CLIP.unpack_from(good, clip_table + i * gp.CLIP.size)
            if offset > end:
                good[end] = 1
                with self.assertRaisesRegex(ValueError, "padding"):
                    gp.parse(bytes(good))
                return
            end = offset + size
        self.fail("no padding in the test pack")


class SharedTables(unittest.TestCase):
    """The firmware and the tool must agree on every id."""

    def test_art_ids_match_game_view_h(self) -> None:
        text = gp.GAME_VIEW_H.read_text(encoding="utf-8")
        enum = text[text.index("enum class GameArt"):text.index("};", text.index("enum class GameArt"))]
        found = {name: int(v) for name, v in re.findall(r"(\w+) = (\d+)", enum)}
        found.pop("None")
        plain = {k: v for k, v in {**gp.ART, **gp.QUESTION_ONLY_ART}.items()
                 if k not in gp.SIZED_ART}
        camel = {"".join(p.capitalize() for p in re.split(r"_|(?<=[a-z])(?=\d)", k)): v
                 for k, v in plain.items()}
        self.assertEqual(found, camel)
        listed = [int(v) for v in re.search(r"kGameArtIds = \{([^}]*)\}", text)
                  .group(1).replace("\n", " ").split(",")]
        self.assertEqual(sorted(listed), sorted(v for k, v in gp.ART.items()
                                                if k not in gp.SIZED_ART))
        # Sized shapes (big_small rounds): 0x3000 | shape << 2 | size, as PlaySizedArt() says.
        self.assertIn("0x3000 | (shape << 2) | size", text)
        self.assertEqual(len(gp.SIZED_ART), 15)

    def test_kinds_and_clip_ids_match_mini_game_h(self) -> None:
        text = MINI_GAME_H.read_text(encoding="utf-8")
        # Only the KPG1 kinds 1..5; the V8 built-in kinds (6..14) never go in a pack.
        kinds = {k: v for k, v in re.findall(r"(\w+) = (\d+),\s+//", text) if int(v) <= 5}
        self.assertEqual({k: int(v) for k, v in kinds.items()},
                         {"".join(p.capitalize() for p in k.split("_")): v
                          for k, v in gp.KINDS.items()})
        for name, cid in gp.SESSION_CLIPS.items():
            const = "kGameClip" + "".join(p.capitalize() for p in name.split("_"))
            self.assertRegex(text, rf"{const} = {cid};")
        self.assertRegex(text, rf"kGameRoundClipMin = {gp.ROUND_CLIP_MIN}")
        self.assertRegex(text, rf"kGameRoundClipMax = {gp.ROUND_CLIP_MAX}")

    def test_game_type_matches_kpb2_reservation(self) -> None:
        # KPG1 has its own magic; its manifest names the KPB2 reserved type.
        self.assertEqual(kpb2.PACKAGE_TYPES["game"], 3)


class Cli(unittest.TestCase):
    def test_demo_and_verify(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out = io.StringIO()
            with redirect_stdout(out):
                self.assertEqual(gp.main(["demo", "--output", tmp]), 0)
            pack = Path(tmp) / "games" / "default.kpg"
            self.assertTrue((Path(tmp) / "README-SMOKE-ONLY.txt").exists())
            with redirect_stdout(out):
                self.assertEqual(gp.main(["verify", str(pack)]), 0)
            self.assertIn("PACKAGE_VALID", out.getvalue())
            pack.write_bytes(pack.read_bytes()[:-2])
            err = io.StringIO()
            with redirect_stderr(err):
                self.assertEqual(gp.main(["verify", str(pack)]), 1)
            self.assertIn("PACKAGE_INVALID", err.getvalue())


if __name__ == "__main__":
    if "--write-golden" in sys.argv:
        GOLDEN.parent.mkdir(parents=True, exist_ok=True)
        GOLDEN.write_bytes(golden_bytes())
        print(f"wrote {GOLDEN}")
    else:
        unittest.main()
