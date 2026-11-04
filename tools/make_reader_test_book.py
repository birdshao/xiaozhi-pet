"""Create the KPB2 test book for the V4-05 picture-book reader trial.

NON-CHILD CONTENT: synthetic grey bands, black shapes and gentle sine tones,
not a story. The fixed approval bits only satisfy the packer. Test use only;
do not put it on a card a child uses.

Run (no Pillow needed):
  python -m tools.make_reader_test_book --output build/reader-test
Copy build/reader-test/books/reader_test.kpb to FAT32 /books/ on the TF card
(next to forest_friends.kpb); tools/selftest/v4_book_reader.pettest selects it
with "book reader_test".

Pages: 1 grey (4 bands) with a like question (favorite_color: red / blue /
"both", not a like) and a quiz (circle / square, circle is right); 2 grey
(4 vertical bands), no question; 3 mono (two black bars).
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import struct
import tempfile
import wave

from tools import kpb2
from tools.story_pack import make_pack

BOOK_ID = "reader_test"
CONTENT_ID = 900
NOT_CHILD = "（测试样例，非儿童内容）"
RATE = 24000


def _tone(path: Path, seconds: float, pitch: float) -> None:
    # Gentle 14 %-peak sine with 30 ms fades, as the SD smoke book.
    samples = int(RATE * seconds)
    pcm = bytearray()
    for n in range(samples):
        ramp = min(1.0, n / 720, (samples - 1 - n) / 720)
        value = round(0.14 * max(0.0, ramp) * math.sin(2 * math.pi * pitch * n / RATE) * 32767)
        pcm.extend(struct.pack("<h", value))
    with wave.open(str(path), "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(RATE)
        wf.writeframes(bytes(pcm))


def _frame(lum: bytearray) -> None:
    # Black 8 px frame so a cut-off refresh is easy to see.
    for y in range(480):
        for x in range(800):
            if x < 8 or y < 8 or x >= 792 or y >= 472:
                lum[y * 800 + x] = 0


def _page1() -> bytes:
    lum = bytearray(bytes((0, 85, 170, 255)[y // 120] for y in range(480) for _ in range(800)))
    _frame(lum)
    return bytes(lum)


def _page2() -> bytes:
    lum = bytearray(bytes((255, 170, 85, 0)[x // 200] for _ in range(480) for x in range(800)))
    _frame(lum)
    return bytes(lum)


def _page3() -> bytes:
    lum = bytearray(b"\xff" * 800 * 480)
    for y in range(120, 360):
        lum[y * 800 + 200:y * 800 + 330] = b"\0" * 130
        lum[y * 800 + 470:y * 800 + 600] = b"\0" * 130
    _frame(lum)
    return bytes(lum)


def _icon(kind: str, size: int = 96) -> bytes:
    lum = bytearray(b"\xff" * size * size)
    c = (size - 1) / 2
    for y in range(size):
        for x in range(size):
            edge = x < 3 or y < 3 or x >= size - 3 or y >= size - 3
            d = math.hypot(x - c, y - c)
            if kind == "circle":
                black = edge or d < size * 0.36
            elif kind == "square":
                black = edge or (size * 0.18 <= x < size * 0.82 and size * 0.18 <= y < size * 0.82)
            elif kind == "red":  # solid disc
                black = edge or d < size * 0.40
            elif kind == "blue":  # ring
                black = edge or size * 0.26 < d < size * 0.40
            else:  # "both": two small discs
                black = edge or math.hypot(x - size * 0.3, y - c) < size * 0.16 or \
                    math.hypot(x - size * 0.7, y - c) < size * 0.16
            lum[y * size + x] = 0 if black else 255
    return bytes(lum)


def write_source(root: Path) -> Path:
    root.mkdir(parents=True, exist_ok=True)
    for name, pixels in (("page1.pgm", _page1()), ("page2.pgm", _page2()),
                         ("page3.pgm", _page3())):
        kpb2.write_pgm(root / name, 800, 480, pixels)
    _tone(root / "page1.wav", 2.0, 440)
    _tone(root / "page2.wav", 1.5, 523)
    _tone(root / "page3.wav", 1.0, 659)
    _tone(root / "q_color.wav", 1.0, 330)
    _tone(root / "q_shape.wav", 1.0, 349)
    for i, kind in enumerate(("red", "blue", "both", "circle", "square")):
        kpb2.write_pgm(root / f"{kind}.pgm", 96, 96, _icon(kind))
        _tone(root / f"{kind}.wav", 0.4, 587 + 60 * i)
    book = {
        "schema_version": 2, "package_type": "book", "book_id": BOOK_ID,
        "content_id": CONTENT_ID, "revision": 1, "title": "阅读器测试书" + NOT_CHILD,
        "reviewer": "V4-05 硬件自测", "approved": True,
        "pages": [
            {"number": 1, "text": "第一页：四条横向灰带。" + NOT_CHILD, "approved": True,
             "image": "page1.pgm", "image_mode": "grey", "audio": "page1.wav",
             "questions": [
                 {"prompt": "你最喜欢哪个颜色？", "prompt_audio": "q_color.wav",
                  "approved": True, "like_topic": "favorite_color",
                  "options": [
                      {"key": "red", "label": "红色", "icon": "red.pgm", "audio": "red.wav",
                       "x": 112, "y": 344, "like": True},
                      {"key": "blue", "label": "蓝色", "icon": "blue.pgm",
                       "audio": "blue.wav", "x": 352, "y": 344, "like": True},
                      {"key": "both", "label": "都喜欢", "icon": "both.pgm",
                       "audio": "both.wav", "x": 592, "y": 344},
                  ]},
                 {"prompt": "哪个是圆形？", "prompt_audio": "q_shape.wav", "approved": True,
                  "answer": "circle",
                  "options": [
                      {"key": "circle", "label": "圆形", "icon": "circle.pgm",
                       "audio": "circle.wav", "x": 200, "y": 160},
                      {"key": "square", "label": "方形", "icon": "square.pgm",
                       "audio": "square.wav", "x": 504, "y": 160},
                  ]},
             ]},
            {"number": 2, "text": "第二页：四条竖向灰带。" + NOT_CHILD, "approved": True,
             "image": "page2.pgm", "image_mode": "grey", "audio": "page2.wav"},
            {"number": 3, "text": "第三页：两根黑柱。" + NOT_CHILD, "approved": True,
             "image": "page3.pgm", "image_mode": "mono", "audio": "page3.wav"},
        ],
    }
    source = root / "book.json"
    source.write_text(json.dumps(book, ensure_ascii=False, indent=1), encoding="utf-8")
    return source


def build(output: Path) -> dict:
    output = output.resolve()
    target = output / "books" / f"{BOOK_ID}.kpb"
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="reader_test_") as workspace:
        result = make_pack(write_source(Path(workspace)), target, reviewed=True)
    # The sidecar names the synthetic reviewer only; keep the card folder clean.
    manifest = target.with_name(f"{BOOK_ID}.manifest.json")
    if manifest.exists():
        manifest.replace(output / manifest.name)
    readme = (
        "V4-05 PICTURE-BOOK READER TEST BOOK - NOT CHILD CONTENT\n"
        "Synthetic grey bands, shapes and sine tones only.\n"
        f"Copy books/{BOOK_ID}.kpb onto the FAT32 TF card at /books/{BOOK_ID}.kpb\n"
        "(keep /books/forest_friends.kpb; the device reads the test book only when a\n"
        f"self-test script says 'book {BOOK_ID}').\n"
        "Page 1: 4 grey bands, 440 Hz; questions: favourite colour (red disc / blue ring /\n"
        "two dots = both), then circle or square (circle is right).\n"
        "Page 2: 4 vertical grey bands, 523 Hz. Page 3: two black bars, 659 Hz (1-bit).\n"
        "Checksums detect damage only; they do not prove review or origin.\n"
        f"SHA256: {result['sha256']}\n"
    )
    (output / "README-READER-TEST-ONLY.txt").write_text(readme, encoding="utf-8")
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    result = build(args.output)
    print("READER_TEST_ONLY", result["sha256"], result["package_bytes"])


if __name__ == "__main__":
    main()
