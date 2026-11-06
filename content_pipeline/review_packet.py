"""Human-review packet for existing KPB2 story staging directories.

Shows exact mono/4-grey EPD pictures and lets a reviewer play actual staged
WAVs in their own browser. The HTML DOES NOT approve any content, save
personal information or call any external URL. All inputs are locally hash
bound to stage/candidate.json before building the packet.
"""
from __future__ import annotations

from html import escape
import json
from math import ceil
from pathlib import Path

from content_pipeline.packaging import WIDTH, HEIGHT, _core_manifest, _hash
from content_pipeline.schema import ContentError, canonical_json, write_json


def _verified_stage(stage: Path) -> tuple[dict, dict, dict]:
    candidate = json.loads((stage / "candidate.json").read_text(encoding="utf-8"))
    current = _core_manifest(stage)
    current["checked_hash"] = _hash(canonical_json(current))
    if candidate != current:
        raise ContentError("review packet: staged story changed since candidate hash")
    book = json.loads((stage / "book.json").read_text(encoding="utf-8"))
    details = json.loads((stage / "attribution.json").read_text(encoding="utf-8"))
    if book["approved"] is not False or any(
            p.get("approved") is not False for p in book["pages"]):
        raise ContentError("review packet: expected explicitly unapproved book")
    if len(book["pages"]) > 32:
        raise ContentError("review packet: invalid page count")
    return candidate, book, details


def _sheet(stage: Path, count: int, mode: str, dest: Path) -> None:
    from PIL import Image, ImageDraw
    cols, cell_width, image_width, image_height, gap, heading = 4, 320, 320, 192, 14, 27
    rows = ceil(count / cols)
    width = cols * cell_width + (cols + 1) * gap
    height = rows * (heading + image_height) + (rows + 1) * gap
    canvas = Image.new("RGB", (width, height), "white")
    painter = ImageDraw.Draw(canvas)
    for i in range(count):
        x = gap + (i % cols) * (cell_width + gap)
        y = gap + (i // cols) * (image_height + heading + gap)
        painter.text((x + 4, y + 4), f"Page {i+1:02d} / {mode}", fill="black")
        source = stage / "preview" / f"page_{i+1:02d}_{mode}.png"
        with Image.open(source) as img:
            if img.size != (WIDTH, HEIGHT) or img.mode != "L":
                raise ContentError("review packet: invalid EPD preview")
            canvas.paste(img.convert("RGB").resize(
                (image_width, image_height), resample=Image.Resampling.LANCZOS),
                (x, y + heading))
    canvas.save(dest, format="PNG")


def make_review_packet(stage_dir: Path) -> dict:
    """Return a read-only review interface; cannot produce approved artifacts."""
    stage = Path(stage_dir)
    candidate, book, details = _verified_stage(stage)
    target = stage / "review"
    target.mkdir(parents=True, exist_ok=True)
    _sheet(stage, len(book["pages"]), "mono", target / "contact-mono.png")
    _sheet(stage, len(book["pages"]), "grey", target / "contact-grey.png")

    title = escape(book["title"])
    creators = details["source"]["creators"]
    credits = (
        "Author(s): " + escape(", ".join(creators["text"])) + " | "
        "Illustrator(s): " + escape(", ".join(creators["illustration"])) + " | "
        "Translation(s): " + escape(", ".join(creators["translation"]))
    )
    license_ = escape(details["source"]["license_id"])
    test_only = details.get("audio_test_only") is True
    pages = []
    for p in book["pages"]:
        n = p["number"]
        if type(n) is not int or not 1 <= n <= len(book["pages"]):
            raise ContentError("review packet: invalid page number")
        text = escape(p["text"]).replace("\n", "<p></p>")
        pages.append(
            f'<article><h2>第 {n} 页</h2>'
            f'<div class="pics"><figure><img src="../preview/page_{n:02d}_mono.png" alt="Page {n} mono"><figcaption>实际 1-bit 回退</figcaption></figure>'
            f'<figure><img src="../preview/page_{n:02d}_grey.png" alt="Page {n} grey"><figcaption>四级灰度</figcaption></figure></div>'
            f'<p class="script">{text}</p>'
            f'<audio controls preload="none" src="../page_{n:02d}.wav">请在本地播放 WAV 文件。</audio>'
            f'</article>'
        )
    notice = ("测试音调，没有中文朗读！" if test_only else
              "真实朗读仍未审核；请逐句试听发音、语速与内容。")
    page = (
        '<!doctype html><html lang="zh-Hans"><head><meta charset="utf-8">'
        '<meta name="viewport" content="width=device-width,initial-scale=1">'
        '<title>NOT APPROVED — ' + title + '</title>'
        '<style>body{font:16px/1.6 system-ui,sans-serif;max-width:1100px;margin:30px auto;padding:0 16px;color:#222}'
        '.notice{padding:16px;border:2px solid #555;background:#f4f4f4}article{border-top:1px solid #bbb;padding:25px 0}'
        '.pics{display:flex;gap:14px;flex-wrap:wrap}figure{margin:0;width:min(48%,500px)}figure img{width:100%;border:1px solid #ddd}'
        'figcaption{font-size:14px;color:#555}audio{width:100%;max-width:540px}.script{font-size:19px}</style></head><body>'
        '<h1>' + title + '</h1><div class="notice"><strong>NOT APPROVED / 未审核</strong>'
        '<p>本页面仅供家长审看。不能自动批准、发布或改写 .kpb。' +
        escape(notice) + '</p><p>审核素材 SHA-256: <code>' +
        candidate["checked_hash"] + '</code></p></div>'
        '<p>' + credits + '</p><p>License: ' + license_ + '</p>'
        '<p>修改声明：AI 辅助中文草稿，电子纸画面转换和实验性音频；需要人工检查。</p>'
        '<h2>快速浏览</h2><p><img style="width:100%;height:auto" src="contact-mono.png" alt="1-bit contact"></p>'
        '<p><img style="width:100%;height:auto" src="contact-grey.png" alt="4-gray contact"></p>'
        + "".join(pages) +
        '</body></html>'
    )
    (target / "index.html").write_text(page, encoding="utf-8")
    result = {
        "status": "test_only" if test_only else "unreviewed",
        "pages": len(book["pages"]),
        "checked_hash": candidate["checked_hash"],
        "files": [
            "review/index.html", "review/contact-mono.png",
            "review/contact-grey.png"
        ],
        "notice": "Review presentation only; never self-approves child content.",
    }
    write_json(target / "review-summary.json", result)
    return result
