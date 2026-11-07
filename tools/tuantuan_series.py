"""团团系列绘本（V8）：从故事脚本生成 KPB2 书源目录，再用现有流水线编译。

故事脚本在 curriculum/stories/tuantuan/*.story.json（原创、3–5 岁、状态草稿）。
本工具做三件事，全部离线可复现，**不替成人审核**：

1. 插画：用 Pillow 画 800x480 的简笔插图（草稿，需要插画师/家长润色）；
2. 朗读：`--tts edge` 走现有流水线 content_pipeline.speech 的 Edge TTS 草稿
   （需要 edge-tts 和 ffmpeg，晓晓 zh-CN-XiaoxiaoNeural，见 docs/LEARNING_REAL_ASSETS_TTS.md）；
   `--tts tone` 只生成轻柔的占位音（测试用，不是朗读）；
3. 写出 book.json 和 DRAFT_STATUS.json。**默认 approved 全为 false**：成人逐页、
   逐题、逐段录音审核后手工改成 true，再 `python -m tools.story_pack pack --reviewed`。

用法：
  python -m tools.tuantuan_series build --output research_content/stories --tts edge
  python -m tools.tuantuan_series build --output build/tuantuan --tts tone      # 测试
  python -m tools.tuantuan_series pack-draft --output build/tuantuan-books     # CI：占位音、
      # 仅用于验证流水线，产出的 .kpb 不是给孩子的成品

脚本格式见 docs/V8_BOOKS.md §5。
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

ROOT = Path(__file__).resolve().parents[1]
STORY_DIR = ROOT / "curriculum" / "stories" / "tuantuan"
VOICE = "zh-CN-XiaoxiaoNeural"
RATE = 24000
W, H = 800, 480

# ------------------------------------------------------------ the pictures


def _pil():
    from PIL import Image, ImageDraw

    return Image, ImageDraw


class Art:
    """Tiny flat illustration kit. Greys are chosen from 4 levels so the grey
    page and its 1-bit fallback both read well on the ePaper."""

    def __init__(self) -> None:
        image, draw = _pil()
        self.image = image.new("L", (W, H), 255)
        self.d = draw.Draw(self.image)

    # backgrounds ---------------------------------------------------
    def background(self, kind: str) -> None:
        d = self.d
        if kind == "day":
            d.rectangle((0, 0, W, 330), fill=235)
            d.rectangle((0, 330, W, H), fill=170)
        elif kind == "night":
            d.rectangle((0, 0, W, H), fill=85)
            d.rectangle((0, 380, W, H), fill=0)
        elif kind == "room":
            d.rectangle((0, 0, W, H), fill=210)
            d.rectangle((0, 360, W, H), fill=130)
        elif kind == "water":
            d.rectangle((0, 0, W, 250), fill=235)
            d.rectangle((0, 250, W, H), fill=170)
            for y in range(280, H, 40):
                d.arc((60, y, 200, y + 30), 200, 340, fill=255, width=4)
                d.arc((420, y + 12, 560, y + 42), 200, 340, fill=255, width=4)
        else:
            raise ValueError(f"unknown background {kind}")

    # props ---------------------------------------------------------
    def sun(self, x, y, s):
        for k in range(8):
            a = k * math.pi / 4
            self.d.line((x + math.cos(a) * s * 1.2, y + math.sin(a) * s * 1.2,
                         x + math.cos(a) * s * 1.7, y + math.sin(a) * s * 1.7), fill=0, width=6)
        self.d.ellipse((x - s, y - s, x + s, y + s), fill=255, outline=0, width=5)

    def moon(self, x, y, s):
        self.d.ellipse((x - s, y - s, x + s, y + s), fill=255, outline=0, width=4)
        self.d.ellipse((x - s * 0.2, y - s * 1.1, x + s * 1.5, y + s * 0.7), fill=85)

    def star(self, x, y, s):
        pts = []
        for k in range(10):
            r = s if k % 2 == 0 else s * 0.45
            a = -math.pi / 2 + k * math.pi / 5
            pts.append((x + math.cos(a) * r, y + math.sin(a) * r))
        self.d.polygon(pts, fill=255, outline=170)

    def cloud(self, x, y, s):
        for dx, r in ((-s, 0.6), (0, 0.85), (s, 0.6)):
            self.d.ellipse((x + dx - s * r, y - s * r, x + dx + s * r, y + s * r),
                           fill=255, outline=170, width=3)

    def tree(self, x, y, s):
        self.d.rectangle((x - s * 0.15, y, x + s * 0.15, y + s), fill=85)
        self.d.ellipse((x - s * 0.7, y - s * 0.9, x + s * 0.7, y + s * 0.3), fill=130,
                       outline=0, width=4)

    def flower(self, x, y, s):
        self.d.line((x, y, x, y + s * 1.6), fill=0, width=4)
        for k in range(6):
            a = k * math.pi / 3
            cx, cy = x + math.cos(a) * s * 0.6, y + math.sin(a) * s * 0.6
            self.d.ellipse((cx - s * 0.4, cy - s * 0.4, cx + s * 0.4, cy + s * 0.4),
                           fill=255, outline=0, width=3)
        self.d.ellipse((x - s * 0.3, y - s * 0.3, x + s * 0.3, y + s * 0.3), fill=85, outline=0)

    def butterfly(self, x, y, s):
        for sx in (-1, 1):
            self.d.ellipse((x + sx * s * 0.9 - s * 0.8, y - s * 0.9, x + sx * s * 0.9 + s * 0.8,
                            y + s * 0.5), fill=255, outline=0, width=3)
        self.d.line((x, y - s * 0.5, x, y + s * 0.8), fill=0, width=5)

    def ball(self, x, y, s):
        self.d.ellipse((x - s, y - s, x + s, y + s), fill=255, outline=0, width=5)
        self.d.arc((x - s, y - s, x + s, y + s), 40, 140, fill=0, width=5)
        self.d.line((x - s, y, x + s, y), fill=0, width=4)

    def path(self, x, y, s):  # a road that forks left and right
        self.d.polygon([(x - s * 0.25, H), (x + s * 0.25, H), (x + s * 0.2, y + s * 0.5),
                        (x + s * 1.9, y - s * 0.4), (x + s * 1.6, y - s * 0.55),
                        (x, y + s * 0.2), (x - s * 1.6, y - s * 0.55), (x - s * 1.9, y - s * 0.4),
                        (x - s * 0.2, y + s * 0.5)], fill=235, outline=0)

    def bunny(self, x, y, s):
        d = self.d
        d.ellipse((x - s * 0.9, y - s * 0.2, x + s * 0.9, y + s * 1.2), fill=255, outline=0, width=4)
        d.ellipse((x - s * 0.6, y - s * 1.0, x + s * 0.6, y + s * 0.2), fill=255, outline=0, width=4)
        for sx in (-1, 1):
            d.ellipse((x + sx * s * 0.35 - s * 0.18, y - s * 2.0, x + sx * s * 0.35 + s * 0.18,
                       y - s * 0.7), fill=255, outline=0, width=4)
            d.ellipse((x + sx * s * 0.22 - s * 0.07, y - s * 0.55, x + sx * s * 0.22 + s * 0.07,
                       y - s * 0.4), fill=0)
        d.ellipse((x - s * 0.06, y - s * 0.3, x + s * 0.06, y - s * 0.18), fill=85)

    def duck(self, x, y, s):
        d = self.d
        d.ellipse((x - s, y - s * 0.4, x + s, y + s * 0.8), fill=255, outline=0, width=4)
        d.ellipse((x + s * 0.4, y - s * 1.0, x + s * 1.3, y - s * 0.1), fill=255, outline=0, width=4)
        d.polygon([(x + s * 1.25, y - s * 0.65), (x + s * 1.8, y - s * 0.5),
                   (x + s * 1.25, y - s * 0.4)], fill=85, outline=0)
        d.ellipse((x + s * 0.85, y - s * 0.75, x + s * 0.95, y - s * 0.65), fill=0)

    def fish(self, x, y, s):
        d = self.d
        d.ellipse((x - s, y - s * 0.5, x + s, y + s * 0.5), fill=255, outline=0, width=4)
        d.polygon([(x - s, y), (x - s * 1.7, y - s * 0.5), (x - s * 1.7, y + s * 0.5)],
                  fill=255, outline=0)
        d.ellipse((x + s * 0.45, y - s * 0.2, x + s * 0.6, y - s * 0.05), fill=0)

    def doll(self, x, y, s):
        d = self.d
        d.ellipse((x - s * 0.5, y - s * 1.4, x + s * 0.5, y - s * 0.4), fill=255, outline=0, width=4)
        d.rounded_rectangle((x - s * 0.7, y - s * 0.4, x + s * 0.7, y + s * 0.9), s * 0.2,
                            fill=170, outline=0, width=4)
        d.ellipse((x - s * 0.2, y - s * 1.0, x - s * 0.1, y - s * 0.9), fill=0)
        d.ellipse((x + s * 0.1, y - s * 1.0, x + s * 0.2, y - s * 0.9), fill=0)

    def window(self, x, y, s):
        self.d.rectangle((x - s, y - s, x + s, y + s), fill=85, outline=0, width=8)
        self.d.line((x, y - s, x, y + s), fill=0, width=6)
        self.d.line((x - s, y, x + s, y), fill=0, width=6)

    def bed(self, x, y, s):
        d = self.d
        d.rounded_rectangle((x - s * 1.6, y, x + s * 1.6, y + s * 0.9), s * 0.2, fill=255,
                            outline=0, width=5)
        d.rounded_rectangle((x - s * 1.5, y + s * 0.3, x + s * 1.5, y + s * 1.1), s * 0.25,
                            fill=170, outline=0, width=5)
        d.rounded_rectangle((x - s * 1.45, y - s * 0.15, x - s * 0.5, y + s * 0.35), s * 0.2,
                            fill=255, outline=0, width=4)

    def basin(self, x, y, s):
        self.d.ellipse((x - s, y, x + s, y + s * 0.6), fill=235, outline=0, width=5)
        for k in range(3):
            self.d.ellipse((x - s * 0.5 + k * s * 0.4, y - s * 0.3 - k * s * 0.15,
                            x - s * 0.5 + k * s * 0.4 + s * 0.25, y - s * 0.05 - k * s * 0.15),
                           fill=255, outline=0, width=3)

    # the cat -------------------------------------------------------
    def tuantuan(self, x, y, s, mood="happy", turn=0):
        """TuanTuan: a round white cat. (x, y) = head centre, s = head radius."""
        d = self.d
        d.ellipse((x - s * 0.9, y + s * 0.6, x + s * 0.9, y + s * 2.0), fill=255, outline=0, width=5)
        for sx in (-1, 1):
            d.polygon([(x + sx * s * 0.95, y - s * 0.1), (x + sx * s * 0.75, y - s * 1.35),
                       (x + sx * s * 0.2, y - s * 0.85)], fill=255, outline=0)
            d.polygon([(x + sx * s * 0.8, y - s * 0.3), (x + sx * s * 0.72, y - s * 1.0),
                       (x + sx * s * 0.4, y - s * 0.75)], fill=170)
        d.ellipse((x - s, y - s, x + s, y + s), fill=255, outline=0, width=5)
        ex = turn * s * 0.12
        for sx in (-1, 1):
            cx, cy = x + sx * s * 0.38 + ex, y - s * 0.12
            if mood == "sleepy":
                d.arc((cx - s * 0.2, cy - s * 0.1, cx + s * 0.2, cy + s * 0.2), 0, 180, fill=0,
                      width=5)
            elif mood == "surprised":
                d.ellipse((cx - s * 0.17, cy - s * 0.2, cx + s * 0.17, cy + s * 0.2), fill=0)
            else:
                d.ellipse((cx - s * 0.12, cy - s * 0.15, cx + s * 0.12, cy + s * 0.15), fill=0)
        d.polygon([(x + ex - s * 0.1, y + s * 0.18), (x + ex + s * 0.1, y + s * 0.18),
                   (x + ex, y + s * 0.3)], fill=85)
        if mood == "surprised":
            d.ellipse((x + ex - s * 0.1, y + s * 0.38, x + ex + s * 0.1, y + s * 0.6), outline=0,
                      width=4)
        elif mood == "yawn":
            d.ellipse((x + ex - s * 0.18, y + s * 0.35, x + ex + s * 0.18, y + s * 0.75),
                      fill=85, outline=0, width=4)
        else:
            d.arc((x + ex - s * 0.25, y + s * 0.2, x + ex, y + s * 0.5), 20, 160, fill=0, width=4)
            d.arc((x + ex, y + s * 0.2, x + ex + s * 0.25, y + s * 0.5), 20, 160, fill=0, width=4)
        for sx in (-1, 1):
            for k in (-1, 0, 1):
                d.line((x + sx * s * 0.55, y + s * 0.28 + k * s * 0.08, x + sx * s * 1.05,
                        y + s * 0.2 + k * s * 0.18), fill=0, width=3)

    # icons for the question cards ----------------------------------
    def save(self, path: Path) -> None:
        self.image.save(path, format="PNG")


PROP_NAMES = ("sun", "moon", "star", "cloud", "tree", "flower", "butterfly", "ball", "path",
              "bunny", "duck", "fish", "doll", "window", "bed", "basin")


def render_page(art_spec: dict, target: Path) -> None:
    art = Art()
    art.background(art_spec["bg"])
    for name, x, y, s in art_spec.get("props", []):
        if name not in PROP_NAMES:
            raise ValueError(f"unknown prop {name}")
        getattr(art, name)(x, y, s)
    cat = art_spec.get("cat")
    if cat:
        art.tuantuan(*cat)
    art.save(target)


def render_icon(kind: str, target: Path, size: int = 96) -> None:
    image_mod, draw_mod = _pil()
    image = image_mod.new("L", (size, size), 255)
    d = draw_mod.Draw(image)
    d.rectangle((0, 0, size - 1, size - 1), outline=0, width=4)
    c = size // 2
    if kind == "left":
        d.polygon([(14, c), (c, 16), (c, size - 16)], fill=0)
        d.rectangle((c, c - 10, size - 14, c + 10), fill=0)
    elif kind == "right":
        d.polygon([(size - 14, c), (c, 16), (c, size - 16)], fill=0)
        d.rectangle((14, c - 10, c, c + 10), fill=0)
    elif kind in ("ball", "doll"):
        art = Art()
        art.image = image
        art.d = d
        if kind == "ball":
            art.ball(c, c, 28)
        else:
            art.doll(c, c + 8, 28)
    else:
        raise ValueError(f"unknown icon {kind}")
    image.save(target, format="PNG")


# --------------------------------------------------------------- the voice


def tone(text: str, target: Path) -> None:
    """Soft placeholder tone, length follows the text. NOT narration."""
    seconds = max(0.6, min(14.0, 0.28 * len(text)))
    samples = int(RATE * seconds)
    pcm = bytearray()
    for n in range(samples):
        ramp = min(1.0, n / 720, (samples - 1 - n) / 720)
        value = round(0.10 * max(0.0, ramp) * math.sin(2 * math.pi * 392 * n / RATE) * 32767)
        pcm.extend(struct.pack("<h", value))
    with wave.open(str(target), "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(RATE)
        wf.writeframes(bytes(pcm))


def speak(text: str, target: Path, backend: str, max_seconds: int) -> None:
    if backend == "tone":
        tone(text, target)
        return
    from content_pipeline import speech

    speech._generate(text, target, VOICE, synthesize=None, replace=True, max_seconds=max_seconds)


# ------------------------------------------------------------- the sources


def load_stories(directory: Path = STORY_DIR) -> list[dict]:
    stories = [json.loads(p.read_text(encoding="utf-8")) for p in sorted(directory.glob("*.story.json"))]
    return sorted(stories, key=lambda s: s["series"]["index"])


def check_story(story: dict) -> None:
    """Script-level checks (the compiler and kpb2.parse repeat the hard ones)."""
    pages = story["pages"]
    if not 5 <= len(pages) <= 8:
        raise ValueError(f"{story['book_id']}: a story has 5..8 pages")
    for number, page in enumerate(pages, 1):
        if not page["text"].strip() or len(page["text"]) > 60:
            raise ValueError(f"{story['book_id']} page {number}: text must be 1..60 characters")
        if not 5 <= len(story["title"]) <= 20:
            raise ValueError("title length")
    series = story["series"]
    kpb2.check_series(story["content_id"], (series["id"], series["index"], series["count"],
                                            series["next_content_id"]))


def build_source(story: dict, out: Path, tts: str, approve: bool = False) -> Path:
    check_story(story)
    out.mkdir(parents=True, exist_ok=True)
    pages = []
    icons: dict[str, str] = {}
    for number, page in enumerate(story["pages"], 1):
        render_page(page["art"], out / f"page_{number:02d}.png")
        speak(page["text"], out / f"page_{number:02d}.wav", tts, 60)
        entry = {"number": number, "text": page["text"], "approved": approve,
                 "image": f"page_{number:02d}.png", "image_mode": "grey",
                 "audio": f"page_{number:02d}.wav"}
        if "next" in page:
            entry["next"] = page["next"]
        question = page.get("question")
        if question:
            speak(question["prompt"], out / f"q_{number:02d}.wav", tts, 15)
            options = []
            for k, option in enumerate(question["options"]):
                icon = option["icon"]
                if icon not in icons:
                    render_icon(icon, out / f"icon_{icon}.png")
                    icons[icon] = f"icon_{icon}.png"
                speak(option["label"], out / f"name_{option['key']}_{number:02d}.wav", tts, 5)
                item = {"key": option["key"], "label": option["label"], "icon": icons[icon],
                        "audio": f"name_{option['key']}_{number:02d}.wav",
                        "x": option["x"], "y": option["y"]}
                for extra in ("goto", "like"):
                    if extra in option:
                        item[extra] = option[extra]
                options.append(item)
            q = {"prompt": question["prompt"], "prompt_audio": f"q_{number:02d}.wav",
                 "approved": approve, "options": options}
            if "like_topic" in question:
                q["like_topic"] = question["like_topic"]
            entry["questions"] = [q]
        pages.append(entry)
    book = {"schema_version": 2, "package_type": "book", "book_id": story["book_id"],
            "content_id": story["content_id"], "revision": 1, "title": story["title"],
            "reviewer": "待成人审核" if not approve else "CI 草稿（未审核）",
            "approved": approve, "series": story["series"], "pages": pages}
    (out / "book.json").write_text(json.dumps(book, ensure_ascii=False, indent=1),
                                   encoding="utf-8")
    status = {
        "status": "draft", "needs_adult_review": True,
        "images": "procedural_draft_needs_illustrator_polish",
        "audio": "tone_placeholder" if tts == "tone" else f"edge_tts_draft:{VOICE}",
        "approved_flags_set": approve,
        "next": "docs/V8_STORY_REVIEW.md 的清单逐项过完，再把 book.json 里的 approved 改成 true",
    }
    (out / "DRAFT_STATUS.json").write_text(json.dumps(status, ensure_ascii=False, indent=1),
                                           encoding="utf-8")
    return out / "book.json"


def build_all(output: Path, tts: str, approve: bool = False) -> list[Path]:
    return [build_source(s, output / s["book_id"], tts, approve) for s in load_stories()]


def pack_drafts(output: Path) -> list[dict]:
    """CI: placeholder audio + approved flags, only to prove the pipeline and
    the story graph compile. The result is NOT a finished book."""
    from tools.story_pack import make_pack

    results = []
    with tempfile.TemporaryDirectory(prefix="tuantuan_") as work:
        for story in load_stories():
            source = build_source(story, Path(work) / story["book_id"], "tone", approve=True)
            target = output / f"{story['book_id']}.kpb"
            results.append(make_pack(source, target, reviewed=True))
    return results


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)
    build = sub.add_parser("build")
    build.add_argument("--output", type=Path, required=True)
    build.add_argument("--tts", choices=("edge", "tone"), default="edge")
    pack = sub.add_parser("pack-draft")
    pack.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.cmd == "build":
        for path in build_all(args.output, args.tts):
            print("DRAFT_SOURCE", path)
    else:
        for result in pack_drafts(args.output):
            print("DRAFT_PACK_NOT_REVIEWED", result["sha256"], result["package_bytes"])


if __name__ == "__main__":
    main()
