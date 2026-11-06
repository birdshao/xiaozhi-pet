"""Stage 15 original math game rounds and build KPG1 after adult review.

Never silently synthesize speech or approve a pack. 24 kHz PCM16 speech WAVs
must be provided for every utterance listed by game_lessons.audio_script().
"""
from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path
import shutil

from content_pipeline.game_lessons import audio_key, audio_script, validate_lesson
from content_pipeline.packaging import _asset, _hash, _sha_file
from content_pipeline.schema import ContentError, canonical_json, write_json
from tools import game_pack

MAX_GAME_WAV = game_pack.MAX_CLIP_BYTES + 1024


def _audio_map(mapping: object, required: set[str]) -> dict[str, str]:
    if not isinstance(mapping, dict) or set(mapping) != {"schema_version", "clips"} or \
            mapping["schema_version"] != 1 or not isinstance(mapping["clips"], dict):
        raise ContentError("game audio map: schema_version=1 and clips required")
    clips = mapping["clips"]
    if set(clips) != required:
        raise ContentError("game audio map: clip keys missing or extra")
    return clips


def _manifest(stage_dir: Path) -> dict:
    lesson_file = stage_dir / "lesson.json"
    game_file = stage_dir / "game.json"
    attr_file = stage_dir / "attribution.json"
    lesson = validate_lesson(json.loads(lesson_file.read_text(encoding="utf-8")))
    game = json.loads(game_file.read_text(encoding="utf-8"))
    attr = json.loads(attr_file.read_text(encoding="utf-8"))
    expected = set(audio_script(lesson)["clips"])
    used = set()
    for item in game["rounds"]:
        used.add(item["prompt_audio"])
        for option in item["options"]:
            used.add(option["audio"])
    required_files = {f"clips/{key}.wav" for key in expected}
    if used != required_files or len(game["rounds"]) != len(lesson["rounds"]):
        raise ContentError("game: staged clip/round mismatch")
    files = {}
    for rel in sorted(used):
        f = _asset(stage_dir, rel, max_size=MAX_GAME_WAV)
        files[rel] = _sha_file(f, MAX_GAME_WAV)
        game_pack.read_wav(f, rel)
    return {
        "schema_version": 1,
        "lesson_sha256": _hash(lesson_file.read_bytes()),
        "game_sha256": _hash(game_file.read_bytes()),
        "attribution_sha256": _hash(attr_file.read_bytes()),
        "audio_files": files,
    }


def stage_game(lesson_file: Path, audio_map_file: Path, stage_dir: Path) -> dict:
    """Stage recorded WAVs and unapproved game.json. Never creates .kpg."""
    lesson_file, audio_map_file, stage_dir = map(
        Path, (lesson_file, audio_map_file, stage_dir))
    lesson = validate_lesson(json.loads(lesson_file.read_text(encoding="utf-8")))
    script = audio_script(lesson)
    clips = _audio_map(json.loads(audio_map_file.read_text(encoding="utf-8")),
                       set(script["clips"]))
    asset_root = audio_map_file.parent.resolve(strict=True)
    output_root = stage_dir.resolve()
    if output_root == asset_root or asset_root.is_relative_to(output_root):
        raise ContentError("stage must not overwrite original audio directory")
    stage_dir.mkdir(parents=True, exist_ok=True)
    (stage_dir / "clips").mkdir(exist_ok=True)
    provenance = []
    for key, text in script["clips"].items():
        ref = clips[key]
        source = _asset(audio_map_file.parent, ref, max_size=MAX_GAME_WAV)
        if source.is_relative_to(output_root):
            raise ContentError("source audio cannot be inside stage directory")
        # Existing KPG1 reader is the single authority for audio duration.
        game_pack.read_wav(source, text)
        target = stage_dir / "clips" / f"{key}.wav"
        shutil.copyfile(source, target)
        provenance.append({
            "clip_key": key, "spoken_text": text, "source_path": ref,
            "source_sha256": _sha_file(source, MAX_GAME_WAV),
        })
    audio_test_only = False
    spoken_manifest_file = audio_map_file.parent / "audio.manifest.json"
    if spoken_manifest_file.exists():
        spoken = json.loads(spoken_manifest_file.read_text(encoding="utf-8"))
        if spoken.get("schema_version") != "GAMEAUDIO1" or \
                spoken.get("status") not in ("draft", "test_only") or \
                spoken.get("lesson_uid") != lesson["lesson_uid"] or \
                spoken.get("lesson_sha256") != _hash(lesson_file.read_bytes()):
            raise ContentError("game: generated audio manifest stale or invalid")
        entries = spoken.get("audio")
        if not isinstance(entries, list) or len(entries) != len(provenance):
            raise ContentError("game: audio manifest incomplete")
        by_key = {entry.get("clip_key"): entry for entry in entries if isinstance(entry, dict)}
        if len(by_key) != len(provenance):
            raise ContentError("game: audio manifest duplicate clips")
        for record in provenance:
            manifest_record = by_key.get(record["clip_key"])
            if manifest_record is None or \
                    manifest_record.get("path") != record["source_path"] or \
                    manifest_record.get("sha256") != record["source_sha256"] or \
                    manifest_record.get("spoken_text") != record["spoken_text"]:
                raise ContentError("game: TTS clip mismatched or tampered")
        audio_test_only = spoken["status"] == "test_only"
    rounds = []
    for r in lesson["rounds"]:
        rounds.append({
            "kind": r["kind"], "prompt": r["prompt"],
            "prompt_audio": f"clips/{audio_key(r['prompt'])}.wav",
            "question_art": r["question_art"],
            "answer": r["answer"], "approved": False,
            "options": [{
                "art": option["art"], "label": option["label"],
                "audio": f"clips/{audio_key(option['label'])}.wav",
            } for option in r["options"]],
        })
    game = {
        "schema_version": 1, "package_type": "game",
        "game_id": lesson["game_id"], "content_id": lesson["content_id"],
        "revision": lesson["revision"], "title": lesson["title"],
        "reviewer": "pending", "approved": False, "rounds": rounds,
    }
    write_json(stage_dir / "lesson.json", lesson)
    write_json(stage_dir / "game.json", game)
    write_json(stage_dir / "attribution.json", {
        "lesson_uid": lesson["lesson_uid"], "source": lesson["source"],
        "audio_sources": provenance, "audio_test_only": audio_test_only,
        "notice": (
            "Original game prompts and builtin artwork. WAV recordings and "
            "educational content must be reviewed by an adult."),
    })
    candidate = _manifest(stage_dir)
    candidate["checked_hash"] = _hash(canonical_json(candidate))
    write_json(stage_dir / "candidate.json", candidate)
    return candidate


