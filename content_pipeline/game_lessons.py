"""Original preschool math exercises using the firmware's existing KPG1 art.

The mathematical teaching patterns overlap with general activities in GCompris
(enumeration, matching) but this module copies NO GCompris code or assets.
Everything emitted is DRAFT until an adult reviews the recordings and answers.
"""
from __future__ import annotations

import hashlib
import re
from typing import Any

from content_pipeline.schema import ContentError, canonical_json, write_json
from tools import game_pack

SCHEMA = "GAMELESSON1"
LESSON_UID = "preschool.shapes_and_numbers.v1"
LESSON_ID = 20001

# Three individually approvable SD lessons for boot-time course selection.
# Short pack basenames are intentionally FAT-compatible and never reused.
UNIT_COURSES = {
    "shapes": ("preschool.math.shapes.v1", "preschool_shapes", 20011,
               "认识形状", "match_shape"),
    "counts": ("preschool.math.counts.v1", "preschool_counts", 20012,
               "数一数", "count"),
    "sizes": ("preschool.math.sizes.v1", "preschool_sizes", 20013,
              "比较大小", "big_small"),
}
SHAPES = ("circle", "square", "triangle", "star", "heart")
SHAPE_NAMES = {
    "circle": "圆形", "square": "正方形", "triangle": "三角形",
    "star": "星形", "heart": "爱心形"
}
NUMBERS = {1: "一个", 2: "两个", 3: "三个", 4: "四个", 5: "五个"}
BALL_NAMES = {"ball_big": "大球", "ball_small": "小球"}
KIND_SKILL = {"match_shape": "math.shapes_basic",
              "count": "math.count_1_5",
              "big_small": "math.size_compare"}
MAX_LESSON_ROUNDS = game_pack.MAX_ROUNDS


def _option(art: str) -> dict[str, str]:
    if art in SHAPE_NAMES:
        name = SHAPE_NAMES[art]
    elif art.startswith("dots") and art[4:].isdigit() and int(art[4:]) in NUMBERS:
        name = NUMBERS[int(art[4:])] + "圆点"
    elif art in BALL_NAMES:
        name = BALL_NAMES[art]
    else:
        raise ContentError("unknown option picture: " + str(art))
    return {"art": art, "label": name}


def original_math_lesson() -> dict[str, Any]:
    """15 exercises: 5 shapes, 5 numbers, 5 size-judgment variants."""
    rounds: list[dict] = []
    # Keep the correct answer at different positions; the board may shuffle
    # again on each session. No numerical labels are required for a 3yo child.
    for i, shape in enumerate(SHAPES):
        arts = [SHAPES[(i + 1) % 5], SHAPES[(i + 2) % 5]]
        arts.insert(i % 3, shape)
        rounds.append({
            "kind": "match_shape", "skill": "math.shapes_basic",
            "target": shape, "question_art": shape,
            "prompt": "找出和上面一样的形状。", "answer": shape,
            "options": [_option(a) for a in arts],
        })
    for n in range(1, 6):
        # Distractors are neighboring values where possible; never include
        # 0 or 6, which are not in firmware's built-in artwork.
        distractors = [m for m in range(1, 6) if m != n]
        distractors.sort(key=lambda m: (abs(m - n), m))
        arts = [f"dots{distractors[0]}", f"dots{distractors[1]}"]
        arts.insert(n % 3, f"dots{n}")
        rounds.append({
            "kind": "count", "skill": "math.count_1_5", "target": str(n),
            "question_art": f"blocks{n}",
            "prompt": "数一数积木，选出一样多的圆点。",
            "answer": f"dots{n}", "options": [_option(a) for a in arts],
        })
    comparisons = [
        ("big", "哪个球更大？"),
        ("small", "哪个球更小？"),
        ("big", "找出大球。"),
        ("small", "找出小球。"),
        ("big", "听一听，哪一个是大球？"),
    ]
    for i, (target, prompt) in enumerate(comparisons):
        arts = ["ball_big", "ball_small"]
        if i % 2:
            arts.reverse()
        rounds.append({
            "kind": "big_small", "skill": "math.size_compare",
            "target": target, "question_art": "listen",
            "prompt": prompt, "answer": f"ball_{target}",
            "options": [_option(a) for a in arts],
        })
    lesson = {
        "schema_version": SCHEMA,
        "lesson_uid": LESSON_UID,
        "game_id": "preschool_math",
        "content_id": LESSON_ID,
        "revision": 1,
        "title": "形状与数量",
        "locale": "zh-Hans",
        "age_band": "3-5",
        "source": {
            "origin": "original-preschool-exercises",
            "reference_url": "https://github.com/KDE/gcompris",
            "copied_assets": False,
            "notes": "Original prompts and art combinations; no copied QML/audio/images."
        },
        "review": {"status": "draft"},
        "rounds": rounds,
    }
    return validate_lesson(lesson)


def original_math_unit_lessons() -> dict[str, dict[str, Any]]:
    """Split the original 15 questions into three *separately reviewed* units.

    This only prepares draft GAMELESSON1 input; all three packs must go
    through real audio, adult approval, and the existing KPG1 build gate.
    """
    import copy
    base = original_math_lesson()
    results = {}
    for basename, (uid, game_id, content_id, title, kind) in UNIT_COURSES.items():
        unit = copy.deepcopy(base)
        unit.update({
            "lesson_uid": uid, "game_id": game_id,
            "content_id": content_id, "title": title,
            "rounds": [r for r in unit["rounds"] if r["kind"] == kind],
        })
        results[basename] = validate_lesson(unit)
    return results


def _text(value: Any, where: str, limit: int) -> str:
    if not isinstance(value, str) or not value.strip() or len(value) > limit or any(
            ord(ch) < 32 for ch in value):
        raise ContentError(f"{where}: invalid text")
    return value


