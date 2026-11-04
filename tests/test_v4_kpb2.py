"""V4-04/V4-09 KPB2 compiler, strict validator and the C++ device reader.

Everything here is synthetic (PGM images, sawtooth tones) and is NOT child
content. Runs without Pillow. Regenerate the golden fixtures after an
intentional format change with:
    python tests/test_v4_kpb2.py --write-golden
"""
from __future__ import annotations

import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import wave
import zlib

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools import kpb2  # noqa: E402
from tools.story_pack import make_pack  # noqa: E402

FIXTURES = ROOT / "tests" / "fixtures" / "kpb2"
NOT_CHILD = "（测试样例，非儿童内容）"


def _wav(path: Path, samples: int, step: int) -> None:
    pcm = b"".join(struct.pack("<h", ((n * step) % 4000) - 2000) for n in range(samples))
    with wave.open(str(path), "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(24000)
        wf.writeframes(pcm)


def _icon(path: Path, size: int, seed: int) -> None:
    lum = bytearray(size * size)
    for y in range(size):
        for x in range(size):
            border = x < 4 or y < 4 or x >= size - 4 or y >= size - 4
            mark = (x + y * seed) % (seed + 5) == 0
            lum[y * size + x] = 0 if border or mark else 255
    kpb2.write_pgm(path, size, size, bytes(lum))


def write_sample_book(folder: Path) -> Path:
    """Two pages: page 1 grey (4 bands) with a like question and a quiz,
    page 2 mono (threshold) without questions."""
    folder.mkdir(parents=True, exist_ok=True)
    bands = bytes((0, 85, 170, 255)[x // 200] for x in range(800))
    kpb2.write_pgm(folder / "page1.pgm", 800, 480, bands * 480)
    mono = bytearray(b"\xff" * 800 * 480)
    mono[0] = 0
    for y in range(100, 200):
        mono[y * 800 + 300:y * 800 + 500] = b"\0" * 200
    kpb2.write_pgm(folder / "page2.pgm", 800, 480, bytes(mono))
    _wav(folder / "page1.wav", 2400, 7)
    _wav(folder / "page2.wav", 2400, 11)
    _wav(folder / "q1.wav", 1200, 13)
    _wav(folder / "q2.wav", 1200, 17)
    for i, (name, size) in enumerate((("rabbit", 64), ("bird", 64), ("skip", 48),
                                      ("circle", 96), ("square", 96)), 1):
        _icon(folder / f"{name}.pgm", size, i)
        _wav(folder / f"{name}.wav", 480, 19 + i)
    book = {
        "schema_version": 2, "package_type": "book", "book_id": "golden_book",
        "content_id": 101, "revision": 1, "title": "灰度样例书" + NOT_CHILD,
        "reviewer": "CI 测试", "approved": True,
        "pages": [
            {"number": 1, "text": "四条灰色带子。" + NOT_CHILD, "approved": True,
             "image": "page1.pgm", "audio": "page1.wav", "image_mode": "grey",
             "questions": [
                 {"prompt": "你最喜欢哪个小动物？", "prompt_audio": "q1.wav",
                  "approved": True, "like_topic": "favorite_animal",
                  "options": [
                      {"key": "rabbit", "label": "小兔子", "icon": "rabbit.pgm",
                       "audio": "rabbit.wav", "x": 96, "y": 320, "like": True},
                      {"key": "bird", "label": "小鸟", "icon": "bird.pgm",
                       "audio": "bird.wav", "x": 336, "y": 320, "like": True},
                      {"key": "skip", "label": "都喜欢", "icon": "skip.pgm",
                       "audio": "skip.wav", "x": 576, "y": 336},
                  ]},
                 {"prompt": "哪个是圆形？", "prompt_audio": "q2.wav", "approved": True,
                  "answer": "circle", "voice_answer": True,
                  "options": [
                      {"key": "circle", "label": "圆形", "icon": "circle.pgm",
                       "audio": "circle.wav", "x": 160, "y": 200},
                      {"key": "square", "label": "方形", "icon": "square.pgm",
                       "audio": "square.wav", "x": 480, "y": 200},
                  ]},
             ]},
            {"number": 2, "text": "黑色方块。" + NOT_CHILD, "approved": True,
             "image": "page2.pgm", "audio": "page2.wav", "image_mode": "mono"},
        ],
    }
    source = folder / "book.json"
    source.write_text(json.dumps(book, ensure_ascii=False, indent=1), encoding="utf-8")
    return source


def write_sample_kpb1(folder: Path) -> Path:
    folder.mkdir(parents=True, exist_ok=True)
    lum = bytearray(b"\xff" * 800 * 480)
    lum[0] = 0
    kpb2.write_pgm(folder / "p1.pgm", 800, 480, bytes(lum))
    _wav(folder / "p1.wav", 2400, 5)
    doc = {"schema_version": 1, "book_id": "golden_kpb1", "title": "黑白样例" + NOT_CHILD,
           "approved": True, "pages": [{"number": 1, "text": "一页。" + NOT_CHILD,
                                        "approved": True, "image": "p1.pgm",
                                        "audio": "p1.wav"}]}
    source = folder / "book.json"
    source.write_text(json.dumps(doc, ensure_ascii=False), encoding="utf-8")
    return source


def refresh_crcs(blob: bytearray) -> bytearray:
    """Re-seal tables and header so a test reaches the semantic checks."""
    data_start = struct.unpack_from("<I", blob, 40)[0]
    struct.pack_into("<I", blob, 60, zlib.crc32(blob[96:data_start]) & 0xFFFFFFFF)
    struct.pack_into("<I", blob, 92, zlib.crc32(blob[:92]) & 0xFFFFFFFF)
    return blob


QUESTION_AT = 96 + 2 * 96  # golden: two page records, then questions


class Kpb2Tests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.base = Path(self.temp.name)
        self.source = write_sample_book(self.base / "src")
        self.output = self.base / "out" / "golden_book.kpb"
        self.original = self.source.read_text(encoding="utf-8")

    def tearDown(self):
        self.temp.cleanup()

    def _doc(self) -> dict:
        return json.loads(self.original)

    def _save(self, doc: dict) -> None:
        self.source.write_text(json.dumps(doc, ensure_ascii=False), encoding="utf-8")

    def _rejects(self, doc: dict, pattern: str) -> None:
        self._save(doc)
        with self.assertRaisesRegex(ValueError, pattern):
            make_pack(self.source, self.output, reviewed=True)
        self.assertFalse(self.output.exists())

    def test_golden_fixture_is_reproducible(self):
        meta = make_pack(self.source, self.output, reviewed=True)
        golden = FIXTURES / "golden_book.kpb"
        self.assertEqual(self.output.read_bytes(), golden.read_bytes(),
                         "KPB2 bytes drifted: regenerate only for an intended format change")
        self.assertEqual(meta["sha256"], json.loads(
            (FIXTURES / "golden_book.manifest.json").read_text(encoding="utf-8"))["sha256"])
        kpb1 = self.base / "out" / "golden_kpb1.kpb"
        make_pack(write_sample_kpb1(self.base / "k1"), kpb1, reviewed=True)
        self.assertEqual(kpb1.read_bytes(), (FIXTURES / "golden_kpb1.kpb").read_bytes())

    def test_layout_grey_dither_and_questions(self):
        meta = make_pack(self.source, self.output, reviewed=True)
        blob = self.output.read_bytes()
        head = kpb2.HEADER.unpack_from(blob)
        self.assertEqual(head[:11], (b"KPB2", 2, 96, 1, 0, 800, 480, 101, 1, 2, 2))
        info = kpb2.parse(blob)
        self.assertEqual(info["title"], "灰度样例书" + NOT_CHILD)
        page1, page2 = info["pages"]
        self.assertEqual(len(page1["grey"]), kpb2.GREY_BYTES)
        self.assertIsNone(page2["grey"])
        levels = kpb2.unpack_grey(page1["grey"])
        self.assertEqual([levels[x] for x in (0, 199, 200, 400, 799)], [0, 0, 1, 2, 3])
        self.assertEqual(page1["grey"][0], 0x00)
        self.assertEqual(page1["grey"][50], 0x55)   # x=200..203 level 1
        self.assertEqual(page1["grey"][199], 0xFF)
        white = kpb2.unpack_bits(page1["mono"])
        # level 1 white only on even row+even column; level 2 black only on odd/odd.
        self.assertEqual([white[200], white[201], white[1000], white[1001]], [1, 0, 0, 0])
        self.assertEqual([white[400], white[1201]], [1, 0])
        mono2 = kpb2.unpack_bits(page2["mono"])
        self.assertEqual((mono2[0], mono2[1], mono2[150 * 800 + 400]), (0, 1, 0))
        like, quiz = page1["questions"]
        self.assertEqual((like["like_slot"], like["correct"], like["voice"]), (1, 0, False))
        self.assertEqual([(o["id"], o["like"]) for o in like["options"]],
                         [(3, True), (4, True), (0xFF03, False)])
        self.assertEqual(like["options"][2]["box"], (576, 336, 48, 48))
        self.assertEqual((quiz["like_slot"], quiz["correct"], quiz["voice"]), (0, 1, True))
        self.assertEqual([o["id"] for o in quiz["options"]], [0xFF01, 0xFF02])
        self.assertEqual(quiz["options"][0]["label_text"], "圆形")
        side = meta["pages"][0]["questions"][0]["options"]
        self.assertEqual([o["writes_like"] for o in side], [True, True, False])
        self.assertEqual(meta["reviewer"], "CI 测试")

    def test_malformed_packages_rejected(self):
        make_pack(self.source, self.output, reviewed=True)
        good = bytearray(self.output.read_bytes())
        data_start = struct.unpack_from("<I", good, 40)[0]

        def bad(mutate, pattern):
            blob = bytearray(good)
            mutate(blob)
            with self.assertRaisesRegex(ValueError, pattern):
                kpb2.parse(bytes(blob))

        bad(lambda b: b.__setitem__(5, 3), "header")                    # version
        bad(lambda b: refresh_crcs(b).__setitem__(92, b[92] ^ 1), "header")
        bad(lambda b: b.__setitem__(96, 9), "table CRC")                 # page number
        bad(lambda b: b.__setitem__(len(b) - 1, b[-1] ^ 1), "CRC")       # last label
        bad(lambda b: b.extend(b"\0\0\0\0"), "header")                   # trailing
        bad(lambda b: b.__setitem__(data_start + 100, b[data_start + 100] ^ 1), "CRC")
        bad(lambda b: refresh_crcs(struct.pack_into("<H", b, 8, 2) or b), "reserved")
        bad(lambda b: refresh_crcs(struct.pack_into("<H", b, 16, 0) or b), "header")
        # Semantic checks behind valid CRCs:
        # quiz option marked as a like (no slot) -> rejected
        bad(lambda b: refresh_crcs(b.__setitem__(QUESTION_AT + 192 + 32 + 2, 1) or b),
            "slot")
        # like question given a right answer
        bad(lambda b: refresh_crcs(b.__setitem__(QUESTION_AT + 4, 1) or b), "no answer")
        # option moved onto its neighbour
        bad(lambda b: refresh_crcs(struct.pack_into("<H", b, QUESTION_AT + 32 + 48 + 4, 112)
                                   or b), "overlap")
        # icon x not byte aligned
        bad(lambda b: refresh_crcs(struct.pack_into("<H", b, QUESTION_AT + 32 + 4, 100)
                                   or b), "8 px")
        # like slot / option id outside the PREFERENCE_TOPICS whitelist
        bad(lambda b: refresh_crcs(struct.pack_into("<H", b, QUESTION_AT + 6, 5) or b),
            "registered")
        bad(lambda b: refresh_crcs(struct.pack_into("<H", b, QUESTION_AT + 32, 11) or b),
            "registered")
        # invalid UTF-8 label behind a correct segment CRC
        def bad_label(b):
            offset, size = struct.unpack_from("<II", b, QUESTION_AT + 32 + 36)
            b[offset] = 0xFF
            struct.pack_into("<I", b, QUESTION_AT + 32 + 44,
                             zlib.crc32(b[offset:offset + size]) & 0xFFFFFFFF)
            refresh_crcs(b)
        bad(bad_label, "utf-8")
        # third question claimed on page 1
        bad(lambda b: refresh_crcs(struct.pack_into("<H", b, 96 + 6, 3) or b), "page record")
        self.assertEqual(kpb2.parse(bytes(good))["format"], "KPB2")

    def test_source_rejections(self):
        def edit(change):
            doc = self._doc()
            change(doc)
            return doc
        q = lambda d, i: d["pages"][0]["questions"][i]  # noqa: E731
        self._rejects(edit(lambda d: q(d, 0).update(approved=False)), "review")
        self._rejects(edit(lambda d: d.update(package_type="soundpack")), "reserved")
        self._rejects(edit(lambda d: d.pop("reviewer")), "fields")
        self._rejects(edit(lambda d: q(d, 0).update(like_topic="favorite_planet")),
                      "unknown like_topic")
        self._rejects(edit(lambda d: q(d, 0)["options"][0].update(key="unicorn")),
                      "not a registered")
        self._rejects(edit(lambda d: q(d, 0).update(answer="rabbit")), "no right answer")
        self._rejects(edit(lambda d: q(d, 1)["options"][0].update(like=True)), "like_topic")
        self._rejects(edit(lambda d: [o.update(like=False) for o in q(d, 0)["options"]]),
                      "at least one like")
        self._rejects(edit(lambda d: q(d, 1)["options"][1].update(x=168)), "overlap")
        self._rejects(edit(lambda d: q(d, 1)["options"][1].update(x=484)), "multiple of 8")
        self._rejects(edit(lambda d: q(d, 1)["options"][1].update(y=380)), "inside the page")
        self._rejects(edit(lambda d: q(d, 1).update(answer="triangle")), "answer")
        self._rejects(edit(lambda d: q(d, 1).update(options=q(d, 1)["options"][:1])),
                      "2 or 3")
        self._rejects(edit(lambda d: d["pages"][0]["questions"].append(q(d, 1))),
                      "at most 2")
        self._rejects(edit(lambda d: d["pages"][1].update(image_mode="colour")), "image_mode")
        self._rejects(edit(lambda d: d.update(content_id=0)), "content_id")
        kpb2.write_pgm(self.base / "src" / "skip.pgm", 50, 48, b"\xff" * 50 * 48)
        self._rejects(self._doc(), "multiple of 8")
        _icon(self.base / "src" / "skip.pgm", 48, 3)
        with wave.open(str(self.base / "src" / "skip.wav"), "wb") as wf:
            wf.setnchannels(1)
            wf.setsampwidth(2)
            wf.setframerate(16000)
            wf.writeframes(b"\0\0" * 100)
        self._rejects(self._doc(), "24 kHz")
        _wav(self.base / "src" / "skip.wav", 24000 * 6, 3)
        self._rejects(self._doc(), "5 seconds")
        with self.assertRaisesRegex(ValueError, "--reviewed"):
            make_pack(self.source, self.output, reviewed=False)

    def test_like_whitelist_matches_cpp_reader(self):
        table = ", ".join(["0"] + [str(kpb2.LIKE_OPTION_MAX[s])
                                   for s in range(1, len(kpb2.LIKE_OPTION_MAX) + 1)])
        source = (ROOT / "firmware/components/kid_app/src/content_pack_reader.cc").read_text(
            encoding="utf-8")
        self.assertIn("kLikeOptionMax[] = {%s};" % table, source)
        for slot, _, options in kpb2.PREFERENCE_TOPICS.values():
            ids = sorted(i for i, _ in options.values())
            self.assertEqual(ids, list(range(1, len(ids) + 1)), "ids are append-only 1..n")

    def test_kpb1_still_compiles_and_validates(self):
        out = self.base / "k1" / "golden_kpb1.kpb"
        meta = make_pack(write_sample_kpb1(self.base / "k1"), out, reviewed=True)
        self.assertEqual(meta["format"], "KPB1")
        info = kpb2.parse(out.read_bytes())
        self.assertEqual((info["format"], len(info["pages"])), ("KPB1", 1))
        blob = bytearray(out.read_bytes())
        blob[-1] ^= 1
        with self.assertRaisesRegex(ValueError, "KPB1"):
            kpb2.parse(bytes(blob))

    def test_preview_and_validate_cli_without_pillow(self):
        make_pack(self.source, self.output, reviewed=True)
        written = kpb2.preview(self.output, self.base / "preview")
        self.assertEqual(sorted(p.name for p in written),
                         ["page01_grey.png", "page01_mono.png", "page01_q1.png",
                          "page01_q2.png", "page02_mono.png"])
        self.assertTrue(all(p.read_bytes()[:8] == b"\x89PNG\r\n\x1a\n" for p in written))
        ok = subprocess.run([sys.executable, "-m", "tools.story_pack", "validate",
                             "--package", str(self.output)], cwd=ROOT,
                            capture_output=True, text=True, timeout=60)
        self.assertEqual(ok.returncode, 0, ok.stderr)
        self.assertIn("PACKAGE_VALID KPB2 pages=2 questions=2", ok.stdout)
        broken = self.base / "broken.kpb"
        broken.write_bytes(self.output.read_bytes()[:-1])
        bad = subprocess.run([sys.executable, "-m", "tools.story_pack", "validate",
                              "--package", str(broken)], cwd=ROOT,
                             capture_output=True, text=True, timeout=60)
        self.assertNotEqual(bad.returncode, 0)
        self.assertIn("PACKAGE_INVALID", bad.stderr)

    @unittest.skipUnless(shutil.which("g++"), "g++ runs this in CI")
    def test_compiled_cpp_reader_on_tool_output(self):
        out = self.base / "cards"
        make_pack(self.source, out / "golden_book.kpb", reviewed=True)
        make_pack(write_sample_kpb1(self.base / "k1"), out / "golden_kpb1.kpb", reviewed=True)
        binary = self.base / "content_reader_check"
        src = ROOT / "firmware/components/kid_app/src"
        subprocess.run(["g++", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-I" + str(ROOT / "firmware/components/kid_app/include"),
                        str(src / "content_pack_reader.cc"), str(src / "story_pack_reader.cc"),
                        str(src / "story_pack_file_port.cc"), str(src / "scene_model.cc"),
                        str(src / "memory_store.cc"),
                        str(ROOT / "tests/cpp/test_content_pack_reader.cc"),
                        "-o", str(binary)], check=True, timeout=120)
        checked = subprocess.run([str(binary), str(out)], capture_output=True, text=True,
                                 timeout=30)
        self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
        self.assertIn("PASS: KPB2", checked.stdout)


def write_golden() -> None:
    with tempfile.TemporaryDirectory() as folder:
        base = Path(folder)
        FIXTURES.mkdir(parents=True, exist_ok=True)
        make_pack(write_sample_book(base / "src"), FIXTURES / "golden_book.kpb", reviewed=True)
        make_pack(write_sample_kpb1(base / "k1"), FIXTURES / "golden_kpb1.kpb", reviewed=True)
    (FIXTURES / "golden_kpb1.manifest.json").unlink()
    print("GOLDEN_WRITTEN", FIXTURES)


if __name__ == "__main__":
    if sys.argv[1:] == ["--write-golden"]:
        write_golden()
    else:
        unittest.main()
