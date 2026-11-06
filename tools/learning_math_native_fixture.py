"""Generate a strictly TEST-ONLY KPG1 fixture from the actual 15 math rounds.

This is for native C++/scene/input/audio tests. Speech can be synthetic
or real unreviewed Mandarin TTS; no parent authorization is claimed.

The existing KPG1 tool's placeholder=True is mandatory. The transient
"approved" source is created *solely for this throwaway binary test fixture*;
the original stage remains unapproved. Never use this output as lesson content.
"""
from __future__ import annotations

import argparse
import copy
import json
from pathlib import Path

from content_pipeline.game_lessons import original_math_lesson, audio_script
from content_pipeline.game_packaging import stage_game
from content_pipeline.schema import ContentError, write_json
from content_pipeline.speech import narrate_game
from tools import game_pack

VOICE = "zh-CN-XiaoxiaoNeural"


def _tone(text: str, path: Path, voice: str) -> None:
    # The synthetic tone's pitch depends on length, not linguistic content.
    game_pack.write_wav(path, game_pack.tone(
        [350 + min(40, len(text)) * 11], note_s=0.12))


def make_native_fixture(workdir: Path, *, narration: str = "synthetic") -> dict:
    """Create 15-round, 19-clip placeholder KPG1 in an ephemeral test folder."""
    if narration not in ("synthetic", "edge"):
        raise ContentError("device fixture: unsupported voice engine")
    root = Path(workdir)
    if root.exists() and any(root.iterdir()):
        raise ContentError("device fixture: refusing to overwrite existing output")
    root.mkdir(parents=True, exist_ok=True)
    src_dir, stage_dir = root / "source", root / "stage"
    src_dir.mkdir()
    lesson = original_math_lesson()
    source = src_dir / "lesson.json"
    write_json(source, lesson)
    if narration == "synthetic":
        speech = narrate_game(source, src_dir, voice=VOICE,
                              synthesize=_tone, test_only=True)
    else:
        speech = narrate_game(source, src_dir, voice=VOICE)
    expected = "test_only" if narration == "synthetic" else "draft"
    if speech["status"] != expected:
        raise ContentError("device fixture: mismatched unreviewed voice status")
    candidate = stage_game(source, src_dir / "audio_map.json", stage_dir)
    game = json.loads((stage_dir / "game.json").read_text(encoding="utf-8"))
    if game["approved"] is not False or any(r["approved"] is not False
                                             for r in game["rounds"]):
        raise ContentError("device fixture: staging cannot approve child content")
    # Only the existing KPG1 encoder recognizes this ephemeral test fixture
    # as reviewed. The test-only flag is retained in the sidecar manifest.
    ephemeral = copy.deepcopy(game)
    ephemeral["approved"] = True
    ephemeral["reviewer"] = "CI placeholder fixture ONLY"
    for r in ephemeral["rounds"]:
        r["approved"] = True
    scratch = stage_dir / "__test_fixture_for_encoder.json"
    pack = root / "games" / "default.kpg"
    try:
        write_json(scratch, ephemeral)
        manifest = game_pack.make_pack(scratch, pack, reviewed=True, placeholder=True)
    finally:
        scratch.unlink(missing_ok=True)
    decoded = game_pack.parse(pack.read_bytes())
    if len(decoded["rounds"]) != 15 or len(decoded["clips"]) != len(
            audio_script(lesson)["clips"]) or not manifest["placeholder"]:
        raise ContentError("device fixture: encoder/decoder produced unexpected pack")
    if not all(entry["sha256"] for entry in manifest["clips"]):
        raise ContentError("device fixture: clip digest missing")
    summary = {
        "format": "NATIVEGAMEFIXTURE1",
        "status": "test_only",
        "voice_provider": speech["provider"],
        "original_audio_status": speech["status"],
        "content_id": decoded["content_id"],
        "revision": decoded["revision"],
        "rounds": len(decoded["rounds"]),
        "clips": len(decoded["clips"]),
        "kpg_sha256": manifest["sha256"],
        "candidate_hash": candidate["checked_hash"],
        "path": "games/default.kpg",
        "manifest": "games/default.manifest.json",
        "warning": (
            "KPG1 CI test placeholder only. Online speech is UNREVIEWED. "
            "NEVER deploy this file as child-facing course content."),
    }
    write_json(root / "device-fixture-report.json", summary)
    (root / "README-TEST-ONLY.txt").write_text(
        "XIAOZHI PET KPG1 NATIVE DEVICE TEST - NOT APPROVED\n"
        f"15 math questions / original GameArt ids / {speech['provider']} WAV.\n"
        "Review simulation exists only to exercise the binary test encoder.\n"
        "KPG1 manifest placeholder=true. Do not use as child content.\n",
        encoding="utf-8")
    return summary


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--narration", choices=("synthetic", "edge"),
                        default="synthetic")
    args = parser.parse_args(argv)
    result = make_native_fixture(args.output_dir, narration=args.narration)
    print(json.dumps(result, ensure_ascii=False, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
