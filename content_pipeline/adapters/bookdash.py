"""Import Book Dash GitHub Markdown index.md from a pinned local snapshot.

Supported V1 form: YAML frontmatter, # title, and alternating Markdown
images and paragraph text. Asset paths are *references* awaiting explicit
resolution, not fake downloaded files. Author/illustrator metadata generally
needs a separate per-book attribution review before approval.
"""
from __future__ import annotations

import re

from content_pipeline.adapters.common import make_book_draft
from content_pipeline.schema import ContentError, safe_relpath

IMAGE = re.compile(r"!\[([^\]]*)\]\(([^)]+)\)")
SLUG = re.compile(r"^[a-z0-9]+(?:-[a-z0-9]+)*$")


def _image_path(raw: str) -> str:
    raw = raw.strip()
    prefix = "{{ site.image-set }}/"
    if raw.startswith(prefix):
        raw = "images/" + raw[len(prefix):]
    if not raw.startswith("images/"):
        raise ContentError("Book Dash: only local images/ references are supported")
    return safe_relpath(raw, "Book Dash image")


def _frontmatter(markdown: str) -> tuple[dict[str, str], str]:
    if not markdown.startswith("---\n"):
        raise ContentError("Book Dash: YAML frontmatter required")
    end = markdown.find("\n---\n", 4)
    if end < 0:
        raise ContentError("Book Dash: frontmatter not terminated")
    raw = markdown[4:end]
    meta: dict[str, str] = {}
    for line in raw.splitlines():
        if ":" not in line:
            raise ContentError("Book Dash: unsupported frontmatter structure")
        key, value = line.split(":", 1)
        meta[key.strip()] = value.strip().strip(chr(34) + chr(39))
    return meta, markdown[end + 5:]


def import_bookdash(markdown: str, *, source_item_id: str, source_url: str,
                    source_ref: str, content_id: int, revision: int = 1,
                    locale: str = "en", skills: list[str] | None = None,
                    license_id: str = "unknown") -> dict:
    safe_relpath(source_item_id, "source_item_id")
    parts = source_item_id.split("/")
    if len(parts) != 3 or parts[2] != "index.md" or not SLUG.fullmatch(parts[0]):
        raise ContentError("Book Dash: expected book-slug/en/index.md")
    if parts[1] != locale:
        raise ContentError("Book Dash: locale/source path mismatch")
    meta, body = _frontmatter(markdown)
    heading = re.match(r"\s*# ([^\n]+)\n", body)
    if not heading:
        raise ContentError("Book Dash: missing story heading")
    title = meta.get("title") or heading.group(1).strip()
    # Any prose preceding the first image should be flagged instead of lost.
    rest = body[heading.end():]
    marks = list(IMAGE.finditer(rest))
    if not marks:
        raise ContentError("Book Dash: no images in story")
    if rest[:marks[0].start()].strip():
        raise ContentError("Book Dash: orphan text before first image")
    pages: list[tuple[str, str]] = []
    for i, mark in enumerate(marks):
        next_start = marks[i + 1].start() if i + 1 < len(marks) else len(rest)
        text = rest[mark.end():next_start].strip()
        text = re.sub(r"\n{3,}", "\n\n", text)
        # Image-only pages are intentional, e.g. Come Back, Cat! page 5.
        # Preserve exactly as empty instead of fabricating original text.
        pages.append((text, _image_path(mark.group(2))))
    return make_book_draft(
        provider="bookdash", source_text=markdown,
        source_item_id=source_item_id, source_url=source_url,
        source_ref=source_ref,
        item_uid=f"bookdash:{parts[0]}:{locale.lower()}:original",
        title=title.strip(), locale=locale, content_id=content_id,
        revision=revision, skills=skills or [], license_id=license_id,
        license_hint="Book Dash book-level license requires verification",
        creators={"text": [], "illustration": [], "translation": []},
        pages=pages,
    )
