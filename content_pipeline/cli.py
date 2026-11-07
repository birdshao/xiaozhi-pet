"""Import pinned local source Markdown into *unreviewed* LCIR1 JSON.

Example:
    python -m content_pipeline.cli import-global-asp \
      --input research_content/sources/0008.md \
      --source-path 'zh/0008_你在做什么.md' \
      --ref 0123456789012345678901234567890123456789 \
      --content-id 1001 --skill language.action_verbs \
      --output research_content/normalized/asp-0008.json \
      --registry research_content/catalog/ids.json

The import stage NEVER downloads from the network or sets approved=true.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
from urllib.parse import quote

from content_pipeline.adapters import import_bookdash, import_global_asp
from content_pipeline.schema import (ContentError, content_digest,
                                     register_document, validate_document,
                                     write_json)


def _url(provider: str, ref: str, path: str) -> str:
    repo = ("global-asp/global-asp" if provider == "global-asp"
            else "bookdash/bookdash-books")
    return f"https://github.com/{repo}/blob/{ref}/{quote(path, safe='/')}"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    for command, provider in (("import-global-asp", "global-asp"),
                              ("import-bookdash", "bookdash")):
        p = sub.add_parser(command)
        p.set_defaults(provider=provider)
        p.add_argument("--input", type=Path, required=True,
                       help="Local UTF-8 Markdown snapshot; never fetched remotely")
        p.add_argument("--source-path", required=True,
                       help="Path inside the pinned upstream GitHub repository")
        p.add_argument("--ref", required=True, help="Full 40-digit upstream commit SHA")
        p.add_argument("--content-id", type=int, required=True,
                       help="Reserved book ID 1000..19999")
        p.add_argument("--revision", type=int, default=1)
        p.add_argument("--skill", action="append", default=[])
        p.add_argument("--license-id", default="unknown",
                       help="Exact verified license, otherwise keep unknown/draft")
        p.add_argument("--locale", default="zh-Hans" if provider == "global-asp" else "en")
        p.add_argument("--output", required=True, type=Path)
        p.add_argument("--registry", type=Path,
                       help="Stable ID registry; strongly recommended for multiple books")
    p = sub.add_parser("check")
    p.add_argument("--input", type=Path, required=True)
    p = sub.add_parser("stage-book", help="Render draft pages and bind review hash")
    p.add_argument("--input", type=Path, required=True, help="LCIR1 JSON file")
    p.add_argument("--assets", type=Path, required=True, help="Local asset-map JSON")
    p.add_argument("--output-dir", type=Path, required=True)
    p = sub.add_parser("build-book", help="Build existing KPB2 with human review proof")
    p.add_argument("--stage-dir", type=Path, required=True)
    p.add_argument("--review-file", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p = sub.add_parser("make-math-game", help="Generate 15 original math draft rounds and WAV script")
    p.add_argument("--output-dir", type=Path, required=True)
    p = sub.add_parser("check-game", help="Validate math lesson draft and correct answers")
    p.add_argument("--input", type=Path, required=True)
    p = sub.add_parser("stage-game", help="Prepare KPG1 game with external speech WAVs")
    p.add_argument("--input", type=Path, required=True, help="GAMELESSON1 JSON draft")
    p.add_argument("--audio-map", type=Path, required=True, help="WAV file locations")
    p.add_argument("--output-dir", type=Path, required=True)
    p = sub.add_parser("build-game", help="Build default.kpg only after adult review")
    p.add_argument("--stage-dir", type=Path, required=True)
    p.add_argument("--review-file", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True, help="SD games/default.kpg")
    p = sub.add_parser("build-course-index", help="Verify KPB2/KPG1 packages and map skills")
    p.add_argument("--course", type=Path, required=True)
    p.add_argument("--skills", type=Path, required=True)
    p.add_argument("--package", type=Path, action="append", required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--test-only", action="store_true",
                   help="Explicitly label the entire index test-only; allow tone fixtures")
    p = sub.add_parser("fetch-bookdash-images",
                       help="Download referenced JPEG pages from exact upstream Git SHA")
    p.add_argument("--input", type=Path, required=True, help="Imported Book Dash LCIR1")
    p.add_argument("--output-dir", type=Path, required=True)
    p.add_argument("--replace", action="store_true",
                   help="Explicitly replace images/manifest already in this research folder")
    p = sub.add_parser("tts-book", help="Use optional Edge TTS + ffmpeg for page narration")
    p.add_argument("--input", type=Path, required=True)
    p.add_argument("--output-dir", type=Path, required=True)
    p.add_argument("--voice", required=True, help="Locale-matched neural voice, e.g. zh-CN-XiaoxiaoNeural")
    p.add_argument("--replace", action="store_true")
    p = sub.add_parser("tts-game", help="Deduplicate and synthesize Chinese math prompts")
    p.add_argument("--input", type=Path, required=True)
    p.add_argument("--output-dir", type=Path, required=True)
    p.add_argument("--voice", default="zh-CN-XiaoxiaoNeural")
    p.add_argument("--replace", action="store_true")
    p = sub.add_parser("prepare-book-assets",
                       help="Verify downloaded images and recorded WAVs before stage-book")
    p.add_argument("--input", type=Path, required=True)
    p.add_argument("--assets-dir", type=Path, required=True)
    p.add_argument("--output", type=Path,
                   help="Must be placed inside --assets-dir (default assets.json)")
    p = sub.add_parser("localize-bookdash",
                       help="Convert page-aligned Chinese draft into a pinned LCIR1 translation")
    p.add_argument("--input", type=Path, required=True, help="Original English LCIR1")
    p.add_argument("--translations", type=Path, required=True, help="Page-aligned zh-Hans JSON")
    p.add_argument("--content-id", type=int, required=True)
    p.add_argument("--revision", type=int, default=1)
    p.add_argument("--registry", type=Path)
    p.add_argument("--output", type=Path, required=True)
    p = sub.add_parser("fetch-bookdash-source",
                       help="Fetch pinned upstream Markdown and import real wordless pages")
    p.add_argument("--source-path", required=True)
    p.add_argument("--ref", required=True, help="Full 40-char upstream Git SHA")
    p.add_argument("--content-id", type=int, required=True)
    p.add_argument("--license-id", default="unknown")
    p.add_argument("--skill", action="append", default=[])
    p.add_argument("--output-dir", type=Path, required=True)
    p.add_argument("--replace", action="store_true")
    p = sub.add_parser("prepare-cat-pilot",
                       help="Apply verified Book Dash author credits and Chinese draft")
    p.add_argument("--input", type=Path, required=True, help="Original English LCIR1")
    p.add_argument("--translations", type=Path, required=True)
    p.add_argument("--credits", type=Path, required=True)
    p.add_argument("--output-dir", type=Path, required=True)
    p.add_argument("--replace", action="store_true")
    p = sub.add_parser("make-review-packet",
                       help="Create read-only EPD contact sheets and local WAV review HTML")
    p.add_argument("--stage-dir", type=Path, required=True)
    p = sub.add_parser("make-game-review-packet",
                       help="Review 15 math game rounds and play staged local WAVs")
    p.add_argument("--stage-dir", type=Path, required=True)
    p = sub.add_parser("make-review-template",
                       help="Generate unsigned, all-false parent review checklist")
    p.add_argument("--stage-dir", type=Path, required=True)
    p.add_argument("--kind", choices=("book", "game"), required=True)
    p.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.command == "make-review-template":
            from content_pipeline.review_templates import make_review_template
            result = make_review_template(args.stage_dir, kind=args.kind,
                                          output=args.output)
            print(json.dumps(result, ensure_ascii=False))
            return 0
        if args.command == "make-game-review-packet":
            from content_pipeline.game_review import make_game_review_packet
            result = make_game_review_packet(args.stage_dir)
            print(json.dumps({"status": result["status"],
                              "rounds": len(result["rounds"]),
                              "unique_audio_clips": result["unique_audio_clips"],
                              "checked_hash": result["checked_hash"]},
                             ensure_ascii=False))
            return 0
        if args.command == "make-review-packet":
            from content_pipeline.review_packet import make_review_packet
            result = make_review_packet(args.stage_dir)
            print(json.dumps(result, ensure_ascii=False))
            return 0
        if args.command == "fetch-bookdash-source":
            from content_pipeline.source_snapshots import fetch_bookdash_source
            result = fetch_bookdash_source(
                source_item_id=args.source_path, source_ref=args.ref,
                content_id=args.content_id, output_dir=args.output_dir,
                license_id=args.license_id, skills=args.skill, replace=args.replace)
            print(json.dumps(result, ensure_ascii=False))
            return 0
        if args.command == "prepare-cat-pilot":
            from content_pipeline.pilot import prepare_cat_pilot
            result = prepare_cat_pilot(
                args.input, args.translations, args.credits, args.output_dir,
                replace=args.replace)
            print(json.dumps(result, ensure_ascii=False))
            return 0
        if args.command == "localize-bookdash":
            from content_pipeline.localize import localize_bookdash
            document = localize_bookdash(args.input, args.translations,
                                         args.output, content_id=args.content_id,
                                         revision=args.revision)
            if args.registry:
                register_document(args.registry, document)
            print(json.dumps({"status": "draft", "item_uid": document["item_uid"],
                              "pages": len(document["pages"]),
                              "sha256": content_digest(document)}, ensure_ascii=False))
            return 0
        if args.command == "fetch-bookdash-images":
            from content_pipeline.assets import fetch_bookdash_images
            result = fetch_bookdash_images(args.input, args.output_dir, replace=args.replace)
            print(json.dumps({"status": result["status"],
                              "images": len(result["images"]),
                              "source_git_sha": result["source_git_sha"]}, ensure_ascii=False))
            return 0
        if args.command in ("tts-book", "tts-game"):
            from content_pipeline.speech import narrate_book, narrate_game
            if args.command == "tts-book":
                result = narrate_book(args.input, args.output_dir, voice=args.voice,
                                      replace=args.replace)
            else:
                result = narrate_game(args.input, args.output_dir, voice=args.voice,
                                      replace=args.replace)
            print(json.dumps({"status": result["status"], "provider": result["provider"],
                              "clips": len(result["audio"])}, ensure_ascii=False))
            return 0
        if args.command == "prepare-book-assets":
            from content_pipeline.assets import combine_book_assets
            result = combine_book_assets(args.input, args.assets_dir, args.output)
            print(json.dumps({"pages": len(result["pages"]), "status": "draft"},
                             ensure_ascii=False))
            return 0
        if args.command == "build-course-index":
            from content_pipeline.catalog import build_index
            result = build_index(args.course, args.skills, args.package,
                                 args.output, allow_test_artifacts=args.test_only)
            print(json.dumps({"course": result["course_id"],
                              "status": result["status"],
                              "packages": len(result["packages"]),
                              "units": len(result["units"])}, ensure_ascii=False))
            return 0
        if args.command == "make-math-game":
            from content_pipeline.game_lessons import (
                original_math_lesson, audio_script, audio_map)
            lesson = original_math_lesson()
            write_json(args.output_dir / "lesson.json", lesson)
            write_json(args.output_dir / "audio_script.json", audio_script(lesson))
            write_json(args.output_dir / "audio_map.json", audio_map(lesson))
            print(json.dumps({"lesson": str(args.output_dir / "lesson.json"),
                              "status": "draft", "rounds": len(lesson["rounds"]),
                              "speech_clips": len(audio_script(lesson)["clips"])},
                             ensure_ascii=False))
            return 0
        if args.command == "check-game":
            from content_pipeline.game_lessons import validate_lesson, audio_script
            lesson = validate_lesson(json.loads(args.input.read_text(encoding="utf-8")))
            print(json.dumps({"status": "draft", "rounds": len(lesson["rounds"]),
                              "speech_clips": len(audio_script(lesson)["clips"])},
                             ensure_ascii=False))
            return 0
        if args.command in ("stage-game", "build-game"):
            from content_pipeline.game_packaging import stage_game, build_reviewed_game
            if args.command == "stage-game":
                result = stage_game(args.input, args.audio_map, args.output_dir)
            else:
                result = build_reviewed_game(args.stage_dir, args.review_file, args.output)
            print(json.dumps(result, ensure_ascii=False))
            return 0
        if args.command in ("stage-book", "build-book"):
            from content_pipeline.packaging import stage_book, build_reviewed_book
            if args.command == "stage-book":
                result = stage_book(args.input, args.assets, args.output_dir)
            else:
                result = build_reviewed_book(args.stage_dir, args.review_file, args.output)
            print(json.dumps(result, ensure_ascii=False))
            return 0
        if args.command == "check":
            document = validate_document(json.loads(args.input.read_text(encoding="utf-8")))
        else:
            text = args.input.read_text(encoding="utf-8")
            importer = (import_global_asp if args.provider == "global-asp"
                        else import_bookdash)
            document = importer(text, source_item_id=args.source_path,
                                source_url=_url(args.provider, args.ref, args.source_path),
                                source_ref=args.ref, content_id=args.content_id,
                                revision=args.revision, skills=args.skill,
                                locale=args.locale, license_id=args.license_id)
            if args.registry:
                register_document(args.registry, document)
            write_json(args.output, document)
        print(json.dumps({"item_uid": document["item_uid"],
                          "status": document["review"]["status"],
                          "pages": len(document["pages"]),
                          "sha256": content_digest(document)}, ensure_ascii=False))
        return 0
    except (ValueError, OSError, json.JSONDecodeError) as exc:
        print(f"CONTENT_IMPORT_ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
