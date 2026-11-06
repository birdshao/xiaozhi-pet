"""Create a strictly attributed Chinese draft for the first real Book Dash pilot.

This function cannot approve a story, cannot synthesize sound, and never
copies source images into the public Git repository. Translator metadata must
identify AI assistance until reviewed/corrected by an adult.
"""
from __future__ import annotations

import copy
import json
from pathlib import Path

from content_pipeline.localize import localize_bookdash
from content_pipeline.schema import ContentError, content_digest, validate_document, write_json

PILOT_REF = "54916310c5c06a5282e2d5d4add18d64f90e5fde"
PILOT_SOURCE = "come-back-cat/en/index.md"
PILOT_TITLE = "Come back, cat!"


def prepare_cat_pilot(source_doc: Path, translations: Path, credits: Path,
                      output_dir: Path, *, replace: bool = False) -> dict:
    """Match a pinned upstream 12-page source to author credits and zh draft."""
    source_doc, translations, credits, output_dir = map(
        Path, (source_doc, translations, credits, output_dir))
    original = validate_document(json.loads(source_doc.read_text(encoding="utf-8")))
    source = original["source"]
    if (source["provider"] != "bookdash" or
            source["source_item_id"] != PILOT_SOURCE or
            source["source_ref"] != PILOT_REF or
            original["title"] != PILOT_TITLE or
            original["locale"] != "en" or original["transformations"] or
            len(original["pages"]) != 12 or
            [p["number"] for p in original["pages"] if p["text"] == ""] != [5, 12]):
        raise ContentError("pilot: not the expected pinned 12-page Book Dash source")
    meta = json.loads(credits.read_text(encoding="utf-8"))
    expected = {"schema_version", "source_item_id", "source_ref",
                "title", "license_id", "license_url", "official_book_url",
                "writer", "illustrator", "designer", "attribution_notice",
                "review_status"}
    if not isinstance(meta, dict) or set(meta) != expected:
        raise ContentError("pilot: credits file missing or unexpected fields")
    if (meta["schema_version"] != 1 or meta["source_item_id"] != PILOT_SOURCE or
            meta["source_ref"] != PILOT_REF or meta["title"] != PILOT_TITLE or
            meta["license_id"] != "CC-BY-4.0" or
            meta["license_url"] != "https://creativecommons.org/licenses/by/4.0/" or
            meta["official_book_url"] != "https://bookdash.org/books/come-back-cat/" or
            meta["writer"] != "Nicola Rijsdijk" or
            meta["illustrator"] != "Karen Lilje" or
            meta["designer"] != "Sam Scarborough" or
            meta["review_status"] != "draft"):
        raise ContentError("pilot: credits differ from the documented official book")
    if not isinstance(meta["attribution_notice"], str) or len(
            meta["attribution_notice"]) > 400:
        raise ContentError("pilot: invalid attribution notice")

    curated = copy.deepcopy(original)
    curated["source"]["license_id"] = "CC-BY-4.0"
    curated["source"]["license_hint"] = (
        "Book Dash CC BY 4.0; Sam Scarborough is credited as designer")
    curated["source"]["creators"] = {
        "text": [meta["writer"]], "illustration": [meta["illustrator"]],
        "translation": [],
    }
    validate_document(curated)

    curated_file = output_dir / "curated.en.lcir.json"
    translated_file = output_dir / "come_back_cat.zh.lcir.json"
    citation_file = output_dir / "attribution.json"
    if not replace and any(f.exists() for f in
                           (curated_file, translated_file, citation_file)):
        raise ContentError("pilot: outputs already exist; require explicit --replace")
    write_json(curated_file, curated)
    translated = localize_bookdash(
        curated_file, translations, translated_file, content_id=1301, revision=1)
    write_json(citation_file, {
        "schema_version": "PILOTCREDITS1", "status": "draft",
        "source_ref": PILOT_REF, "source_item_id": PILOT_SOURCE,
        "source_sha256": original["source"]["source_files"][0]["sha256"],
        "source_url": meta["official_book_url"],
        "license_id": meta["license_id"],
        "license_url": meta["license_url"],
        "creators": {
            "writer": meta["writer"], "illustrator": meta["illustrator"],
            "designer": meta["designer"],
            "translation": translated["source"]["creators"]["translation"],
        },
        "modification_notice": meta["attribution_notice"],
        "original_digest": content_digest(original),
        "curated_digest": content_digest(curated),
        "chinese_digest": content_digest(translated),
        "review_required": True,
    })
    return {
        "status": "draft", "title": translated["title"],
        "pages": len(translated["pages"]),
        "source_git_sha": PILOT_REF,
        "lcir_path": str(translated_file),
        "original_text_sha256": original["source"]["source_files"][0]["sha256"],
    }
