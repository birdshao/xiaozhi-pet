"""V4-05 picture-book reader: test book tool, board overlay contract and the
hardware self-test scenario. The state machine, like guard, overlays and
grey planes are host-tested in tests/cpp/test_book_reader.cc (scripts/p2_test.sh).
"""
from __future__ import annotations

from pathlib import Path
import re
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from scripts.p1_integrate import (  # noqa: E402
    BOARD_WORKER, DISPLAY_IMPL, patch_board, patch_display_h,
)
from tools import kpb2  # noqa: E402
from tools.make_reader_test_book import BOOK_ID, CONTENT_ID, build  # noqa: E402
from tools.selftest_report import expectations, sector_bytes  # noqa: E402

KID_APP = ROOT / "firmware" / "components" / "kid_app"
SCENARIO = ROOT / "tools" / "selftest" / "v4_book_reader.pettest"


def _body(source: str, start: str) -> str:
    body = source[source.index(start):]
    return body[:body.index("\n    }\n")]


class ReaderTestBookTests(unittest.TestCase):
    def test_tool_builds_a_valid_reproducible_kpb2_book(self):
        with tempfile.TemporaryDirectory() as tmp:
            first = build(Path(tmp) / "a")
            second = build(Path(tmp) / "b")
            blob = (Path(tmp) / "a" / "books" / f"{BOOK_ID}.kpb").read_bytes()
            self.assertEqual(blob, (Path(tmp) / "b" / "books" / f"{BOOK_ID}.kpb").read_bytes())
            self.assertEqual(first["sha256"], second["sha256"])
            # Only the book goes to the card folder; the readme says what to copy.
            self.assertEqual(sorted(p.name for p in (Path(tmp) / "a" / "books").iterdir()),
                             [f"{BOOK_ID}.kpb"])
            readme = (Path(tmp) / "a" / "README-READER-TEST-ONLY.txt").read_text(encoding="utf-8")
            self.assertIn(f"/books/{BOOK_ID}.kpb", readme)
            self.assertIn("NOT CHILD CONTENT", readme)
        parsed = kpb2.parse(blob)
        self.assertEqual(blob[:4], b"KPB2")
        self.assertEqual(parsed["content_id"], CONTENT_ID)
        pages = parsed["pages"]
        self.assertEqual(len(pages), 3)
        self.assertEqual([bool(p["grey"]) for p in pages], [True, True, False])
        questions = pages[0]["questions"]
        self.assertEqual(len(questions), 2)
        self.assertEqual(questions[0]["like_slot"], 2)  # favorite_color
        self.assertEqual(questions[0]["correct"], 0)
        self.assertEqual(questions[1]["correct"], 1)    # circle
        self.assertEqual(questions[1]["like_slot"], 0)
        self.assertLess(len(blob), 1024 * 1024)

    def test_scenario_selects_the_test_book_and_fits_the_sector(self):
        text = SCENARIO.read_text(encoding="ascii")
        self.assertEqual(len(sector_bytes(text)), 4096)
        steps = [line.split() for line in text.splitlines()
                 if line.strip() and not line.lstrip().startswith("#")]
        self.assertLessEqual(len(steps) - 2, 64)  # header and end are not steps
        ops = [s[0] for s in steps]
        self.assertLess(ops.index("book"), ops.index("pet"))  # card read on first Pet start
        self.assertEqual(steps[ops.index("book")], ["book", BOOK_ID])
        exp = expectations(text)
        for name in ("min:like=2:3", "min:kind=finished", "min:how=grey", "min:kind=prompt",
                     "min:result=wrong", "none:PET_BOOK_DRAW_FAILED", "commits"):
            self.assertIn(name, exp)
        self.assertEqual(exp["commits"], "2")  # StartStory + StopStory only


