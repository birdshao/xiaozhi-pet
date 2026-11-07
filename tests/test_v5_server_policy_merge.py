"""S3: the conservative merge of parent-panel and device settings (plan §5.4)."""
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from services.pet_learning.memory_codec import ParentSettings  # noqa: E402
from services.pet_learning.policy import MergeError, merge_settings, quiet_hull  # noqa: E402

BASE = ParentSettings()  # quiet 20:30-07:00, volume 100/30, motion on


def at(h, m=0):
    return h * 60 + m


def variant(**changes):
    return ParentSettings(**{**BASE.as_dict(), **changes})


class QuietHullTests(unittest.TestCase):
    def test_union_of_overlapping_and_wrapping_windows(self):
        for a, b, expected in (
                ((at(20, 30), at(7)), (at(21), at(8)), (at(20, 30), at(8))),      # overlap across midnight
                ((at(20, 30), at(7)), (at(20, 30), at(7)), (at(20, 30), at(7))),  # identical
                ((at(20, 30), at(7)), (at(22), at(6)), (at(20, 30), at(7))),      # one inside the other
                ((at(13), at(14)), (at(13, 30), at(15)), (at(13), at(15))),       # same-day overlap
        ):
            self.assertEqual(quiet_hull(a, b), expected, (a, b))
            self.assertEqual(quiet_hull(b, a), expected, (b, a))

    def test_disjoint_windows_cover_the_gap_not_the_whole_day(self):
        self.assertEqual(quiet_hull((at(20), at(21)), (at(22), at(23))), (at(20), at(23)))

    def test_whole_day_has_no_window(self):
        self.assertIsNone(quiet_hull((at(1), at(14)), (at(13), at(2))))


class MergeTableTests(unittest.TestCase):
    CASES = (
        ("volume takes the lower value, device lower",
         {"volume_cap_pct": 80}, {"volume_cap_pct": 60}, {"volume_cap_pct": 60}),
        ("volume takes the lower value, panel lower",
         {"volume_cap_pct": 40}, {"volume_cap_pct": 70}, {"volume_cap_pct": 40}),
        ("quiet volume takes the lower value",
         {"quiet_volume_pct": 20}, {"quiet_volume_pct": 10}, {"quiet_volume_pct": 10}),
        ("motion off wins when the device turned it off",
         {"volume_cap_pct": 90}, {"motion_enabled": False}, {"motion_enabled": False, "volume_cap_pct": 90}),
        ("motion off wins when the panel turned it off",
         {"motion_enabled": False}, {"volume_cap_pct": 90}, {"motion_enabled": False, "volume_cap_pct": 90}),
        ("quiet hours are the union",
         {"quiet_start_min": at(21), "quiet_end_min": at(8), "wake_min": at(8, 30)},
         {"quiet_start_min": at(20), "quiet_end_min": at(7, 30), "wake_min": at(8, 30)},
         {"quiet_start_min": at(20), "quiet_end_min": at(8)}),
        ("a field only the device changed keeps the device value",
         {"volume_cap_pct": 90}, {"wake_min": at(7, 30)}, {"wake_min": at(7, 30), "volume_cap_pct": 90}),
        ("a field only the panel changed keeps the panel value",
         {"wake_min": at(8)}, {"volume_cap_pct": 70}, {"wake_min": at(8), "volume_cap_pct": 70}),
        ("a schedule field both changed: the panel wins",
         {"wake_min": at(8)}, {"wake_min": at(9)}, {"wake_min": at(8)}),
        ("both sides raised nothing: volume unchanged",
         {"wake_min": at(8)}, {"wake_min": at(9)}, {"volume_cap_pct": 100}),
    )

    def test_table(self):
        for name, panel, device, expected in self.CASES:
            with self.subTest(name):
                merged = merge_settings(variant(**panel), variant(**device), BASE)
                self.assertTrue(merged.valid())
                for field, value in expected.items():
                    self.assertEqual(getattr(merged, field), value, field)

    def test_merge_is_never_less_conservative_than_either_side(self):
        pairs = ((dict(volume_cap_pct=80, quiet_volume_pct=20, motion_enabled=True),
                  dict(volume_cap_pct=60, quiet_volume_pct=30, motion_enabled=False)),
                 (dict(volume_cap_pct=50, quiet_volume_pct=0), dict(volume_cap_pct=100, quiet_volume_pct=10)))
        for panel, device in pairs:
            a, b = variant(**panel), variant(**device)
            m = merge_settings(a, b, BASE)
            self.assertLessEqual(m.volume_cap_pct, min(a.volume_cap_pct, b.volume_cap_pct))
            self.assertLessEqual(m.quiet_volume_pct, min(a.quiet_volume_pct, b.quiet_volume_pct))
            self.assertFalse(m.motion_enabled and not (a.motion_enabled and b.motion_enabled))

    def test_schedule_broken_by_the_union_falls_back_to_a_valid_one(self):
        # Union quiet hours start 19:45, which swallows the panel's evening (19:30): the
        # device's earlier evening schedule is the valid fallback.
        merged = merge_settings(variant(), variant(quiet_start_min=at(19, 45), evening_min=at(19, 15)), BASE)
        self.assertTrue(merged.valid())
        self.assertEqual(merged.quiet_start_min, at(19, 45))
        self.assertEqual(merged.evening_min, at(19, 15))

    def test_no_single_quiet_window_fails_closed(self):
        with self.assertRaises(MergeError):
            merge_settings(variant(quiet_start_min=at(20), quiet_end_min=at(2), wake_min=at(5)),
                           variant(quiet_start_min=at(1), quiet_end_min=at(20), wake_min=at(5),
                                   evening_min=at(18)), BASE)


if __name__ == "__main__":
    unittest.main()
