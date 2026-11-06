"""Read-only, browser-based adult review for the 15-round preschool KPG1 lesson.

The visual symbols here are deliberately schematic and are NOT ESP32 screenshots.
All audio references point to local staged WAVs; no network code, scoring,
submission, self-approval or public upload is contained in this file.
"""
from __future__ import annotations

from html import escape
import json
from pathlib import Path

from content_pipeline.game_lessons import validate_lesson, audio_key
from content_pipeline.audio_qc import analyze_wav
from content_pipeline.game_packaging import _manifest, MAX_GAME_WAV
from content_pipeline.schema import ContentError, canonical_json, write_json
from content_pipeline.packaging import _hash, _asset, _sha_file

GAME_ICONS = {
    "match_shape": "认形状", "count": "数数", "big_small": "比较大小",
}


def _schematic(art: str) -> str:
    """Small original SVG diagrams for review only; not device GameArt pixels."""
    first = '<svg viewBox="0 0 120 80" width="130" height="87" role="img" aria-label="' + escape(art, quote=True) + '" xmlns="http://www.w3.org/2000/svg">'
    last = '</svg>'
    stroke = 'stroke="#252525" stroke-width="3" fill="none"'
    if art == "circle":
        shape = f'<circle cx="60" cy="40" r="28" {stroke}/>'
    elif art == "square":
        shape = f'<rect x="32" y="12" width="56" height="56" {stroke}/>'
    elif art == "triangle":
        shape = f'<polygon points="60,8 96,68 24,68" {stroke}/>'
    elif art == "star":
        shape = f'<polygon points="60,7 73,30 101,33 80,51 87,75 60,61 33,75 40,51 19,33 47,30" {stroke}/>'
    elif art == "heart":
        shape = f'<path d="M60 68 C18 43 20 15 41 15 C52 15 58 22 60 26 C70 8 96 12 99 31 C101 49 83 59 60 68Z" {stroke}/>'
    elif art.startswith("dots") and art[4:].isdigit():
        count = int(art[4:])
        if not 1 <= count <= 5:
            raise ContentError("review: unsupported dot count")
        positions = [(22+18*i, 40) for i in range(count)]
        shape = ''.join(f'<circle cx="{x}" cy="{y}" r="7" fill="#252525"/>' for x,y in positions)
    elif art.startswith("blocks") and art[6:].isdigit():
        count = int(art[6:])
        if not 1 <= count <= 5:
            raise ContentError("review: unsupported block count")
        shape = ''.join(f'<rect x="{12+i*21}" y="28" width="18" height="22" {stroke}/>' for i in range(count))
    elif art == "ball_big":
        shape = f'<circle cx="60" cy="40" r="33" {stroke}/>'
    elif art == "ball_small":
        shape = f'<circle cx="60" cy="40" r="16" {stroke}/>'
    elif art == "listen":
        shape = '<text x="60" y="46" fill="#252525" font-size="22" text-anchor="middle">♫</text>'
    else:
        raise ContentError("review: unsupported GameArt for math lesson")
    return first + shape + last


def _validated(stage: Path) -> tuple[dict, dict, dict]:
    candidate = json.loads((stage / "candidate.json").read_text(encoding="utf-8"))
    current = _manifest(stage)
    current["checked_hash"] = _hash(canonical_json(current))
    if candidate != current:
        raise ContentError("game review: staged WAVs or JSON changed after staging")
    lesson = validate_lesson(json.loads((stage / "lesson.json").read_text(encoding="utf-8")))
    game = json.loads((stage / "game.json").read_text(encoding="utf-8"))
    if game.get("approved") is not False or any(
            r.get("approved") is not False for r in game["rounds"]):
        raise ContentError("game review: expected unapproved draft game")
    if len(game["rounds"]) != len(lesson["rounds"]):
        raise ContentError("game review: original and staged rounds differ")
    return candidate, lesson, game


