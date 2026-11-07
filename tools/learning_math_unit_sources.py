"""Write three separate draft GAMELESSON1 sources (no audio, no approvals).

The original exercise generator stays the single source of truth. Each
new source requires its own adult-reviewed WAVs and KPG1 build.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

from content_pipeline.game_lessons import original_math_unit_lessons
from content_pipeline.schema import ContentError, write_json


def write_unit_sources(output: Path) -> dict:
    output = Path(output)
    if output.exists() and any(output.iterdir()):
        raise ContentError("unit sources: refusing to overwrite existing courses")
    docs = original_math_unit_lessons()
    output.mkdir(parents=True, exist_ok=True)
    for name, lesson in docs.items():
        target = output / name
        target.mkdir()
        write_json(target / "lesson.json", lesson)
    result = {
        "status": "draft",
        "units": [{"name": name, "game_id": doc["game_id"],
                   "content_id": doc["content_id"],
                   "rounds": len(doc["rounds"]),
                   "lesson": f"{name}/lesson.json"} for name, doc in docs.items()],
        "warning": "Original teaching prompts only, NO audio or adult review.",
    }
    write_json(output / "unit-sources.json", result)
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args(argv)
    print(json.dumps(write_unit_sources(args.output_dir), ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
