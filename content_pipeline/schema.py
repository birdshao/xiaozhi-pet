"""Strict LCIR1 draft validation, provenance and stable content ID registry.

This module uses only the Python standard library (Python 3.10+). It does not
download assets, approve child-facing material, or publish device packages.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import tempfile
from typing import Any
from urllib.parse import urlsplit


class ContentError(ValueError):
    """Invalid imported content or conflicting content identity."""


UID = re.compile(r"^[a-z0-9][a-z0-9._:-]{2,95}$")
SKILL = re.compile(r"^[a-z][a-z0-9_.-]{2,95}$")
REF = re.compile(r"^[a-fA-F0-9]{40}$")
HASH = re.compile(r"^[a-f0-9]{64}$")
LOCALE = re.compile(r"^[a-z]{2,3}(?:-[A-Za-z0-9]{2,8})*$")
LICENSE = re.compile(
    r"^(?:unknown|CC0-1\.0|PUBLIC-DOMAIN|CC-BY(?:-NC|-ND|-SA|-NC-ND|-NC-SA)?-[34]\.0)$"
)


def _keys(obj: Any, expected: set[str], label: str) -> dict:
    if not isinstance(obj, dict) or set(obj) != expected:
        raise ContentError(f"{label}: missing or unknown keys")
    return obj


def _string(value: Any, label: str, maximum: int = 4000) -> str:
    if not isinstance(value, str) or not value.strip() or len(value) > maximum:
        raise ContentError(f"{label}: invalid text")
    if "\x00" in value or "\r" in value:
        raise ContentError(f"{label}: invalid control character")
    return value


def safe_relpath(path: Any, label: str) -> str:
    """Validate a portable path. Callers must still check real paths/symlinks."""
    value = _string(path, label, 240)
    if "\\" in value or "%" in value or value.startswith("/") or ":" in value:
        raise ContentError(f"{label}: unsafe relative path")
    parts = PurePosixPath(value).parts
    if not parts or any(part in ("", ".", "..") for part in value.split("/")):
        raise ContentError(f"{label}: unsafe relative path")
    if parts != tuple(value.split("/")):
        raise ContentError(f"{label}: path not canonical")
    return value


def canonical_json(value: Any) -> bytes:
    return json.dumps(value, ensure_ascii=False, sort_keys=True,
                      separators=(",", ":"), allow_nan=False).encode("utf-8")


def content_digest(doc: dict) -> str:
    """Review binding excludes only review state, not text, license or source."""
    return hashlib.sha256(canonical_json({k: v for k, v in doc.items()
                                         if k != "review"})).hexdigest()


def _url(value: Any, label: str) -> str:
    s = _string(value, label, 2048)
    parsed = urlsplit(s)
    if parsed.scheme != "https" or not parsed.netloc or parsed.username or parsed.password:
        raise ContentError(f"{label}: HTTPS public URL required")
    return s


def validate_document(doc: Any) -> dict:
    d = _keys(doc, {"schema_version", "item_uid", "kind", "title", "locale",
                    "age_band", "content_id", "revision", "skills", "source",
                    "transformations", "pages", "review"}, "LCIR1")
    if d["schema_version"] != "LCIR1" or d["kind"] != "picture_book":
        raise ContentError("LCIR1: only picture_book drafts are supported in LCP-01/02")
    if not isinstance(d["item_uid"], str) or not UID.fullmatch(d["item_uid"]):
        raise ContentError("item_uid: invalid")
    _string(d["title"], "title", 180)
    if not isinstance(d["locale"], str) or not LOCALE.fullmatch(d["locale"]):
        raise ContentError("locale: invalid BCP47 subset")
    if d["age_band"] != "3-5":
        raise ContentError("age_band: expected 3-5")
    if type(d["content_id"]) is not int or not 1000 <= d["content_id"] <= 19999:
        raise ContentError("content_id: reserved book range 1000..19999")
    if type(d["revision"]) is not int or not 1 <= d["revision"] <= 65535:
        raise ContentError("revision: invalid")
    if not isinstance(d["skills"], list) or len(d["skills"]) > 30 or len(set(
            str(x) for x in d["skills"])) != len(d["skills"]):
        raise ContentError("skills: invalid list")
    for skill in d["skills"]:
        if not isinstance(skill, str) or not SKILL.fullmatch(skill):
            raise ContentError("skill: invalid")

    s = _keys(d["source"], {"provider", "source_item_id", "source_url",
                             "source_ref", "license_id", "license_hint",
                             "creators", "source_files", "attribution_required"},
              "source")
    if s["provider"] not in ("global-asp", "bookdash"):
        raise ContentError("source.provider: unsupported")
    safe_relpath(s["source_item_id"], "source_item_id")
    _url(s["source_url"], "source_url")
    if not isinstance(s["source_ref"], str) or not REF.fullmatch(s["source_ref"]):
        raise ContentError("source_ref: expected pinned 40-digit git SHA")
    if not isinstance(s["license_id"], str) or not LICENSE.fullmatch(s["license_id"]):
        raise ContentError("license_id: unknown or exact approved SPDX-like ID")
    if s["license_hint"] is not None:
        _string(s["license_hint"], "license_hint", 96)
    if type(s["attribution_required"]) is not bool:
        raise ContentError("attribution_required: must be bool")
    creators = _keys(s["creators"], {"text", "illustration", "translation"}, "creators")
    for key in creators:
        vals = creators[key]
        if not isinstance(vals, list) or len(vals) > 20:
            raise ContentError(f"creators.{key}: invalid")
        for name in vals:
            _string(name, f"creators.{key}", 120)
    files = s["source_files"]
    if not isinstance(files, list) or not files:
        raise ContentError("source_files: missing")
    for f in files:
        _keys(f, {"path", "sha256"}, "source_file")
        safe_relpath(f["path"], "source_file.path")
        if not isinstance(f["sha256"], str) or not HASH.fullmatch(f["sha256"]):
            raise ContentError("source_file.sha256: invalid")

    transformations = d["transformations"]
    if not isinstance(transformations, list) or len(transformations) > 1:
        raise ContentError("transformations: only one explicit translation step is supported")
    for change in transformations:
        _keys(change, {"kind", "source_locale", "target_locale",
                       "source_digest", "method", "notes"}, "translation")
        if change["kind"] != "translation" or change["source_locale"] != "en" or \
                change["target_locale"] != "zh-Hans" or d["locale"] != "zh-Hans" or \
                s["provider"] != "bookdash" or change["method"] != "manual_draft" or \
                not isinstance(change["source_digest"], str) or \
                not HASH.fullmatch(change["source_digest"]):
            raise ContentError("translation: unsupported or unpinned transformation")
        _string(change["notes"], "translation.notes", 300)
    pages = d["pages"]
    if not isinstance(pages, list) or not 1 <= len(pages) <= 32:
        raise ContentError("pages: expected 1..32 (KPB2 bound)")
    for i, p in enumerate(pages, 1):
        _keys(p, {"number", "text", "image_ref", "image", "audio",
                  "image_mode", "skills", "questions"}, f"page {i}")
        if type(p["number"]) is not int or p["number"] != i:
            raise ContentError(f"page {i}: non-sequential number")
        if p["text"] == "":
            # The original Book Dash archive contains genuinely wordless
            # illustrated pages. Keep those pages silent rather than
            # inventing words and attributing them to the book's author.
            if not (s["provider"] == "bookdash" and
                    d["locale"] == "en" and not d["transformations"] and
                    p["image_ref"] is not None):
                raise ContentError(f"page {i}: empty text only allowed in original Book Dash")
        else:
            _string(p["text"], f"page {i} text", 3200)
        if p["image_ref"] is not None:
            safe_relpath(p["image_ref"], f"page {i} image_ref")
        for key in ("image", "audio"):
            if p[key] is not None:
                safe_relpath(p[key], f"page {i} {key}")
        if p["image_mode"] != "grey" or p["questions"] != []:
            raise ContentError(f"page {i}: imported draft cannot fabricate questions")
        if p["skills"] != d["skills"]:
            raise ContentError(f"page {i}: unknown skill set")

    r = _keys(d["review"], {"status", "reviewer", "checked_hash",
                            "checked_at", "notes"}, "review")
    if r["status"] not in ("draft", "review_pending", "reviewed", "rejected"):
        raise ContentError("review.status: invalid")
    if r["notes"] is not None:
        _string(r["notes"], "review.notes", 1000)
    if r["status"] == "reviewed":
        _string(r["reviewer"], "review.reviewer", 100)
        _string(r["checked_at"], "review.checked_at", 60)
        if r["checked_hash"] != content_digest(d):
            raise ContentError("review: stale or missing checked_hash")
        if s["license_id"] == "unknown" or any(
                p["image"] is None or p["audio"] is None for p in pages):
            raise ContentError("review: assets and exact source license required")
    elif any(r[k] is not None for k in ("reviewer", "checked_hash", "checked_at")):
        raise ContentError("review: unapproved draft must have no approval metadata")
    return d


def write_json(path: Path, doc: dict) -> None:
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    content = json.dumps(doc, ensure_ascii=False, sort_keys=True, indent=2) + "\n"
    with tempfile.NamedTemporaryFile("w", encoding="utf-8", dir=path.parent,
                                     prefix=".lcir-", delete=False) as output:
        tmp = Path(output.name)
        try:
            output.write(content)
            output.flush()
            os.fsync(output.fileno())
        except BaseException:
            tmp.unlink(missing_ok=True)
            raise
    os.replace(tmp, path)


def register_document(registry_path: Path, doc: dict) -> None:
    """Single-writer V1 registry; protects IDs and revisions, not concurrency."""
    validate_document(doc)
    path = Path(registry_path)
    if path.exists():
        state = json.loads(path.read_text(encoding="utf-8"))
    else:
        state = {"schema_version": 1, "items": {}}
    _keys(state, {"schema_version", "items"}, "id_registry")
    if state["schema_version"] != 1 or not isinstance(state["items"], dict):
        raise ContentError("id_registry: unsupported")
    uid = doc["item_uid"]
    digest = content_digest(doc)
    identity = state["items"].get(uid)
    for other_uid, other in state["items"].items():
        if other_uid != uid and other.get("content_id") == doc["content_id"]:
            raise ContentError(f"content_id {doc['content_id']} already claimed")
    if identity:
        if identity["content_id"] != doc["content_id"]:
            raise ContentError("item_uid already has a different content_id")
        previous = identity["revisions"].get(str(doc["revision"]))
        if previous and previous != digest:
            raise ContentError("revision changed without increment")
        identity["revisions"][str(doc["revision"])] = digest
    else:
        state["items"][uid] = {"content_id": doc["content_id"],
                               "revisions": {str(doc["revision"]): digest}}
    write_json(path, state)
