#!/usr/bin/env python3
"""Live Book Dash pilot: fetch 12 real illustrations, stage 24 EPD previews.

This script is for RESEARCH VALIDATION ONLY. Narration may be synthetic
test tones or real unreviewed Chinese TTS. It never generates .kpb or approves
content. All source imagery retains Book Dash CC BY 4.0 attribution.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from content_pipeline.assets import fetch_bookdash_images, combine_book_assets
from content_pipeline.packaging import stage_book
from content_pipeline.review_packet import make_review_packet
from content_pipeline.pilot import PILOT_REF, PILOT_SOURCE, prepare_cat_pilot
from content_pipeline.schema import ContentError, write_json
from content_pipeline.source_snapshots import fetch_bookdash_source
from content_pipeline.speech import narrate_book
from tools import game_pack

ROOT = Path(__file__).resolve().parent.parent
TRANSLATIONS = ROOT / "curriculum/pilots/come_back_cat.zh_draft.json"
CREDITS = ROOT / "curriculum/pilots/come_back_cat.credits.json"


def _synthetic_tone(text: str, wav: Path, voice: str) -> None:
    game_pack.write_wav(wav, game_pack.tone(
        [350 + min(400, len(text) * 9)], note_s=0.12))


def build_research_preview(workdir: Path, *, narration: str = "synthetic") -> dict:
    """Create review-ready pages but never approve or compile child content."""
    if narration not in ("synthetic", "edge"):
        raise ContentError("live source: unsupported speech backend")
    workdir = Path(workdir)
    source_dir = workdir / "source"
    pilot_dir = workdir / "pilot"
    assets_dir = workdir / "assets"
    staged_dir = workdir / "staged"
    source = fetch_bookdash_source(
        source_item_id=PILOT_SOURCE, source_ref=PILOT_REF, content_id=1101,
        license_id="unknown", skills=["language.story_sequence"],
        output_dir=source_dir)
    if source["pages"] != 12 or source["image_only_pages"] != [5, 12]:
        raise ContentError("live source: page count or wordless-page structure changed")
    pilot = prepare_cat_pilot(
        source_dir / "original.lcir.json", TRANSLATIONS, CREDITS, pilot_dir)
    lcir = pilot_dir / "come_back_cat.zh.lcir.json"
    images = fetch_bookdash_images(lcir, assets_dir)
    if len(images["images"]) != 12:
        raise ContentError("live source: not all 12 photos were downloaded")
    for number, entry in enumerate(images["images"], 1):
        if entry["page"] != number or not entry["source_url"].endswith(
                f"/{number:02d}.jpg"):
            raise ContentError("live source: illustration order mismatch")
    if narration == "synthetic":
        audio = narrate_book(lcir, assets_dir, voice="zh-CN-XiaoxiaoNeural",
                             synthesize=_synthetic_tone, test_only=True)
        if audio["status"] != "test_only":
            raise ContentError("live source: unsafe fixture status")
        combine_book_assets(lcir, assets_dir, allow_test_artifacts=True)
    else:
        # The external service creates genuinely spoken Chinese WAVs, but
        # neither the words nor voice quality are automatically approved.
        audio = narrate_book(lcir, assets_dir, voice="zh-CN-XiaoxiaoNeural")
        if audio["status"] != "draft":
            raise ContentError("live source: real TTS must remain unreviewed draft")
        combine_book_assets(lcir, assets_dir)
    candidate = stage_book(lcir, assets_dir / "assets.json", staged_dir)
    if len(candidate["preview_files"]) != 24 or len(candidate["stage_files"]) != 24:
        raise ContentError("live source: expected 12 images, 12 tones and 24 previews")
    review_packet = make_review_packet(staged_dir)
    if review_packet["pages"] != 12:
        raise ContentError("live source: reviewer page count mismatch")
    if list(workdir.rglob("*.kpb")):
        raise ContentError("live source: unexpected child-facing package")
    report = {
        "schema_version": "BOOKLIVESMOKE1",
        "status": "test_only" if narration == "synthetic" else "draft",
        "source_url": source["source_url"],
        "git_sha": source["source_ref"],
        "source_sha256": source["sha256"],
        "english_wordless_pages": source["image_only_pages"],
        "zh_pages": pilot["pages"],
        "source_images": len(images["images"]),
        "image_sha256": [x["sha256"] for x in images["images"]],
        "audio_clips": len(audio["audio"]),
        "audio_provider": audio["provider"],
        "audio_status": audio["status"],
        "epaper_previews": len(candidate["preview_files"]),
        "review_contact_sheets": 2,
        "review_html_local": "staged/review/index.html",
        "preview_sha256": candidate["checked_hash"],
        "license": "CC-BY-4.0",
        "credits": {
            "writer": "Nicola Rijsdijk",
            "illustrator": "Karen Lilje",
            "designer": "Sam Scarborough",
        },
        "caveat": (
            "Real pinned Book Dash illustrations and AI-assisted Chinese text; "
            + ("test-only sine tones." if narration == "synthetic" else
               "real unreviewed Chinese TTS. No audio redistribution approved.")
            + " Not adult reviewed; not usable child content."
        ),
    }
    write_json(workdir / "live-report.json", report)
    (workdir / "README-RESEARCH-ONLY.txt").write_text(
        "DO NOT DISTRIBUTE AS APPROVED CHILD CONTENT.\n"
        "Illustrations: Come back, cat! (2014), Book Dash, CC BY 4.0.\n"
        "Writer Nicola Rijsdijk; illustrator Karen Lilje; designer Sam Scarborough.\n"
        "Source: https://bookdash.org/books/come-back-cat/\n"
        "License: https://creativecommons.org/licenses/by/4.0/\n"
        "Chinese text is an unreviewed AI-assisted adaptation.\n"
        "Preview PNGs reflect KPB2 mono/4-grey quantization, not actual hardware tests.\n"
        + ("Audio comprises test-only sine tones, not narration.\n" if
           narration == "synthetic" else
         "Audio is online synthesized Chinese narration, NOT adult reviewed.\n"),
        encoding="utf-8")
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--narration", choices=("synthetic", "edge"),
                        default="synthetic", help="Default is test tones; edge uses real unreviewed online TTS")
    args = parser.parse_args()
    report = build_research_preview(args.output_dir, narration=args.narration)
    print(json.dumps({
        "status": report["status"],
        "source_sha256": report["source_sha256"],
        "images": report["source_images"],
        "previews": report["epaper_previews"],
        "zh_pages": report["zh_pages"],
        "audio_clips": report["audio_clips"],
        "audio_provider": report["audio_provider"],
        "external_git_sha": report["git_sha"],
    }, ensure_ascii=False, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
