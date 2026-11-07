#!/usr/bin/env python3
"""Offline/online research run for the original 15-round math mini-game.

Creates 15 draft rounds and real or synthetic WAV clips, verifies every clip
with the current KPG1 reader, then creates adult-only local review HTML. No
approval, no .kpg, no firmware changes, no upload of TTS audio.

Example:
 python -m tools.learning_live_math_pilot \
   --output-dir research_content/math_game --narration edge
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

from content_pipeline.game_lessons import (
    audio_map, audio_script, original_math_lesson, validate_lesson)
from content_pipeline.game_packaging import stage_game
from content_pipeline.game_review import make_game_review_packet
from content_pipeline.schema import ContentError, write_json
from content_pipeline.speech import narrate_game
from tools import game_pack

VOICE = "zh-CN-XiaoxiaoNeural"


def _tone_fixture(text: str, target: Path, voice: str) -> None:
    """Distinct synthetic tones for CI structure validation, never narration."""
    game_pack.write_wav(
        target, game_pack.tone([310 + min(len(text), 45) * 8], note_s=0.12))


def build_math_research(workdir: Path, *, narration: str = "synthetic") -> dict:
    """Generate an explicit unreviewed research packet with 15 validated rounds."""
    if narration not in ("synthetic", "edge"):
        raise ContentError("math research: unsupported voice engine")
    root = Path(workdir)
    generated = root / "generated"
    stage = root / "staged"
    generated.mkdir(parents=True, exist_ok=True)
    lesson = original_math_lesson()
    validate_lesson(lesson)
    path = generated / "lesson.json"
    if path.exists():
        raise ContentError("math research: output directory is not empty")
    write_json(path, lesson)
    script = audio_script(lesson)
    if not 1 <= len(script["clips"]) <= game_pack.MAX_CLIPS:
        raise ContentError("math research: generated too many audio clips")
    write_json(generated / "audio_script.json", script)
    write_json(generated / "audio_map.json", audio_map(lesson))
    if narration == "synthetic":
        speech = narrate_game(
            path, generated, voice=VOICE,
            synthesize=_tone_fixture, test_only=True)
    else:
        speech = narrate_game(path, generated, voice=VOICE)
        if speech.get("status") != "draft":
            raise ContentError("math research: online TTS must remain unreviewed")
    candidate = stage_game(path, generated / "audio_map.json", stage)
    review = make_game_review_packet(stage)
    if len(review["rounds"]) != 15 or review["unique_audio_clips"] != len(
            script["clips"]):
        raise ContentError("math research: incomplete review packet")
    if narration == "synthetic" and review["status"] != "test_only":
        raise ContentError("math research: test tones were mislabeled")
    if narration == "edge" and review["status"] != "unreviewed":
        raise ContentError("math research: real TTS must be an unreviewed draft")
    if list(root.rglob("*.kpg")):
        raise ContentError("math research: generated an unapproved game package")
    meta = {
        "schema_version": "MATHLIVESMOKE1",
        "status": review["status"],
        "source": "original prompts; GCompris teaching concepts only",
        "content_id": lesson["content_id"],
        "revision": lesson["revision"],
        "rounds": len(lesson["rounds"]),
        "speech_engine": speech["provider"],
        "voice": speech["voice"],
        "unique_audio_clips": review["unique_audio_clips"],
        "total_audio_ms": review["total_audio_ms"],
        "lesson_sha256": speech["lesson_sha256"],
        "candidate_sha256": candidate["checked_hash"],
        "review_file": "staged/review/index.html",
        "warnings": [
            "Adult review required: prompts, answer uniqueness and all spoken audio.",
            "The HTML artwork is schematic and NOT an exact device screenshot.",
            "This is not a device-installed or an approved .kpg content package.",
        ],
    }
    write_json(root / "live-report.json", meta)
    (root / "README-RESEARCH-ONLY.txt").write_text(
        "XIAOZHI PET ORIGINAL MATH LEARNING GAME - RESEARCH ONLY\n"
        "15 original rounds: 5 shapes, 5 counting, 5 size comparisons.\n"
        "Teaching concepts may overlap GCompris; no code/images/audio were copied.\n"
        f"Audio provider: {speech['provider']}; status: {speech['status']}.\n"
        "Audio must be listened to and explicitly approved by an adult before .kpg packaging.\n"
        "The review UI has diagrams, not actual ESP32 GameArt pixels.\n"
        "Local WAV references inside review/index.html are not in public CI artifacts.\n",
        encoding="utf-8",
    )
    return meta


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--narration", choices=["synthetic", "edge"],
                        default="synthetic")
    args = parser.parse_args(argv)
    summary = build_math_research(args.output_dir, narration=args.narration)
    print(json.dumps({
        "status": summary["status"], "rounds": summary["rounds"],
        "unique_audio_clips": summary["unique_audio_clips"],
        "speech_engine": summary["speech_engine"],
        "total_audio_ms": summary["total_audio_ms"],
    }, ensure_ascii=False, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