def build_reviewed_game(stage_dir: Path, review_file: Path, output: Path,
                        *, smoke_only: bool = False) -> dict:
    """Use the existing KPG1 encoder only after review bound to staged bytes."""
    stage_dir, review_file, output = map(Path, (stage_dir, review_file, output))
    if output.name != "default.kpg":
        raise ContentError("current firmware only loads /games/default.kpg")
    candidate = json.loads((stage_dir / "candidate.json").read_text(encoding="utf-8"))
    current = _manifest(stage_dir)
    current["checked_hash"] = _hash(canonical_json(current))
    if candidate != current:
        raise ContentError("staged game content changed since candidate generation")
    review = json.loads(review_file.read_text(encoding="utf-8"))
    if not isinstance(review, dict) or set(review) != {
            "status", "reviewer", "checked_hash", "attestations"}:
        raise ContentError("review: expected status, reviewer, checked_hash, attestations")
    if review["status"] != "reviewed" or review["checked_hash"] != current["checked_hash"]:
        raise ContentError("review: explicit reviewed status with current hash required")
    reviewer = review["reviewer"]
    if not isinstance(reviewer, str) or not 1 <= len(reviewer.strip()) <= 40:
        raise ContentError("review: adult name required")
    attests = review["attestations"]
    required = {"educational_accuracy", "answer_uniqueness", "audio",
                "age_safety", "source"}
    if not isinstance(attests, dict) or set(attests) != required or any(
            attests[item] is not True for item in required):
        raise ContentError("review: five explicit checks required")
    attribution = json.loads((stage_dir / "attribution.json").read_text(encoding="utf-8"))
    if attribution.get("audio_test_only") is True and not smoke_only:
        raise ContentError("game: synthetic test-only TTS requires smoke_only packaging")
    game = copy.deepcopy(json.loads((stage_dir / "game.json").read_text(encoding="utf-8")))
    game["approved"] = True
    game["reviewer"] = reviewer.strip()
    for round_ in game["rounds"]:
        round_["approved"] = True
    temporary = stage_dir / "__reviewed_game_temp.json"
    if temporary.exists():
        raise ContentError("reviewed temporary source already exists")
    try:
        write_json(temporary, game)
        result = game_pack.make_pack(temporary, output,
                                     reviewed=True, placeholder=smoke_only)
    finally:
        temporary.unlink(missing_ok=True)
    return {
        "checked_hash": current["checked_hash"],
        "sha256": result["sha256"],
        "rounds": len(result["rounds"]),
        "clips": len(result["clips"]),
        "package_bytes": result["bytes"],
        "sd_path": result["sd_path"],
        "placeholder": result["placeholder"],
    }