def make_game_review_packet(stage_dir: Path) -> dict:
    """Prepare local HTML + duration/sha checks; never change approval state."""
    stage = Path(stage_dir)
    candidate, lesson, game = _validated(stage)
    output = stage / "review"
    output.mkdir(parents=True, exist_ok=True)
    attributes = json.loads((stage / "attribution.json").read_text(encoding="utf-8"))
    is_test = attributes.get("audio_test_only") is True
    rows = []
    pages = []
    unique_clips = set()

    for i, (source, item) in enumerate(zip(lesson["rounds"], game["rounds"]), 1):
        if source["kind"] != item["kind"] or source["answer"] != item["answer"] \
                or source["prompt"] != item["prompt"] or \
                source["question_art"] != item["question_art"] or \
                [(o["art"], o["label"]) for o in source["options"]] != \
                [(o["art"], o["label"]) for o in item["options"]]:
            raise ContentError(f"game review: round {i} does not match the original draft")
        clips = [(item["prompt_audio"], source["prompt"])]
        for opt in item["options"]:
            clips.append((opt["audio"], opt["label"]))
        metrics = []
        for rel, spoken in clips:
            expected = f"clips/{audio_key(spoken)}.wav"
            if rel != expected:
                raise ContentError(f"game review: round {i} has incorrect speech mapping")
            wav = _asset(stage, rel, max_size=MAX_GAME_WAV)
            quality = analyze_wav(wav, max_seconds=10)
            metrics.append({"path": rel, "spoken_text": spoken,
                            "duration_ms": quality["duration_ms"],
                            "sha256": _sha_file(wav, MAX_GAME_WAV),
                            "quality": quality})
            unique_clips.add(rel)
        rows.append({
            "round": i, "kind": source["kind"], "skill": source["skill"],
            "prompt": source["prompt"],
            "answer_art": source["answer"],
            "answer_label": next(o["label"] for o in source["options"]
                                  if o["art"] == source["answer"]),
            "audio": metrics,
        })
        options = []
        for n, opt in enumerate(item["options"], 1):
            match = opt["art"] == item["answer"]
            options.append(
                '<div class="option"><div>' + _schematic(opt["art"]) + '</div>'
                '<div>' + escape(opt["label"]) + ' (' + escape(opt["art"]) + ')</div>'
                '<div><audio controls preload="none" src="../' +
                escape(opt["audio"], quote=True) + '"></audio></div>' +
                ('<strong>✓ 正确选项（仅审核者可见）</strong>' if match else '') +
                '</div>')
        pages.append(
            '<section><h2>第 ' + str(i) + ' 题 · ' +
            escape(GAME_ICONS[source["kind"]]) + '</h2>' +
            '<div class="hint">教学目标：' + escape(source["skill"]) + '</div>'
            '<div class="question">' + _schematic(source["question_art"]) +
            '<div><strong>' + escape(item["prompt"]) +
            '</strong><audio controls preload="none" src="../' +
            escape(item["prompt_audio"], quote=True) + '"></audio></div></div>' +
            '<div class="options">' + ''.join(options) + '</div></section>')
    unique_quality = {
        clip["path"]: clip["quality"]
        for row in rows for clip in row["audio"]
    }
    warning_clips = {
        path: quality["warnings"]
        for path, quality in unique_quality.items()
        if quality["warnings"]
    }
    html = (
        '<!doctype html><html lang="zh-Hans"><head><meta charset="utf-8">'
        '<meta name="viewport" content="width=device-width,initial-scale=1">'
        '<title>NOT APPROVED - preschool math game</title>'
        '<style>body{font:16px/1.6 system-ui,sans-serif;max-width:1050px;margin:24px auto;padding:0 14px}'
        'section{border-top:2px solid #ccc;margin-top:24px;padding-top:16px}.hint{color:#555}'
        '.warning{padding:16px;background:#eee;border:2px solid #444}'
        '.question{display:flex;gap:20px;align-items:center}.question audio{display:block;width:280px;max-width:100%}'
        '.options{display:flex;gap:12px;flex-wrap:wrap;margin-top:14px}'
        '.option{border:1px solid #aaa;padding:10px;min-width:170px;flex:1}'
        '.option audio{width:100%;max-width:240px}strong{display:block}</style></head><body>'
        '<h1>数学启蒙 · 15 道题 · 待审核</h1><div class="warning">'
        '<strong>NOT APPROVED / 未审核</strong>'
        '<p>本页面是成人用审核界面：正确答案会直接显示，请勿当成孩子的游戏界面。</p>'
        '<p>插图仅是原创矢量示意，并非固件内置 GameArt 的实际像素，必须另做实机验证。</p>'
        '<p>' + ('这些音频是合成测试音调，不是真人/中文朗读。' if is_test else
                 '这些中文合成语音尚未经真人完整试听与审核。') + '</p>'
        '<p>声音技术警告片段：' + str(len(warning_clips)) +
        ' / ' + str(len(unique_quality)) +
        '（低音量、削波、长静音等指标只供参考，仍须逐句试听）。</p>'
        '<p>候选哈希：<code>' + candidate["checked_hash"] + '</code></p></div>'
        + ''.join(pages) + '</body></html>')
    (output / "index.html").write_text(html, encoding="utf-8")
    report = {
        "schema_version": "GAMEREVIEW1",
        "status": "test_only" if is_test else "unreviewed",
        "lesson_uid": lesson["lesson_uid"],
        "content_id": lesson["content_id"],
        "checked_hash": candidate["checked_hash"],
        "rounds": rows,
        "unique_audio_clips": len(unique_clips),
        "technical_warnings": warning_clips,
        "total_audio_ms": sum({
            clip["path"]: clip["duration_ms"]
            for row in rows for clip in row["audio"]
        }.values()),
        "warning": "Illustrations are schematic, not device screenshots. NOT APPROVED.",
    }
    write_json(output / "review-summary.json", report)
    return report
