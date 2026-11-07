"""V8 sound packs: catalogue limits, KPS1 round trip, and the song / routine contracts."""
import json
import re
import tempfile
import unittest
from pathlib import Path

from tools import sound_extras as se
from tools import sound_pack as sp

ROOT = Path(__file__).resolve().parents[1]
KID = ROOT / "firmware/components/kid_app"


class Catalogue(unittest.TestCase):
    def test_limits(self):
        se.validate_catalog()
        self.assertEqual(set(se.PACKS), {"routines", "guess_animals", "guess_vehicles",
                                         "guess_music", "guess_daily"})

    def test_guess_packs_have_the_four_categories_with_enough_sounds(self):
        for pack in ("guess_animals", "guess_vehicles", "guess_music", "guess_daily"):
            self.assertGreaterEqual(len(se.items(pack)), 8, pack)
            self.assertEqual([i.id for i in se.items(pack)], list(range(1, len(se.items(pack)) + 1)))

    def test_routine_clip_ids_match_the_firmware(self):
        header = (KID / "include/kid_app/routine_timer.h").read_text(encoding="utf-8")
        self.assertIn("1..3 brush start/middle/done, 11..13 wash", header)
        by_name = {i.name: i.id for i in se.items("routines")}
        for name, cid in {"brush_start": 1, "brush_middle": 2, "brush_done": 3,
                          "wash_start": 11, "wash_middle": 12, "wash_done": 13}.items():
            self.assertEqual(by_name[name], cid)

    def test_brush_prompt_says_the_required_cue(self):
        text = {i.name: i.text for i in se.items("routines")}
        self.assertIn("刷刷里面", text["brush_middle"])

    def test_catalog_json_is_machine_readable(self):
        data = se.catalog_json()
        json.dumps(data, ensure_ascii=False)
        self.assertEqual(data["packs"]["guess_animals"]["clips"][0]["name"], "cat")

    def test_checklist_lists_every_item(self):
        text = se.checklist()
        for pack in se.PACKS.values():
            for item in pack:
                self.assertIn(f"| {item.name} |", text)

    def test_unknown_pack_is_an_error(self):
        with self.assertRaises(ValueError):
            se.items("nope")


class PackRoundTrip(unittest.TestCase):
    def test_demo_packs_decode_with_the_kps1_reader(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = se.demo(Path(tmp))
            for pack, manifest in result.items():
                self.assertTrue(manifest["placeholder"])
                revision, clips = sp.decode((Path(tmp) / "sounds" / f"{pack}.kps").read_bytes())
                self.assertEqual(sorted(clips), [i.id for i in se.items(pack)])

    def test_unreviewed_and_misnamed_are_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(ValueError):
                se.make_pack(Path(tmp), Path(tmp) / "routines.kps", "routines", reviewed=False)
            with self.assertRaises(ValueError):
                se.make_pack(Path(tmp), Path(tmp) / "other.kps", "routines", reviewed=True)


class SongFormat(unittest.TestCase):
    """Songs reuse the KPB book format: the player may not invent a new container."""

    def test_song_player_reads_no_files_and_names_no_new_magic(self):
        code = ((KID / "src/song_player.cc").read_text(encoding="utf-8")
                + (KID / "include/kid_app/song_player.h").read_text(encoding="utf-8"))
        stripped = re.sub(r"//[^" + chr(92) + "n]*", "", code)
        for needle in ("fopen", "FILE", "KPS1", "KPSG", "<cstdio>"):
            self.assertNotIn(needle, stripped)
        self.assertIn("KPB book", code)


if __name__ == "__main__":
    unittest.main()
