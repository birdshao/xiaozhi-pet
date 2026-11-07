"""Fetch a Book Dash Markdown story from an exact, immutable Git commit.

This is the missing real-world entrypoint between the remote source archive
and the offline Book Dash LCIR1 adapter. No arbitrary URLs are accepted;
online access remains optional and tests can inject a byte fetcher.
"""
from __future__ import annotations

import hashlib
import re
from pathlib import Path
from typing import Callable
from urllib.parse import quote

from content_pipeline.adapters import import_bookdash
from content_pipeline.assets import RAW_HOST, _fetch_https, _atomic_bytes
from content_pipeline.schema import ContentError, REF, safe_relpath, write_json

PATH = re.compile(r"^[a-z0-9]+(?:-[a-z0-9]+)*/en/index\.md$")
MAX_MD_BYTES = 256 * 1024
Fetch = Callable[[str, int], bytes]


def pinned_markdown_url(source_item_id: str, source_ref: str) -> str:
    safe_relpath(source_item_id, "source_item_id")
    if not PATH.fullmatch(source_item_id):
        raise ContentError("pinned source: expected book-slug/en/index.md")
    if not isinstance(source_ref, str) or not REF.fullmatch(source_ref):
        raise ContentError("pinned source: expected full 40-digit Git commit SHA")
    return RAW_HOST + source_ref.lower() + "/" + quote(source_item_id, safe="/")


def fetch_bookdash_source(*, source_item_id: str, source_ref: str,
                          content_id: int, output_dir: Path,
                          license_id: str = "unknown",
                          skills: list[str] | None = None,
                          fetch: Fetch | None = None,
                          replace: bool = False) -> dict:
    """Download and import pinned source, atomically saving original Markdown.

    license_id is caller-provided, never inferred from a repository-wide
    default. Imported content remains a draft, including image-only pages.
    """
    url = pinned_markdown_url(source_item_id, source_ref)
    out = Path(output_dir)
    src_file = out / "source.md"
    lcir_file = out / "original.lcir.json"
    manifest_file = out / "source.manifest.json"
    if not replace and any(p.exists() for p in
                           (src_file, lcir_file, manifest_file)):
        raise ContentError("source snapshot already exists; use explicit --replace")
    downloader = fetch or _fetch_https
    raw = downloader(url, MAX_MD_BYTES)
    if not isinstance(raw, bytes) or not 1 <= len(raw) <= MAX_MD_BYTES:
        raise ContentError("pinned source: missing or oversized Markdown")
    try:
        text = raw.decode("utf-8", errors="strict")
    except UnicodeDecodeError as exc:
        raise ContentError("pinned source: invalid UTF-8") from exc
    if "\x00" in text:
        raise ContentError("pinned source: NUL byte is forbidden")
    doc = import_bookdash(
        text, source_item_id=source_item_id,
        source_url="https://github.com/bookdash/bookdash-books/blob/" +
                   source_ref.lower() + "/" + quote(source_item_id, safe="/"),
        source_ref=source_ref.lower(), content_id=content_id,
        skills=skills or [], locale="en", license_id=license_id)
    sha256 = hashlib.sha256(raw).hexdigest()
    if doc["source"]["source_files"][0]["sha256"] != sha256:
        raise ContentError("pinned source: unexpected Markdown SHA")
    out.mkdir(parents=True, exist_ok=True)
    _atomic_bytes(src_file, raw)
    write_json(lcir_file, doc)
    manifest = {
        "schema_version": "BOOKSOURCE1",
        "status": "draft",
        "source_item_id": source_item_id,
        "source_ref": source_ref.lower(),
        "source_url": url,
        "sha256": sha256,
        "bytes": len(raw),
        "pages": len(doc["pages"]),
        "image_only_pages": [page["number"] for page in doc["pages"]
                             if not page["text"]],
        "lcir_path": "original.lcir.json",
    }
    write_json(manifest_file, manifest)
    return manifest
