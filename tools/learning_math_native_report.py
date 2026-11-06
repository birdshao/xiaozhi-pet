"""Validate actual C++ SceneComposer 800x480 1-bit GameArt frames.

The HTML is a *read-only test-gallery* for adults, not a child game UI.
It embeds native firmware pixel output; correct answers and prompts come from
the original validated lesson. No real audio, flashing, or approvals.
"""
from __future__ import annotations

import argparse
import hashlib
from html import escape
import json
from pathlib import Path

from content_pipeline.game_lessons import validate_lesson
from content_pipeline.schema import ContentError, write_json

SAMPLE_SCENES = (
    "math_shape_1bit",
    "math_count_1bit",
    "math_size_1bit",
    "math_selected_1bit",
    "math_right_1bit",
)
ROUND_SCENES = tuple(f"math_round_{i:02d}_1bit" for i in range(1, 16))


def make_preview_report(source: Path, output: Path, lesson_file: Path) -> dict:
    try:
        from PIL import Image
    except ImportError as exc:
        raise ContentError("device previews: Pillow required") from exc
    lesson = validate_lesson(json.loads(Path(lesson_file).read_text(encoding="utf-8")))
    if len(lesson["rounds"]) != 15:
        raise ContentError("device previews: expected exact 15-round lesson")
    source, output = Path(source), Path(output)
    output.mkdir(parents=True, exist_ok=True)
    records = []
    digests = {}
    for name in SAMPLE_SCENES + ROUND_SCENES:
        pbm = source / f"{name}.pbm"
        if not pbm.is_file() or pbm.stat().st_size > 100_000:
            raise ContentError(f"device previews: missing or oversize {name}")
        binary = pbm.read_bytes()
        if not binary.startswith(b"P4\n800 480\n"):
            raise ContentError(f"device previews: wrong format or dimensions for {name}")
        try:
            with Image.open(pbm) as raw:
                if raw.format != "PPM" or raw.size != (800, 480):
                    raise ContentError(f"device previews: invalid 800x480 P4 frame: {name}")
                rendered = raw.convert("L")
                histogram = rendered.histogram()
                black = sum(histogram[:128])
                if black < 300 or black > 300000:
                    raise ContentError(f"device previews: suspicious solid/blank scene: {name}")
                png = output / f"{name}.png"
                rendered.save(png, format="PNG")
        except OSError as exc:
            raise ContentError(f"device previews: corrupt PBM {name}") from exc
        sha = hashlib.sha256(binary).hexdigest()
        digests[name] = sha
        records.append({
            "scene": name, "pbm_sha256": sha,
            "png_sha256": hashlib.sha256(png.read_bytes()).hexdigest(),
            "black_pixels": black,
            "size": [800, 480],
        })
    if len({digests[n] for n in SAMPLE_SCENES}) != len(SAMPLE_SCENES):
        raise ContentError("device previews: example states unexpectedly identical")

    # Actual hardware shuffles the options each session. These frames show
    # *source pack order*, while notes/answers are from the validated lesson.
    cards = []
    for n, (name, round_) in enumerate(zip(ROUND_SCENES, lesson["rounds"]), 1):
        answer = next(o["label"] for o in round_["options"]
                      if o["art"] == round_["answer"])
        options = " / ".join(o["label"] for o in round_["options"])
        cards.append(
            '<article><h2>第 ' + str(n) + ' 题 · ' +
            escape(round_["skill"]) + '</h2><img src="' +
            escape(name + ".png", quote=True) +
            '" alt="Firmware real 1bit GameArt question ' + str(n) + '">'
            '<p><strong>题干：</strong>' + escape(round_["prompt"]) + '</p>'
            '<p><strong>选项：</strong>' + escape(options) + '</p>'
            '<p><strong>正确答案（仅成人可见）：</strong>' +
            escape(answer) + '</p></article>')
    html = (
        '<!doctype html><html lang="zh-Hans"><head><meta charset="utf-8">'
        '<meta name="viewport" content="width=device-width,initial-scale=1">'
        '<title>TEST ONLY - Native KPG1 math GameArt</title>'
        '<style>body{font:16px/1.55 system-ui,sans-serif;max-width:1050px;'
        'margin:20px auto;padding:0 18px;color:#222}'
        'article{padding:20px 0;border-top:1px solid #aaa}article img{width:100%;'
        'max-width:800px;image-rendering:auto;border:1px solid #bbb}'
        '.warning{border:2px solid #555;padding:16px;background:#eee}'
        'strong{font-weight:700}</style></head><body>'
        '<h1>形状、数数、大小比较 · 固件真实黑白图像</h1>'
        '<div class="warning"><strong>TEST ONLY / NOT APPROVED</strong>'
        '<p>此图由原版 C++ SceneComposer 在 PC 上绘制，是 GameArt 实际像素，'
        '不是 ESP32 或电子纸面板拍摄的画面。</p>'
        '<p>展示的是 KPG1 原始选项顺序，真实游戏会随机打乱。'
        '对应 KPG1 音频全部为纯测试音调，不能给儿童当成中文课程。</p>'
        '<p>以下正确答案仅供成人审阅，不是儿童屏幕的一部分。</p></div>' +
        ''.join(cards) + '</body></html>'
    )
    (output / "index.html").write_text(html, encoding="utf-8")
    result = {
        "schema_version": "MATHDEVICEHOST2", "status": "test_only",
        "engine": "firmware SceneComposer::Render via native C++",
        "resolution": [800, 480],
        "rounds": len(lesson["rounds"]),
        "scenes": records,
        "gallery": "index.html",
        "notice": (
            "Actual firmware GameArt 1bit frame pixels, NOT physical EPD; "
            "all voices are synthetic tones and no content is approved."),
    }
    write_json(output / "device-scenes.json", result)
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--lesson", type=Path, required=True)
    args = parser.parse_args(argv)
    result = make_preview_report(args.source, args.output, args.lesson)
    print(json.dumps({
        "status": result["status"], "scenes": len(result["scenes"]),
        "rounds": result["rounds"], "engine": result["engine"],
    }, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
