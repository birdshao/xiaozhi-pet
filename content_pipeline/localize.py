"""Explicit page-aligned Chinese adaptation of a pinned Book Dash English draft.

No machine translation is silently performed. A human or an external
translation tool supplies a Chinese draft JSON which is kept tied to the
original source digest and must still undergo adult content review.
"""
from __future__ import annotations

import copy
import json
from pathlib import Path

from content_pipeline.schema import ContentError, content_digest, validate_document, write_json


def localize_bookdash(source_file: Path, translations_file: Path, output: Path,
                      *, content_id: int, revision: int = 1) -> dict:
    source_file, translations_file, output = map(Path, (source_file, translations_file, output))
    origin = validate_document(json.loads(source_file.read_text(encoding="utf-8")))
    if origin["locale"] != "en" or origin["source"]["provider"] != "bookdash" or \
            origin["transformations"]:
        raise ContentError("translation: requires original English Book Dash draft")
    raw = json.loads(translations_file.read_text(encoding="utf-8"))
    if not isinstance(raw, dict) or set(raw) != {
            "schema_version", "title", "translator", "pages"} or \
            raw["schema_version"] != 1:
        raise ContentError("translation: expected schema_version, title, translator, pages")
    title, translator = raw["title"], raw["translator"]
    if not isinstance(title, str) or not title.strip() or len(title) > 80:
        raise ContentError("translation: missing Chinese title")
    if not isinstance(translator, str) or not translator.strip() or len(translator) > 80:
        raise ContentError("translation: responsible translator/reviewer field required")
    texts = raw["pages"]
    if not isinstance(texts, list) or len(texts) != len(origin["pages"]) or \
            not all(isinstance(t, str) and 1 <= len(t.strip()) <= 320 for t in texts):
        raise ContentError("translation: exactly one <=320-character sentence block per page")
    if content_id == origin["content_id"]:
        raise ContentError("translation: distinct content_id is required from English book")
    if output.resolve() in (source_file.resolve(), translations_file.resolve()):
        raise ContentError("translation: refusing to overwrite input")
    result = copy.deepcopy(origin)
    slug = origin["source"]["source_item_id"].split("/")[0]
    result["item_uid"] = f"bookdash:{slug}:zh-hans:translation"
    result["title"] = title.strip()
    result["locale"] = "zh-Hans"
    result["content_id"] = content_id
    result["revision"] = revision
    result["source"]["creators"]["translation"].append(translator.strip())
    result["transformations"] = [{
        "kind": "translation", "source_locale": "en",
        "target_locale": "zh-Hans", "source_digest": content_digest(origin),
        "method": "manual_draft",
        "notes": "Page-aligned Chinese draft, not yet approved by an adult.",
    }]
    for page, text in zip(result["pages"], texts):
        page["text"] = text.strip()
    result["review"] = {
        "status": "draft", "reviewer": None, "checked_hash": None,
        "checked_at": None,
        "notes": "Verify translation meaning, names, text length and original image pairings.",
    }
    validate_document(result)
    write_json(output, result)
    return result
