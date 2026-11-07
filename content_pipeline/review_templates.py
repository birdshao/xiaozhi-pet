"""Produce hash-pinned, intentionally UNSIGNED parent review checklists.

A checklist always starts with every attestation false and reviewer blank.
It cannot be directly passed to KPB2/KPG1 build. An adult must actually
review all pages, recordings and answers and manually complete a separate
copy with status='reviewed', a name and five true confirmations.
"""
from __future__ import annotations

import json
from pathlib import Path

from content_pipeline.schema import ContentError, canonical_json, write_json
from content_pipeline.packaging import _core_manifest, _hash
from content_pipeline.game_packaging import _manifest

BOOK_CHECKS = ("source_license", "illustrations", "text", "audio", "age_safety")
GAME_CHECKS = ("educational_accuracy", "answer_uniqueness",
               "audio", "age_safety", "source")


def make_review_template(stage_dir: Path, *, kind: str,
                         output: Path | None = None) -> dict:
    if kind not in ("book", "game"):
        raise ContentError("review checklist: expected book or game")
    stage = Path(stage_dir)
    candidate = json.loads((stage / "candidate.json").read_text(encoding="utf-8"))
    core = _core_manifest(stage) if kind == "book" else _manifest(stage)
    core["checked_hash"] = _hash(canonical_json(core))
    if core != candidate:
        raise ContentError("review checklist: staged content changed; stage again")
    attestations = {
        name: False for name in (BOOK_CHECKS if kind == "book" else GAME_CHECKS)
    }
    template = {
        "status": "review_pending",
        "reviewer": "",
        "checked_hash": core["checked_hash"],
        "attestations": attestations,
    }
    target = Path(output) if output else stage / "review.template.json"
    if target.exists():
        raise ContentError("review checklist: refusing to overwrite existing file")
    write_json(target, template)
    return {
        "status": template["status"],
        "checked_hash": template["checked_hash"],
        "required_checks": list(attestations),
        "file": str(target),
        "warning": "All checks default false. Human reviewer must sign a separate copy.",
    }
