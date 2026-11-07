"""Python memory/settings decoders against the C++ golden vectors (§5.2, §10).

tests/cpp/test_memory_golden.cc checks in CI that every file in
tests/fixtures/memory is exactly what the device codecs produce or reject.
"""
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from services.pet_learning.memory_codec import (  # noqa: E402
    CodecError, Day, ParentSettings, decode_memory, decode_parent_settings)

FIXTURES = ROOT / "tests" / "fixtures" / "memory"
VALID_MEMORY = ("empty", "rich", "full", "wiped", "capped", "limits", "saturated", "seven_days")
VALID_SETTINGS = ("default", "custom")


class MemoryGoldenTest(unittest.TestCase):
    def test_every_fixture_is_classified(self):
        names = {p.name for p in FIXTURES.iterdir()}
        expected = ({f"memory_{n}.bin" for n in VALID_MEMORY}
                    | {f"memory_{n}.summary.json" for n in VALID_MEMORY}
                    | {f"settings_{n}.bin" for n in VALID_SETTINGS})
        corrupt = {n for n in names if n.startswith(("memory_bad_", "settings_bad_"))}
        self.assertEqual(names - corrupt, expected)
        self.assertGreaterEqual(len([n for n in corrupt if n.startswith("memory_")]), 12)

    def test_valid_vectors_round_trip_and_match_device_summary(self):
        for name in VALID_MEMORY:
            with self.subTest(name):
                blob = (FIXTURES / f"memory_{name}.bin").read_bytes()
                memory = decode_memory(blob)
                self.assertEqual(memory.encode(), blob)
                device = json.loads((FIXTURES / f"memory_{name}.summary.json").read_text("utf-8"))
                self.assertEqual(memory.minimal_summary(), device)

    def test_rich_vector_fields(self):
        m = decode_memory((FIXTURES / "memory_rich.bin").read_bytes())
        self.assertEqual(m.trait, "curious")
        self.assertEqual(m.today, Day(20761, 2, 1, 1, 1, 1, 9, 5))
        self.assertEqual(m.yesterday, Day(20760, 1, 0, 1, 0, 1, 17, 0))
        self.assertEqual([(p.slot, p.option) for p in m.preferences], [(7, 2), (3, 4)])
        self.assertEqual(m.recent_books, (9, 17))
        self.assertEqual((m.growth.books_finished, m.growth.active_days, m.growth.milestones), (2, 2, 41))
        # A stale "today" is reported as zero, like MakeMinimalSummary(current_day).
        self.assertEqual(m.minimal_summary(20762)["today"], {"feed": 0, "greet": 0, "books": 0, "games": 0})

    def test_full_and_wiped_vectors(self):
        full = decode_memory((FIXTURES / "memory_full.bin").read_bytes())
        self.assertEqual(len(full.preferences), 16)
        self.assertEqual(full.preferences[0].slot, 17)
        self.assertNotIn(1, [p.slot for p in full.preferences])
        self.assertEqual(full.recent_books, (5, 9, 8, 7, 6, 4, 3, 2))
        wiped = decode_memory((FIXTURES / "memory_wiped.bin").read_bytes())
        self.assertTrue(wiped.empty())
        self.assertEqual(wiped.trait, "lively")

    def test_limit_saturation_and_milestone_vectors(self):
        limits = decode_memory((FIXTURES / "memory_limits.bin").read_bytes())
        self.assertEqual((limits.today.day, limits.yesterday.day), (0xFFFFFFFF, 0xFFFFFFFE))
        self.assertEqual((limits.today.last_book, limits.today.last_game), (0xFFFF, 0xFFFF))
        self.assertEqual(limits.preferences[0].source_event, 2 ** 64 - 1)
        self.assertEqual((limits.preferences[0].slot, limits.preferences[0].option), (0xFFFF, 0xFFFF))
        saturated = decode_memory((FIXTURES / "memory_saturated.bin").read_bytes())
        self.assertEqual(saturated.today.counts(),
                         {"feeds": 250, "greetings": 250, "books": 250, "games": 250, "choices": 250})
        self.assertEqual((saturated.growth.books_finished, saturated.growth.games_completed), (65535, 65535))
        self.assertEqual(saturated.growth.milestones, 0b0111111)  # through ten_games
        seven = decode_memory((FIXTURES / "memory_seven_days.bin").read_bytes())
        self.assertEqual(seven.growth.active_days, 7)
        self.assertEqual(seven.growth.milestones, (1 << 6) | (1 << 3))  # seven_days, first_game
        self.assertEqual(seven.yesterday, Day(105, 1, 0, 0, 1, 0, 0, 3))

    def test_corrupt_memory_vectors_are_rejected(self):
        for path in sorted(FIXTURES.glob("memory_bad_*.bin")):
            with self.subTest(path.name), self.assertRaises(CodecError) as caught:
                decode_memory(path.read_bytes())
            if "format" in path.name:
                self.assertEqual(caught.exception.code, "MEMORY_FORMAT_UNSUPPORTED")

    def test_valid_settings_vectors(self):
        custom = decode_parent_settings((FIXTURES / "settings_custom.bin").read_bytes())
        self.assertEqual(custom, ParentSettings(390, 1140, 1200, 390, True, 780, 870, 70, 0, False))
        for name in VALID_SETTINGS:
            blob = (FIXTURES / f"settings_{name}.bin").read_bytes()
            self.assertEqual(decode_parent_settings(blob).encode(), blob)
        self.assertEqual(decode_parent_settings((FIXTURES / "settings_default.bin").read_bytes()),
                         ParentSettings())

    def test_corrupt_settings_vectors_are_rejected(self):
        for path in sorted(FIXTURES.glob("settings_bad_*.bin")):
            with self.subTest(path.name), self.assertRaises(CodecError) as caught:
                decode_parent_settings(path.read_bytes())
            if "version" in path.name:
                self.assertEqual(caught.exception.code, "SETTINGS_VERSION_UNSUPPORTED")

    def test_non_bytes_and_longer_buffers_are_rejected(self):
        blob = (FIXTURES / "memory_rich.bin").read_bytes()
        for bad in (blob + b"\0", "x" * 288, None):
            with self.assertRaises(CodecError):
                decode_memory(bad)
        with self.assertRaises(CodecError):
            decode_parent_settings((FIXTURES / "settings_default.bin").read_bytes() + b"\0")


if __name__ == "__main__":
    unittest.main()
