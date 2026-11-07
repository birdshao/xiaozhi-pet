"""Create the KPB2 v3 branching test book (V8 branch books, tests and board trial).

NON-CHILD CONTENT: synthetic mono pages, shapes and sine tones. Test use only.

  python -m tools.make_branch_test_book --output build/branch-test

Story graph (series 7, book 1 of 2, next book content id 902):

  1 --> 2 (branch: left / right) --left--> 3 (like question) --> 5 (ending A)
                                  \\-right-> 4 --------------> 6 (ending B)

Page 5 links to END explicitly (next = "end"); page 6 is the last page.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import tempfile

from tools import kpb2
from tools.make_reader_test_book import NOT_CHILD, _icon, _tone
from tools.story_pack import make_pack

BOOK_ID = "branch_test"
CONTENT_ID = 901
SERIES = {"id": 7, "index": 1, "count": 2, "next_content_id": 902}


def _page(path: Path, bars: int) -> None:
    lum = bytearray(b"\xff" * 800 * 480)
    for b in range(bars):
        for y in range(100, 380):
            x0 = 120 + b * 140
            lum[y * 800 + x0:y * 800 + x0 + 90] = b"\0" * 90
    kpb2.write_pgm(path, 800, 480, bytes(lum))


def source_document() -> dict:
    def option(key, label, x, go=None, like=False):
        item = {"key": key, "label": label, "icon": f"{key}.pgm", "audio": f"{key}.wav",
                "x": x, "y": 300}
        if go is not None:
            item["goto"] = go
        if like:
            item["like"] = True
        return item

    def page(n, text, extra=None):
        item = {"number": n, "text": text + NOT_CHILD, "approved": True,
                "image": f"page{n}.pgm", "image_mode": "mono", "audio": f"page{n}.wav"}
        item.update(extra or {})
        return item

    return {
        "schema_version": 2, "package_type": "book", "book_id": BOOK_ID,
        "content_id": CONTENT_ID, "revision": 1, "title": "分支测试书" + NOT_CHILD,
        "reviewer": "V8 测试", "approved": True, "series": SERIES,
        "pages": [
            page(1, "第一页。"),
            page(2, "走左边还是右边？", {"questions": [{
                "prompt": "走哪条路？", "prompt_audio": "q_way.wav", "approved": True,
                "options": [option("left", "左边", 160, go=3),
                            option("right", "右边", 520, go=4)]}]}),
            page(3, "左边的路。", {"next": 5, "questions": [{
                "prompt": "你最喜欢哪个颜色？", "prompt_audio": "q_color.wav",
                "approved": True, "like_topic": "favorite_color",
                "options": [option("red", "红色", 112, like=True),
                            option("blue", "蓝色", 352, like=True)]}]}),
            page(4, "右边的路。", {"next": 6}),
            page(5, "结局甲。", {"next": "end"}),
            page(6, "结局乙。"),
        ],
    }


def write_source(root: Path) -> Path:
    root.mkdir(parents=True, exist_ok=True)
    doc = source_document()
    for n in range(1, 7):
        _page(root / f"page{n}.pgm", n)
        _tone(root / f"page{n}.wav", 1.0, 400 + 40 * n)
    _tone(root / "q_way.wav", 0.8, 330)
    _tone(root / "q_color.wav", 0.8, 349)
    for i, key in enumerate(("left", "right", "red", "blue")):
        kind = {"left": "circle", "right": "square"}.get(key, key)
        kpb2.write_pgm(root / f"{key}.pgm", 96, 96, _icon(kind))
        _tone(root / f"{key}.wav", 0.4, 587 + 60 * i)
    source = root / "book.json"
    source.write_text(json.dumps(doc, ensure_ascii=False, indent=1), encoding="utf-8")
    return source


def build(output: Path) -> dict:
    output = output.resolve()
    target = output / "books" / f"{BOOK_ID}.kpb"
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="branch_test_") as workspace:
        result = make_pack(write_source(Path(workspace)), target, reviewed=True)
    manifest = target.with_name(f"{BOOK_ID}.manifest.json")
    if manifest.exists():
        manifest.replace(output / manifest.name)
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    result = build(args.output)
    print("BRANCH_TEST_ONLY", result["sha256"], result["package_bytes"])


if __name__ == "__main__":
    main()
