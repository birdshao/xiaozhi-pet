"""Pinned Book Dash illustration downloader and verified KPB2 asset-map assembly.

Network access is confined to a constructed raw.githubusercontent.com URL at
the exact upstream Git SHA. No user-supplied URL, redirects, cross-host CDN,
non-image content or unbounded response is followed. Imported files remain
UNREVIEWED local research assets, never committed to the public repository.
"""
from __future__ import annotations

import hashlib
import io
import json
import os
from pathlib import Path
import tempfile
from typing import Callable
from urllib.parse import quote
from urllib.request import HTTPRedirectHandler, Request, build_opener

from content_pipeline.schema import (
    ContentError, content_digest, safe_relpath, validate_document, write_json,
)

MAX_IMAGE_BYTES = 8 * 1024 * 1024
MAX_IMAGE_PIXELS = 20_000_000
RAW_HOST = "https://raw.githubusercontent.com/bookdash/bookdash-books/"


class _NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        raise ContentError("image download: redirects are not permitted")


def _fetch_https(url: str, limit: int = MAX_IMAGE_BYTES) -> bytes:
    if not url.startswith(RAW_HOST):
        raise ContentError("image download: unexpected URL origin")
    request = Request(url, headers={
        "User-Agent": "xiaozhi-pet-content-import/1.0",
        "Accept": "image/jpeg",
    })
    opener = build_opener(_NoRedirect)
    with opener.open(request, timeout=25) as response:
        if response.status != 200:
            raise ContentError("image download: HTTP status not 200")
        size = response.headers.get("Content-Length")
        if size is not None and (not size.isdigit() or int(size) > limit):
            raise ContentError("image download: declared size exceeds limit")
        contents = response.read(limit + 1)
    if not contents or len(contents) > limit:
        raise ContentError("image download: empty or exceeds byte limit")
    return contents


def _image_bytes(data: bytes) -> None:
    """Decode check before saving a public image; do not interpret any SVG."""
    if len(data) > MAX_IMAGE_BYTES:
        raise ContentError("image: exceeds maximum byte size")
    try:
        from PIL import Image, ImageFile
        Image.MAX_IMAGE_PIXELS = MAX_IMAGE_PIXELS
        ImageFile.LOAD_TRUNCATED_IMAGES = False
        with Image.open(io.BytesIO(data)) as img:
            if img.format != "JPEG":
                raise ContentError("Book Dash: expected JPEG illustration")
            if not (1 <= img.width <= 6000 and 1 <= img.height <= 6000) or \
                    img.width * img.height > MAX_IMAGE_PIXELS:
                raise ContentError("image: resolution out of bounds")
            img.verify()
    except (OSError, ValueError, Image.DecompressionBombError) as exc:
        raise ContentError(f"image: malformed or unsafe JPEG: {exc}") from exc


def _atomic_bytes(path: Path, value: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=path.parent, prefix=".fetch-",
                                     delete=False) as handle:
        tmp = Path(handle.name)
        try:
            handle.write(value)
            handle.flush()
            os.fsync(handle.fileno())
        except BaseException:
            tmp.unlink(missing_ok=True)
            raise
    os.replace(tmp, path)


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def bookdash_url(doc: dict, image_ref: str) -> str:
    validate_document(doc)
    source = doc["source"]
    if source["provider"] != "bookdash":
        raise ContentError("image download: provider must be Book Dash")
    source_locale = source["source_item_id"].split("/")[1]
    if doc["locale"] != source_locale and not (
            doc["locale"] == "zh-Hans" and source_locale == "en" and
            len(doc["transformations"]) == 1 and
            doc["transformations"][0]["kind"] == "translation"):
        raise ContentError("image download: source locale mismatch")
    rel = safe_relpath(image_ref, "image_ref")
    if not rel.startswith("images/") or not rel.lower().endswith(".jpg"):
        raise ContentError("image download: expected images/*.jpg from Book Dash")
    prefix = source["source_item_id"].rsplit("/", 1)[0]
    upstream = f"{prefix}/{rel}"
    return RAW_HOST + source["source_ref"].lower() + "/" + quote(upstream, safe="/")