class BoardContractTests(unittest.TestCase):
    def test_official_4grey_sequence_and_partial_previous_image(self):
        grey = DISPLAY_IMPL[DISPLAY_IMPL.index("bool CustomEpdDisplay::ShowBookGrey("):]
        grey = grey[:grey.index("\n}\n")]
        order = ["EPD_Init();", "EPD_SendCommand(0x1A);", "EPD_SendData(0x5A);",
                 "EPD_SendCommand(0x24);\n            writeBytes(ram24, n);",
                 "EPD_SendCommand(0x26);\n            writeBytes(ram26, n);",
                 "EPD_SendData(0xD7);", "read_busy();", "EPD_Init();",
                 "writeBytes(buffer, n);"]
        at = 0
        for item in order:
            at = grey.index(item, at) + len(item)
        part = DISPLAY_IMPL[DISPLAY_IMPL.index("bool CustomEpdDisplay::ShowBookPartial("):]
        part = part[:part.index("\n}\n")]
        self.assertLess(part.index("writeBytes(previous, n);"), part.index("EPD_TurnOnDisplayPart();"))
        self.assertLess(part.index("EPD_TurnOnDisplayPart();"), part.rindex("writeBytes(buffer, n);"))
        for fn in ("ShowBookMono", "ShowBookGrey", "ShowBookPartial"):
            self.assertIn("lvgl_port_lock(5000)", DISPLAY_IMPL[DISPLAY_IMPL.index(fn + "("):])
        header = patch_display_h('#include "lcd_display.h"\n    void EPD_Sleep();\n'
                                 "    uint8_t* buffer = NULL;\n")
        self.assertIn("bool ShowBookGrey(const uint8_t* ram24, const uint8_t* ram26, "
                      "const uint8_t* mono);", header)
        self.assertEqual(header.count("void ExitPetPreview();"), 1)

    def test_grey_planes_match_the_official_example(self):
        src = (KID_APP / "src" / "book_reader.cc").read_text(encoding="utf-8")
        body = src[src.index("bool EpdGreyPlanes("):]
        self.assertIn("SplitGreyPlanes(grey, grey_bytes, ram26, ram24)", body)
        self.assertIn("ram24[i] = static_cast<uint8_t>(~ram24[i]);", body)

    def test_dial_drives_the_book_and_only_home_is_durable(self):
        handle = _body(BOARD_WORKER, "void HandleInputEvent(")
        book = handle.index("book_.Active()")
        self.assertLess(handle.index("if (!pet_mode_.load()) return;"), book)
        self.assertLess(book, handle.index("PetInputRouter::DecideDial("))
        self.assertLess(handle.index("sleep_flow_.Active()"), book)  # sleep wins
        self.assertLess(handle.index("parent_menu_.IsOpen()"), book)
        apply = _body(BOARD_WORKER, "void ApplyBookStep(")
        self.assertEqual(re.findall(r"pet_service_\.\w+\(", apply), ["pet_service_.PostLocal("])
        self.assertIn("kid_app::PetAction::StopStory", apply)
        self.assertNotIn("NextPage", apply)
        # The committed reader scene keeps the open book's page.
        self.assertIn("if (scene.kind == kid_app::SceneKind::Reader) scene = "
                      "self->BookSceneLocked();", BOARD_WORKER)
        loop = _body(BOARD_WORKER, "void InputTaskLoop()")
        self.assertIn("UpdateBook();", loop)
        update = _body(BOARD_WORKER, "void UpdateBook()")
        for gate in ("sleep_active_.load()", "napping_.load()", "ParentMenuOpen()",
                     "book_outline_ready_.load()"):
            self.assertIn(gate, update)

    def test_likes_only_through_the_kid_app_guard(self):
        # ADR-012: no board code builds a preference itself.
        self.assertNotIn("ChoiceMemoryEvent", BOARD_WORKER)
        self.assertNotIn("PreferenceChosen;", BOARD_WORKER)
        self.assertNotIn("MemorySource::DialChoice", BOARD_WORKER)
        record = _body(BOARD_WORKER, "void RecordBookMemory(")
        self.assertIn("kid_app::BookMemoryEvent(book_outline_, request,", record)
        self.assertEqual(BOARD_WORKER.count("RecordBookMemory("), 2)  # definition + one call
        self.assertIn("if (step.memory.kind != kid_app::BookMemoryKind::None) "
                      "RecordBookMemory(step.memory);", BOARD_WORKER)
        reader = (KID_APP / "src" / "book_reader.cc").read_text(encoding="utf-8")
        # The reader asks for a like only on a dial press.
        press = reader[reader.index("if (kind != InputKind::DialPress) return step;"):]
        press = press[:press.index("return step;\n        }")]
        self.assertIn("step.memory = {BookMemoryKind::Like", press)
        self.assertEqual(reader.count("BookMemoryKind::Like,"), 1)
        guard = reader[reader.index("bool BookMemoryEvent("):]
        self.assertIn("return ChoiceMemoryEvent(q, request.option, event_id, day, out);", guard)

    def test_sd_reads_stay_off_the_input_task(self):
        for fn in ("void UpdateBook()", "void ApplyBookStep(", "void PlayBookClip(",
                   "void RecordBookMemory("):
            body = _body(BOARD_WORKER, fn)
            for io in ("Reader()", "Content()", "ReadFrame", "ReadGrey", "Question("):
                self.assertNotIn(io, body, f"{fn} must not read the card ({io})")
        init = _body(BOARD_WORKER, "bool InitializePetRuntime()")
        self.assertLess(init.index("story_sd_.Initialize(story_book_)"),
                        init.index("BuildBookOutline("))
        self.assertLess(init.index("BuildBookOutline("), init.index("book_outline_ready_.store(true)"))
        self.assertIn('story_pcm_.Start("/sdcard/books", story_book_,', init)
        self.assertIn('char story_book_[16] = "forest_friends";', BOARD_WORKER)

    def test_selftest_book_step_only_before_pet_starts(self):
        case = BOARD_WORKER[BOARD_WORKER.index("case kid_app::SelfTestOp::Book:"):]
        case = case[:case.index("break;\n                case") if "break;\n                case" in case
                    else case.index("break;")]
        self.assertIn("pet_runtime_ready_.load()", case)
        self.assertIn("std::memcpy(story_book_, step.name, sizeof(story_book_));", case)
        header = (KID_APP / "include" / "kid_app" / "selftest_script.h").read_text(encoding="utf-8")
        self.assertIn("char name[16]", header)

    def test_board_build_lists_new_sources_and_headers(self):
        cmake = (KID_APP / "CMakeLists.txt").read_text(encoding="utf-8")
        for name in ("src/book_reader.cc", "src/icon_choice.cc"):
            self.assertIn(name, cmake)
        patched = patch_board("#include <stdio.h>\n    void InitializeButtons() {\n"
                              "            app.ToggleChatState();\n"
                              "        pwr_button_.OnClick([this]() {\n")
        self.assertIn('#include "kid_app/book_reader.h"', patched)
        p2 = (ROOT / "scripts" / "p2_test.sh").read_text(encoding="utf-8")
        self.assertIn("tests/cpp/test_book_reader.cc", p2)
        self.assertIn("tools.make_reader_test_book", p2)


if __name__ == "__main__":
    unittest.main()
