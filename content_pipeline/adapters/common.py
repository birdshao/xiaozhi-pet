"""Shared provenance-aware draft factory for offline source adapters."""
from __future__ import annotations

import hashlib
from pathlib import PurePosixPath
from typing import Any

from content_pipeline.schema import ContentError, safe_relpath, validate_document


def make_book_draft(*, provider: str, source_text: str, source_item_id: str,
                    source_url: str, source_ref: str, item_uid: str, title: str,
                    locale: str, content_id: int, revision: int, skills: list[str],
                    license_id: str, license_hint: str | None,
                    creators: dict[str, list[str]],
                    pages: list[tuple[str, str | None]]) -> dict[str, Any]:
    safe_relpath(source_item_id, "source_item_id")
    if not pages:
        raise ContentError("source: no pages found")
    doc = {
        "schema_version": "LCIR1",
        "item_uid": item_uid,
        "kind": "picture_book",
        "title": title,
        "locale": locale,
        "age_band": "3-5",
        "content_id": content_id,
        "revision": revision,
        "skills": skills,
        "source": {
            "provider": provider,
            "source_item_id": source_item_id,
            "source_url": source_url,
            "source_ref": source_ref,
            "license_id": license_id,
            "license_hint": license_hint,
            "creators": creators,
            "source_files": [{
                "path": source_item_id,
                "sha256": hashlib.sha256(source_text.encode("utf-8")).hexdigest()
            }],
            "attribution_required": True,
        },
        "transformations": [],
        "pages": [{
            "number": number,
            "text": text,
            "image_ref": image_ref,
            "image": None,
            "audio": None,
            "image_mode": "grey",
            "skills": skills[:],
            "questions": [],
        } for number, (text, image_ref) in enumerate(pages, 1)],
        "review": {
            "status": "draft",
            "reviewer": None,
            "checked_hash": None,
            "checked_at": None,
            "notes": "Imported text only. Resolve/inspect images, synthesize and "
                     "listen to audio, verify source license, and review before packing.",
        },
    }
    return validate_document(doc)
