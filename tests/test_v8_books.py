"""V8 books: KPB2 v3 branching + series, bedtime contract, the TuanTuan series.

Everything is synthetic or draft content; nothing here is a reviewed child
book. The device-side behaviour (paging, bedtime, memory) is tested in
tests/cpp/test_branch_book.cc (scripts/p2_test.sh).
"""
from __future__ import annotations

import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools import kpb2, make_branch_test_book, tuantuan_series  # noqa: E402
from tools.story_pack import make_pack  # noqa: E402

FIXTURES = ROOT / "tests" / "fixtures" / "kpb2"
P1 = (ROOT / "scripts" / "p1_integrate.py").read_text(encoding="utf-8")
HEADER_TXT = (ROOT / "firmware/components/kid_app/include/kid_app/book_reader.h").read_text(
    encoding="utf-8")


def reseal(blob: bytearray) -> bytearray:
    data_start = struct.unpack_from("<I", blob, 40)[0]
    struct.pack_into("<I", blob, 60, zlib.crc32(blob[96:data_start]) & 0xFFFFFFFF)
    struct.pack_into("<I", blob, 92, zlib.crc32(blob[:92]) & 0xFFFFFFFF)
    return blob


PAGE_AT = 96
QUESTION_AT = 96 + 6 * 96


def next_at(page: int) -> int:
    return PAGE_AT + (page - 1) * 96 + 64


def goto_at(question: int, option: int) -> int:
    return QUESTION_AT + question * 192 + 32 + option * 48 + 3


class BranchBookTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        make_branch_test_book.build(Path(cls.temp.name))
        cls.good = (Path(cls.temp.name) / "books" / "branch_test.kpb").read_bytes()

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def patched(self, *edits) -> bytes:
        blob = bytearray(self.good)
        for at, value, size in edits:
            fmt = {1: "<B", 2: "<H"}[size]
            struct.pack_into(fmt, blob, at, value)
        return bytes(reseal(blob))

    # ---- format, compatibility ---------------------------------------
    def test_v3_header_links_and_series(self):
        head = kpb2.HEADER.unpack_from(self.good)
        self.assertEqual(head[:5], (b"KPB2", 3, 96, 1, 1))  # version 3, branching flag
        info = kpb2.parse(self.good)
        self.assertEqual((info["version"], info["branching"]), (3, True))
        self.assertEqual(info["series"], {"id": 7, "index": 1, "count": 2, "next_content_id": 902})
        self.assertEqual([p["next"] for p in info["pages"]], [0, 0, 5, 6, kpb2.NEXT_END, 0])
        branch = info["pages"][1]["questions"][0]
        self.assertTrue(branch["branch"])
        self.assertEqual([o["goto"] for o in branch["options"]], [3, 4])
        self.assertFalse(info["pages"][2]["questions"][0]["branch"])

    def test_old_books_are_untouched(self):
        # A book without links or a series still compiles to version 2, byte
        # for byte what V4 shipped (the golden fixtures), and reads as before.
        golden = (FIXTURES / "golden_book.kpb").read_bytes()
        info = kpb2.parse(golden)
        self.assertEqual((info["version"], info["branching"]), (2, False))
        self.assertEqual(info["series"]["id"], 0)
        self.assertEqual({p["next"] for p in info["pages"]}, {0})
        self.assertEqual(kpb2.parse((FIXTURES / "golden_kpb1.kpb").read_bytes())["format"], "KPB1")

    def test_series_without_links_is_v3_but_not_branching(self):
        with tempfile.TemporaryDirectory() as tmp:
            doc = make_branch_test_book.source_document()
            for page in doc["pages"]:
                page.pop("next", None)
                for q in page.get("questions", []):
                    for option in q["options"]:
                        option.pop("goto", None)
            src = make_branch_test_book.write_source(Path(tmp))
            src.write_text(json.dumps(doc, ensure_ascii=False), encoding="utf-8")
            blob, _, _ = kpb2.compile_book(src)
        info = kpb2.parse(blob)
        self.assertEqual((info["version"], info["branching"], info["series"]["id"]), (3, False, 7))

    def test_old_reader_would_refuse_v3(self):
        # V4 readers need version 2: a v3 file is rejected by them instead of
        # being misread (documented, see docs/V8_BOOKS.md).
        self.assertEqual(struct.unpack_from("<H", self.good, 4)[0], 3)

    # ---- validator ----------------------------------------------------
    def test_bad_links_are_rejected(self):
        cases = {
            "target missing": [(goto_at(0, 0), 9, 1)],
            "jump to itself": [(goto_at(0, 0), 2, 1)],
            "jump byte 0x80": [(goto_at(0, 0), 0x80, 1)],
            "only one option jumps": [(goto_at(0, 1), 0, 1)],
            "unreachable page": [(goto_at(0, 1), 3, 1)],
            "loop 4 -> 2": [(next_at(4), 2, 2)],
            "loop 5 -> 1": [(next_at(5), 1, 2)],
            "next missing": [(next_at(1), 40, 2)],
            "next to itself": [(next_at(4), 4, 2)],
            "last page only restarts": [(next_at(6), kpb2.NEXT_RESTART, 2)],
            "restart from page 1": [(next_at(1), kpb2.NEXT_RESTART, 2)],
            "first option restarts": [(goto_at(0, 0), kpb2.GOTO_RESTART, 1), (goto_at(0, 1), 3, 1),
                                      (next_at(3), 0, 2), (next_at(4), 5, 2), (next_at(5), 6, 2)],
            "branch page with next": [(next_at(2), 3, 2)],
            "series next is itself": [(70, 901, 2)],
            "series index past count": [(66, 3, 2)],
            "last book with a next": [(66, 2, 2)],
            "unknown flag": [(10, 3, 2)],
            "v2 with links": [(4, 2, 2)],
        }
        for name, edits in cases.items():
            with self.subTest(name), self.assertRaises(ValueError):
                kpb2.parse(self.patched(*edits))

    def test_restart_is_the_one_allowed_way_back(self):
        blob = self.patched((goto_at(0, 1), kpb2.GOTO_RESTART, 1), (next_at(3), 0, 2),
                            (next_at(4), 5, 2), (next_at(5), 6, 2))
        info = kpb2.parse(blob)
        self.assertEqual([o["goto"] for o in info["pages"][1]["questions"][0]["options"]],
                         [3, kpb2.GOTO_RESTART])

    def test_source_validation_messages(self):
        doc = make_branch_test_book.source_document()

        def compile_with(mutate):
            copy = json.loads(json.dumps(doc))
            mutate(copy)
            with tempfile.TemporaryDirectory() as tmp:
                src = make_branch_test_book.write_source(Path(tmp))
                src.write_text(json.dumps(copy, ensure_ascii=False), encoding="utf-8")
                kpb2.compile_book(src)

        def target_missing(d):
            d["pages"][1]["questions"][0]["options"][0]["goto"] = 12

        def loop(d):
            d["pages"][3]["next"] = 2

        def dead_end_restart(d):
            d["pages"][5]["next"] = "restart"

        def partial(d):
            del d["pages"][1]["questions"][0]["options"][1]["goto"]

        def answer_and_goto(d):
            d["pages"][1]["questions"][0]["answer"] = "left"

        def first_restarts(d):
            q = d["pages"][1]["questions"][0]["options"]
            q[0]["goto"], q[1]["goto"] = "restart", 3
            d["pages"][2]["next"], d["pages"][3]["next"], d["pages"][4]["next"] = 4, 5, 6

        def series(d):
            d["series"]["next_content_id"] = 0

        for mutate, pattern in ((target_missing, "does not exist"), (loop, "reached|loop"),
                                (dead_end_restart, "cannot reach an ending"),
                                (partial, "every option"), (answer_and_goto, "every option"),
                                (first_restarts, "first option"),
                                (series, "Series next_content_id")):
            with self.subTest(mutate.__name__), self.assertRaisesRegex(ValueError, pattern):
                compile_with(mutate)

    def test_sidecar_records_links_and_series(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = make_branch_test_book.write_source(Path(tmp))
            _, side, _ = kpb2.compile_book(src)
        self.assertTrue(side["branching"])
        self.assertEqual(side["series"]["next_content_id"], 902)
        self.assertEqual(side["pages"][1]["questions"][0]["options"][0]["goto"], 3)

    # ---- shelf --------------------------------------------------------
    def test_shelf_keeps_a_series_together(self):
        def book(cid, sid=0, idx=0):
            return {"content_id": cid, "series": {"id": sid, "index": idx}}

        shelf = kpb2.shelf_order([book(20), book(902, 7, 2), book(5), book(901, 7, 1), book(7)])
        self.assertEqual([b["content_id"] for b in shelf], [5, 7, 901, 902, 20])
        self.assertEqual(kpb2.next_in_series(shelf, {"series": {"next_content_id": 902}})["content_id"], 902)
        self.assertIsNone(kpb2.next_in_series(shelf, {"series": {"next_content_id": 0}}))

    # ---- board contract -------------------------------------------------
    def test_board_contract(self):
        # Bedtime comes from the RTC day phase, never from the network.
        self.assertIn("kid_app::BedtimeAvailable(static_cast<kid_app::DayPhase>(phase))", P1)
        self.assertIn("book_.Open(&book_outline_, now, book_missing_, bedtime)", P1)
        # Low contrast is applied to the grey page before the planes are split.
        self.assertLess(P1.index("kid_app::BedtimeTone(grey, grey_bytes)"),
                        P1.index("kid_app::EpdGreyPlanes(grey, grey_bytes"))
        # The end of the bedtime story uses the existing sleep flow (3 s to cancel).
        sleep = P1[P1.index("if (step.sleep) {"):]
        self.assertIn("HandleSleepAction(sleep_flow_.Request(NowMs()))", sleep[:400])
        # Reader memory still only goes through the guard.
        self.assertEqual(P1.count("kid_app::BookMemoryEvent("), 1)
        self.assertIn("DayPhase::Evening", HEADER_TXT)
        # The next-book arrow needs the book on the card (no fopen in the patch).
        self.assertIn("story_sd_.ContentBookPresent(book_outline_.series_next)", P1)


class TuanTuanSeriesTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.out = Path(cls.temp.name)
        cls.results = tuantuan_series.pack_drafts(cls.out)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_three_books_compile_and_validate(self):
        stories = tuantuan_series.load_stories()
        self.assertGreaterEqual(len(stories), 3)
        for story in stories:
            info = kpb2.parse((self.out / f"{story['book_id']}.kpb").read_bytes())
            self.assertTrue(5 <= len(info["pages"]) <= 8, story["book_id"])
            self.assertEqual(info["content_id"], story["content_id"])
            self.assertEqual(info["series"]["id"], 1)

    def test_series_chain_and_a_branch_book(self):
        infos = [kpb2.parse((self.out / f"{s['book_id']}.kpb").read_bytes())
                 for s in tuantuan_series.load_stories()]
        self.assertEqual([i["series"]["index"] for i in infos], [1, 2, 3])
        for current, following in zip(infos, infos[1:]):
            self.assertEqual(current["series"]["next_content_id"], following["content_id"])
        self.assertEqual(infos[-1]["series"]["next_content_id"], 0)
        self.assertEqual([kpb2.shelf_key(i)[2] for i in kpb2.shelf_order(infos)], [1, 2, 3])
        self.assertEqual(len({i["content_id"] for i in infos}), 3)
        road = infos[0]
        self.assertTrue(road["branching"])
        self.assertTrue(any(q["branch"] for p in road["pages"] for q in p["questions"]))

    def test_every_branch_path_ends(self):
        road = kpb2.parse((self.out / "tuantuan_road.kpb").read_bytes())
        links = {}
        for page in road["pages"]:
            branch = [q for q in page["questions"] if q["branch"]]
            links[page["number"]] = (page["next"], [o["goto"] for o in branch[0]["options"]]
                                     if branch else None)

        def walk(page: int) -> list[list[int]]:
            nxt, gotos = links[page]
            if gotos:
                return [[page] + path for g in gotos for path in walk(g)]
            if nxt == kpb2.NEXT_END or (nxt == 0 and page == len(links)):
                return [[page]]
            return [[page] + path for path in walk(nxt or page + 1)]

        paths = walk(1)
        self.assertEqual(paths, [[1, 2, 3, 4, 7], [1, 2, 5, 6, 8]])

    def test_scripts_are_drafts_for_children_ages_3_to_5(self):
        for story in tuantuan_series.load_stories():
            self.assertEqual(story["status"], "draft")
            tuantuan_series.check_story(story)
            for page in story["pages"]:
                self.assertLessEqual(len(page["text"]), 60)
                self.assertNotIn("\n", page["text"])

    def test_builder_never_pre_approves(self):
        with tempfile.TemporaryDirectory() as tmp:
            for story in tuantuan_series.load_stories():
                source = tuantuan_series.build_source(story, Path(tmp) / story["book_id"], "tone")
                doc = json.loads(source.read_text(encoding="utf-8"))
                self.assertFalse(doc["approved"])
                flags = [p["approved"] for p in doc["pages"]]
                flags += [q["approved"] for p in doc["pages"] for q in p.get("questions", [])]
                self.assertFalse(any(flags))
                with self.assertRaises(ValueError):  # the packer refuses an unreviewed book
                    make_pack(source, Path(tmp) / "x.kpb", reviewed=True)
                status = json.loads((source.parent / "DRAFT_STATUS.json").read_text(encoding="utf-8"))
                self.assertTrue(status["needs_adult_review"])

    def test_like_question_only_for_whitelisted_topic(self):
        sleep = kpb2.parse((self.out / "tuantuan_sleep.kpb").read_bytes())
        like = [q for p in sleep["pages"] for q in p["questions"]]
        self.assertEqual(len(like), 1)
        self.assertEqual(like[0]["like_slot"], kpb2.PREFERENCE_TOPICS["favorite_toy"][0])


if __name__ == "__main__":
    unittest.main()