def fetch_bookdash_images(lcir_file: Path, output_dir: Path,
                          *, fetch: Callable[[str, int], bytes] | None = None,
                          replace: bool = False) -> dict:
    """Download original story images; return draft-only provenance manifest.

    The caller may inject a byte fetcher for fully offline tests. The CLI does
    NOT offer arbitrary URLs or a mock fetcher.
    """
    doc = validate_document(json.loads(Path(lcir_file).read_text(encoding="utf-8")))
    if doc["source"]["provider"] != "bookdash":
        raise ContentError("only Book Dash has resolvable image references in V1")
    base = Path(output_dir)
    manifest_path = base / "images.manifest.json"
    if manifest_path.exists() and not replace:
        raise ContentError("images manifest already exists; use explicit --replace")
    result = []
    downloader = fetch or _fetch_https
    for p in doc["pages"]:
        ref = p["image_ref"]
        if ref is None:
            raise ContentError(f"page {p['number']}: no upstream image reference")
        url = bookdash_url(doc, ref)
        data = downloader(url, MAX_IMAGE_BYTES)
        if not isinstance(data, bytes):
            raise ContentError("image download: fetcher did not return bytes")
        _image_bytes(data)
        rel = f"images/page_{p['number']:02d}.jpg"
        out = base / rel
        if out.exists() and not replace:
            raise ContentError(f"image exists: {rel}; rerun requires --replace")
        _atomic_bytes(out, data)
        result.append({"page": p["number"], "source_ref": ref,
                       "source_url": url, "path": rel, "sha256": _sha256(data),
                       "bytes": len(data)})
    manifest = {
        "schema_version": "BOOKIMAGES1",
        "status": "draft",
        "item_uid": doc["item_uid"],
        "content_digest": content_digest(doc),
        "source_git_sha": doc["source"]["source_ref"],
        "images": result,
    }
    write_json(manifest_path, manifest)
    return manifest


def combine_book_assets(lcir_file: Path, assets_dir: Path,
                        output: Path | None = None, *,
                        allow_test_artifacts: bool = False) -> dict:
    """Verify two independently generated manifests before stage-book."""
    doc = validate_document(json.loads(Path(lcir_file).read_text(encoding="utf-8")))
    base = Path(assets_dir)
    imgs = json.loads((base / "images.manifest.json").read_text(encoding="utf-8"))
    audio = json.loads((base / "audio.manifest.json").read_text(encoding="utf-8"))
    for label, manifest, schema in (
            ("images", imgs, "BOOKIMAGES1"), ("audio", audio, "BOOKAUDIO1")):
        allowed = {"draft", "test_only"} if label == "audio" and allow_test_artifacts \
            else {"draft"}
        if manifest.get("schema_version") != schema or manifest.get("status") not in allowed \
                or manifest.get("item_uid") != doc["item_uid"] \
                or manifest.get("content_digest") != content_digest(doc):
            raise ContentError(f"{label}: stale, mismatched, or invalid manifest")
    if len(imgs["images"]) != len(doc["pages"]) or len(audio["audio"]) != len(doc["pages"]):
        raise ContentError("assets: page count mismatch")
    pages = {}
    from content_pipeline.packaging import _asset
    for i, page in enumerate(doc["pages"]):
        n = i + 1
        image, recording = imgs["images"][i], audio["audio"][i]
        if image["page"] != n or recording["page"] != n:
            raise ContentError(f"page {n}: image/audio order mismatch")
        if image["source_ref"] != page["image_ref"]:
            raise ContentError(f"page {n}: image source ref mismatch")
        if recording["text_sha256"] != _sha256(page["text"].encode("utf-8")):
            raise ContentError(f"page {n}: recorded text changed")
        for entry, key, cap in ((image, "image", MAX_IMAGE_BYTES),
                                (recording, "audio", 3 * 1024 * 1024)):
            file_path = _asset(base, entry["path"], max_size=cap)
            if _sha256(file_path.read_bytes()) != entry["sha256"]:
                raise ContentError(f"page {n}: {key} changed after creation")
        pages[str(n)] = {"image": image["path"], "audio": recording["path"]}
    mapped = {"schema_version": 1, "pages": pages}
    dest = Path(output) if output is not None else base / "assets.json"
    # StageBook requires paths relative to the asset map's own directory.
    if dest.resolve().parent != base.resolve():
        raise ContentError("asset map must be in the images/audio asset directory")
    write_json(dest, mapped)
    return mapped