def validate_lesson(raw: Any) -> dict:
    expected = {"schema_version", "lesson_uid", "game_id", "content_id",
                "revision", "title", "locale", "age_band", "source",
                "review", "rounds"}
    if not isinstance(raw, dict) or set(raw) != expected:
        raise ContentError("lesson: missing or unexpected field")
    if raw["schema_version"] != SCHEMA or raw["age_band"] != "3-5" or \
            raw["locale"] != "zh-Hans":
        raise ContentError("lesson: invalid schema/locale/age")
    identity = (raw["lesson_uid"], raw["game_id"],
                raw["content_id"], raw["title"])
    standard = (LESSON_UID, "preschool_math", LESSON_ID, "形状与数量")
    unit_identity = None
    for name, (uid, game_id, number, title, kind) in UNIT_COURSES.items():
        if raw["lesson_uid"] == uid:
            unit_identity = (uid, game_id, number, title, kind)
            break
    if identity != standard and (
            unit_identity is None or identity != unit_identity[:4]):
        raise ContentError("lesson: unexpected identity")
    if type(raw["content_id"]) is not int or not 20000 <= raw["content_id"] < 40000:
        raise ContentError("lesson: content_id must be in 20000..39999")
    if type(raw["revision"]) is not int or not 1 <= raw["revision"] <= 0xFFFFFFFF:
        raise ContentError("lesson: revision invalid")
    _text(raw["title"], "title", 40)
    if raw["review"] != {"status": "draft"}:
        raise ContentError("lesson: auto-approval forbidden")
    source = raw["source"]
    if not isinstance(source, dict) or set(source) != {
            "origin", "reference_url", "copied_assets", "notes"}:
        raise ContentError("lesson: provenance missing")
    if source["origin"] != "original-preschool-exercises" or \
            source["reference_url"] != "https://github.com/KDE/gcompris" or \
            source["copied_assets"] is not False:
        raise ContentError("lesson: original content provenance required")
    _text(source["notes"], "source.notes", 300)
    rounds = raw["rounds"]
    if not isinstance(rounds, list) or not 1 <= len(rounds) <= MAX_LESSON_ROUNDS:
        raise ContentError("lesson: 1..64 rounds required")
    if unit_identity and (len(rounds) != 5 or
                          any(r.get("kind") != unit_identity[4] for r in rounds
                              if isinstance(r, dict))):
        raise ContentError("lesson: unit course requires five rounds of its own skill")
    for n, item in enumerate(rounds, 1):
        where = f"round {n}"
        if not isinstance(item, dict) or set(item) != {
                "kind", "skill", "target", "question_art",
                "prompt", "answer", "options"}:
            raise ContentError(f"{where}: malformed")
        kind = item["kind"]
        if kind not in KIND_SKILL or item["skill"] != KIND_SKILL[kind]:
            raise ContentError(f"{where}: unsupported kind/skill")
        _text(item["prompt"], f"{where} prompt", 100)
        arts = item["options"]
        if not isinstance(arts, list) or len(arts) not in (2, 3):
            raise ContentError(f"{where}: need 2-3 options")
        if any(not isinstance(o, dict) or set(o) != {"art", "label"} for o in arts):
            raise ContentError(f"{where}: invalid option fields")
        pictures = [o["art"] for o in arts]
        if len(set(pictures)) != len(pictures) or any(a not in game_pack.ART for a in pictures):
            raise ContentError(f"{where}: invalid/repeated artwork")
        for o in arts:
            _text(o["label"], f"{where} option label", 24)
            if o != _option(o["art"]):
                raise ContentError(f"{where}: wrong option label for artwork")
        if item["answer"] not in pictures:
            raise ContentError(f"{where}: answer not among choices")
        target = item["target"]
        if kind == "match_shape":
            if target not in SHAPES or item["question_art"] != target or \
                    item["answer"] != target or not set(pictures) <= set(SHAPES):
                raise ContentError(f"{where}: incorrect shape question")
        elif kind == "count":
            if target not in {str(i) for i in range(1, 6)} or \
                    item["question_art"] != "blocks" + target or \
                    item["answer"] != "dots" + target or \
                    any(not a.startswith("dots") for a in pictures):
                raise ContentError(f"{where}: incorrect counting question")
        else:
            if target not in ("big", "small") or item["question_art"] != "listen" or \
                    item["answer"] != "ball_" + target or \
                    set(pictures) != {"ball_big", "ball_small"} or len(pictures) != 2:
                raise ContentError(f"{where}: incorrect big/small question")
            expected_target = "big" if ("大" in item["prompt"]) else "small"
            if ("大" in item["prompt"] and "小" in item["prompt"]) or \
                    (not any(word in item["prompt"] for word in ("大", "小"))) or \
                    target != expected_target:
                raise ContentError(f"{where}: size prompt/answer inconsistent")
    return raw


def audio_key(text: str) -> str:
    """Stable content-addressed audio slot ID, not a voice or audio fingerprint."""
    return "clip_" + hashlib.sha256(text.encode("utf-8")).hexdigest()[:20]


def audio_script(lesson: dict) -> dict:
    validate_lesson(lesson)
    clips: dict[str, str] = {}
    for r in lesson["rounds"]:
        for utterance in [r["prompt"]] + [o["label"] for o in r["options"]]:
            key = audio_key(utterance)
            if key in clips and clips[key] != utterance:
                raise ContentError("audio key collision")
            clips[key] = utterance
    return {"schema_version": 1, "locale": lesson["locale"],
            "clips": dict(sorted(clips.items()))}


def audio_map(lesson: dict) -> dict:
    script = audio_script(lesson)
    return {"schema_version": 1,
            "clips": {key: f"clips/{key}.wav" for key in script["clips"]}}
