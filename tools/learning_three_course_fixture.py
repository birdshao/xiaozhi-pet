"""TEST-ONLY three-course TF setup for C++ GameCourseLoader integration.

All WAVs are synthetic tones; no external speech or adult review. Never
upload any generated .kpg as child-usable curriculum.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
from pathlib import Path
import shutil

from content_pipeline.game_lessons import original_math_unit_lessons
from content_pipeline.game_packaging import stage_game
from content_pipeline.schema import ContentError, write_json
from content_pipeline.speech import narrate_game
from tools import game_pack
from tools.learning_course_sd import stage_course_card
from tools.learning_math_native_fixture import _tone, make_native_fixture

ROOT = Path(__file__).resolve().parent.parent
COURSE = ROOT / "curriculum/courses/math_units.v1.json"
SKILLS = ROOT / "curriculum/skills.v1.json"


def make_three_course_fixture(output: Path) -> dict:
    root = Path(output)
    if root.exists() and any(root.iterdir()):
        raise ContentError("course fixture: refusing to overwrite files")
    root.mkdir(parents=True, exist_ok=True)
    fixture_dir = root / "fixtures"
    fixture_dir.mkdir()
    entries = []
    for name, lesson in original_math_unit_lessons().items():
        src = root / "sources" / name
        stage = root / "staging" / name
        src.mkdir(parents=True)
        lesson_path = src / "lesson.json"
        write_json(lesson_path, lesson)
        voice = narrate_game(lesson_path, src,
                             voice="zh-CN-XiaoxiaoNeural",
                             synthesize=_tone, test_only=True)
        if voice["status"] != "test_only":
            raise ContentError("course fixture: incorrect test audio marker")
        staged = stage_game(lesson_path, src / "audio_map.json", stage)
        if staged["checked_hash"] == "":
            raise ContentError("course fixture: missing staging hash")
        game = json.loads((stage / "game.json").read_text(encoding="utf-8"))
        if game["approved"] is not False:
            raise ContentError("course fixture: original staging must remain a draft")
        encoder = copy.deepcopy(game)
        encoder["approved"] = True
        encoder["reviewer"] = "CI test fixture not a real person"
        for item in encoder["rounds"]:
            item["approved"] = True
        ephemeral = stage / "__ci_fixture_encoder.json"
        path = fixture_dir / (name + ".kpg")
        try:
            write_json(ephemeral, encoder)
            manifest = game_pack.make_pack(
                ephemeral, path, reviewed=True, placeholder=True)
        finally:
            ephemeral.unlink(missing_ok=True)
        decoded = game_pack.parse(path.read_bytes())
        if decoded["content_id"] != lesson["content_id"] or len(decoded["rounds"]) != 5:
            raise ContentError("course fixture: unexpected KPG1 binary")
        entries.append({"pack": name, "content_id": decoded["content_id"],
                        "rounds": len(decoded["rounds"]),
                        "clips": len(decoded["clips"]),
                        "sha256": manifest["sha256"]})
    # A separate full mixed default KPG1 validates the compatibility fallback.
    mixed = make_native_fixture(root / "full_default")
    fallback = root / "full_default/games/default.kpg"
    shutil.copyfile(fallback, fixture_dir / "default.kpg")
    card = stage_course_card(
        course_file=COURSE, skills_file=SKILLS,
        packages=[fixture_dir / (name + ".kpg")
                  for name in original_math_unit_lessons()],
        active="counts", output=root / "sdcard", test_only=True)
    if card["status"] != "test_only" or card["active"] != "counts":
        raise ContentError("course fixture: wrong active selection")
    result = {
        "status": "test_only", "active": card["active"],
        "packs": entries,
        "default_id": mixed["content_id"],
        "sd_games": "sdcard/games",
        "warning": "Only synthetic sound tones, no parent review or child package.",
    }
    write_json(root / "course-fixture-report.json", result)
    return result


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--output-dir", type=Path, required=True)
    args = p.parse_args(argv)
    print(json.dumps(make_three_course_fixture(args.output_dir),
                     ensure_ascii=False, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
