"""Import Global ASP `zh/NNNN_....md` story markdown from a pinned local copy.

Global ASP markdown contains ## page separators and a metadata footer.
Illustration files are NOT embedded or inferred from the markdown. This adapter
therefore returns a **draft** with image_ref=None, never a book ready for a child.
"""
from __future__ import annotations

import re

from content_pipeline.adapters.common import join_paragraphs, make_book_draft
from content_pipeline.schema import ContentError, safe_relpath

TITLE = re.compile(r"^# (.+)$", re.MULTILINE)
SEPARATOR = re.compile(r"(?m)^##\s*$")
FOOTER = re.compile(
    r"^\*\s*(License|Text|Illustration|Translation|Language):\s*(.*?)\s*$",
    re.MULTILINE,
)
STORY_ID = re.compile(r"^([0-9]{4})_[^/]+\.md$")


def import_global_asp(markdown: str, *, source_item_id: str, source_url: str,
                      source_ref: str, content_id: int, revision: int = 1,
                      locale: str = "zh-Hans", skills: list[str] | None = None,
                      license_id: str = "unknown") -> dict:
    safe_relpath(source_item_id, "source_item_id")
    match = STORY_ID.fullmatch(source_item_id.split("/")[-1])
    if not match or not source_item_id.startswith("zh/"):
        raise ContentError("Global ASP: expected zh/NNNN_title.md source path")
    header = TITLE.search(markdown)
    if header is None or not markdown.startswith("# "):
        raise ContentError("Global ASP: first line must be a story title")
    title = header.group(1).strip()
    blocks = SEPARATOR.split(markdown)
    if len(blocks) < 2:
        raise ContentError("Global ASP: missing ## story page separators")
    if blocks[0].strip() != header.group(0).strip():
        raise ContentError("Global ASP: unexpected content before first page")
    metadata: dict[str, str] = {}
    pages: list[tuple[str, None]] = []
    for raw in blocks[1:]:
        for key, value in FOOTER.findall(raw):
            if key in metadata:
                raise ContentError(f"Global ASP: duplicate {key} metadata")
            metadata[key] = value.strip().strip("[]")
        body = join_paragraphs(FOOTER.sub("", raw), locale)
        if body:
            pages.append((body, None))
    if not pages or any(k not in metadata for k in ("License", "Text", "Illustration", "Language")):
        raise ContentError("Global ASP: story pages or metadata missing")
    if metadata["Language"] != "zh" or locale != "zh-Hans":
        raise ContentError("Global ASP: only source Chinese locale supported")
    raw_license = metadata["License"]
    if not re.fullmatch(r"CC-BY(?:-NC)?", raw_license):
        raise ContentError("Global ASP: unrecognized raw license")
    if license_id != "unknown" and license_id.rsplit("-", 1)[0] != raw_license:
        raise ContentError("Global ASP: exact license conflicts with source footer")
    creators = {
        "text": [x.strip() for x in metadata["Text"].split(",") if x.strip()],
        "illustration": [x.strip() for x in metadata["Illustration"].split(",") if x.strip()],
        "translation": [x.strip() for x in metadata.get("Translation", "").split(",") if x.strip()],
    }
    return make_book_draft(
        provider="global-asp", source_text=markdown,
        source_item_id=source_item_id, source_url=source_url,
        source_ref=source_ref,
        item_uid=f"global-asp:{match.group(1)}:zh-hans:original",
        title=title, locale=locale, content_id=content_id, revision=revision,
        skills=skills or [], license_id=license_id, license_hint=raw_license,
        creators=creators, pages=pages,
    )
