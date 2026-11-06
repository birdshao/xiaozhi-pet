"""Stage imported picture books for real KPB2 packaging, then require review.

V1 accepts externally provided images and 24 kHz mono PCM16 WAVs. It does NOT
synthesize speech, retrieve network images, approve AI content or alter firmware.
No .kpb is produced without a separate, explicit, hash-bound human review file.
"""
from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path
import re
import shutil
import wave
from typing import Any

from content_pipeline.schema import (ContentError, canonical_json, content_digest,
                                     safe_relpath, validate_document, write_json)

WIDTH, HEIGHT = 800, 480
SAMPLE_RATE = 24000
MAX_IMAGE_INPUT = 20 * 1024 * 1024
MAX_WAV_INPUT = SAMPLE_RATE * 2 * 60 + 1024
BOOK_ID = re.compile(r"^[a-z0-9][a-z0-9_-]{2,63}$")


def _hash(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _sha_file(path: Path, max_size: int) -> str:
    if path.stat().st_size > max_size:
        raise ContentError(f"asset too large: {path.name}")
    digest = hashlib.sha256()
    with path.open("rb") as data:
        while chunk := data.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def _asset(root: Path, rel: str, *, max_size: int) -> Path:
    safe_relpath(rel, "asset")
    base = root.resolve(strict=True)
    full = (root / rel).resolve(strict=True)
    if not full.is_relative_to(base) or not full.is_file():
        raise ContentError("asset path escapes allowed directory")
    _sha_file(full, max_size)
    return full


def _verify_wav(path: Path) -> None:
    try:
        with wave.open(str(path), "rb") as audio:
            if (audio.getframerate() != SAMPLE_RATE or
                    audio.getnchannels() != 1 or audio.getsampwidth() != 2 or
                    audio.getcomptype() != "NONE" or
                    not 1 <= audio.getnframes() <= 60 * SAMPLE_RATE):
                raise ContentError("WAV must be 24 kHz mono PCM16, 0..60 seconds")
    except (wave.Error, EOFError) as exc:
        raise ContentError("invalid WAV") from exc


def _render(image_path: Path, target: Path) -> None:
    try:
        from PIL import Image, ImageOps
        Image.MAX_IMAGE_PIXELS = 20_000_000
        with Image.open(image_path) as source:
            if source.width * source.height > 20_000_000:
                raise ContentError("image too large")
            source.load()
            contained = ImageOps.contain(
                source.convert("RGB"), (WIDTH, HEIGHT), method=Image.Resampling.LANCZOS)
            canvas = Image.new("RGB", (WIDTH, HEIGHT), "white")
            canvas.paste(contained, ((WIDTH - contained.width) // 2,
                                    (HEIGHT - contained.height) // 2))
            canvas.save(target, format="PNG")
    except (OSError, ValueError, Image.DecompressionBombError) as exc:
        raise ContentError(f"cannot render {image_path.name}: {exc}") from exc


def _asset_map(mapping: dict, page_count: int) -> dict:
    if not isinstance(mapping, dict) or set(mapping) != {"schema_version", "pages"} or \
            mapping["schema_version"] != 1 or not isinstance(mapping["pages"], dict):
        raise ContentError("asset map: schema_version=1 and pages mapping required")
    expected = {str(i) for i in range(1, page_count + 1)}
    if set(mapping["pages"]) != expected:
        raise ContentError("asset map: one image/audio mapping required per story page")
    for number, assets in mapping["pages"].items():
        if not isinstance(assets, dict) or set(assets) != {"image", "audio"}:
            raise ContentError(f"asset map: page {number} needs image/audio")
        safe_relpath(assets["image"], "image")
        safe_relpath(assets["audio"], "audio")
    return mapping["pages"]


def _core_manifest(stage: Path) -> dict[str, Any]:
    book_file = stage / "book.json"
    lcir_file = stage / "lcir.json"
    attribution_file = stage / "attribution.json"
    book = json.loads(book_file.read_text(encoding="utf-8"))
    lcir = validate_document(json.loads(lcir_file.read_text(encoding="utf-8")))
    files = {}
    for page in book["pages"]:
        for key in ("image", "audio"):
            f = _asset(stage, page[key], max_size=MAX_IMAGE_INPUT if key == "image"
                       else MAX_WAV_INPUT)
            files[page[key]] = _sha_file(f, MAX_IMAGE_INPUT if key == "image"
                                         else MAX_WAV_INPUT)
    return {
        "schema_version": 1,
        "lcir_digest": content_digest(lcir),
        "book_json_sha256": _hash(book_file.read_bytes()),
        "lcir_json_sha256": _hash(lcir_file.read_bytes()),
        "attribution_sha256": _hash(attribution_file.read_bytes()),
        "stage_files": files,
    }


def stage_book(lcir_file: Path, assets_file: Path, stage_dir: Path) -> dict:
    """Prepare draft images/audio/manifest for review. Returns bound digest."""
    lcir_file, assets_file, stage_dir = map(Path, (lcir_file, assets_file, stage_dir))
    doc = validate_document(json.loads(lcir_file.read_text(encoding="utf-8")))
    mapping = _asset_map(json.loads(assets_file.read_text(encoding="utf-8")),
                         len(doc["pages"]))
    if doc["kind"] != "picture_book":
        raise ContentError("stage: only picture_book supported")
    book_id = re.sub(r"[^a-z0-9_-]+", "_", doc["item_uid"].replace(":", "_"))[:63]
    if not BOOK_ID.fullmatch(book_id):
        raise ContentError("stage: could not derive valid book_id")
    # Do not accidentally overwrite source assets if the stage path is an
    # ancestor of the asset directory or the asset directory itself.
    base = assets_file.parent
    output_root, asset_root = stage_dir.resolve(), base.resolve(strict=True)
    if asset_root == output_root or asset_root.is_relative_to(output_root):
        raise ContentError("stage directory must not contain the source asset directory")
    stage_dir.mkdir(parents=True, exist_ok=True)
    pages = []
    provenance_assets = []
    for page in doc["pages"]:
        n = page["number"]
        assets = mapping[str(n)]
        img = _asset(base, assets["image"], max_size=MAX_IMAGE_INPUT)
        wav = _asset(base, assets["audio"], max_size=MAX_WAV_INPUT)
        _verify_wav(wav)
        if img.is_relative_to(output_root) or wav.is_relative_to(output_root):
            raise ContentError("source assets must not be inside stage directory")
        provenance_assets.append({
            "page": n, "image_ref": page["image_ref"],
            "image_source": assets["image"], "image_sha256": _sha_file(img, MAX_IMAGE_INPUT),
            "audio_source": assets["audio"], "audio_sha256": _sha_file(wav, MAX_WAV_INPUT),
        })
        if len(page["text"]) > 320:
            raise ContentError(f"page {n}: >320 chars; manual editing/splitting required")
        image_target = f"page_{n:02d}.png"
        audio_target = f"page_{n:02d}.wav"
        _render(img, stage_dir / image_target)
        shutil.copyfile(wav, stage_dir / audio_target)
        pages.append({
            "number": n, "text": page["text"], "approved": False,
            "image": image_target, "image_mode": "grey", "audio": audio_target,
            "questions": [],
        })
    book = {
        "schema_version": 2, "package_type": "book",
        "book_id": book_id, "content_id": doc["content_id"],
        "revision": doc["revision"], "title": doc["title"],
        "reviewer": "pending", "approved": False, "pages": pages,
    }
    write_json(stage_dir / "lcir.json", doc)
    write_json(stage_dir / "book.json", book)
    write_json(stage_dir / "attribution.json", {
        "title": doc["title"], "locale": doc["locale"],
        "source": doc["source"], "transformations": doc["transformations"],
        "assets": provenance_assets,
        "notice": "Check source/license/illustration/audio before distributing.",
    })
    manifest = _core_manifest(stage_dir)
    manifest["checked_hash"] = _hash(canonical_json(manifest))
    write_json(stage_dir / "candidate.json", manifest)
    return manifest


def build_reviewed_book(stage_dir: Path, review_file: Path, output: Path) -> dict:
    """Build a KPB2 *only* after an adult attests the exact staged bytes."""
    stage_dir, review_file, output = map(Path, (stage_dir, review_file, output))
    candidate = json.loads((stage_dir / "candidate.json").read_text(encoding="utf-8"))
    current = _core_manifest(stage_dir)
    current["checked_hash"] = _hash(canonical_json(current))
    if current != candidate:
        raise ContentError("staged content changed after candidate was created")
    review = json.loads(review_file.read_text(encoding="utf-8"))
    expected = {"status", "reviewer", "checked_hash", "attestations"}
    if not isinstance(review, dict) or set(review) != expected:
        raise ContentError("review: expected status, reviewer, checked_hash and attestations")
    if review["status"] != "reviewed" or review["checked_hash"] != current["checked_hash"]:
        raise ContentError("review: not approved or hash mismatch")
    if not isinstance(review["reviewer"], str) or not 1 <= len(review["reviewer"].strip()) <= 40:
        raise ContentError("review: adult reviewer name required")
    attest = review["attestations"]
    required = {"source_license", "illustrations", "text", "audio", "age_safety"}
    if not isinstance(attest, dict) or set(attest) != required or any(
            attest[k] is not True for k in required):
        raise ContentError("review: all five human checks required")
    doc = json.loads((stage_dir / "lcir.json").read_text(encoding="utf-8"))
    if doc["source"]["license_id"] == "unknown":
        raise ContentError("review: exact source license required")
    if not doc["source"]["creators"]["text"] or not doc["source"]["creators"]["illustration"]:
        raise ContentError("review: verified text and illustration credits required")
    # No approved manifest is saved to the stage. The trusted build is temporary
    # and calls the existing story_pack implementation, not a second encoder.
    from tools.story_pack import make_pack
    book = copy.deepcopy(json.loads((stage_dir / "book.json").read_text(encoding="utf-8")))
    book["reviewer"] = review["reviewer"].strip()
    book["approved"] = True
    for page in book["pages"]:
        page["approved"] = True
    temporary = stage_dir / "__reviewed_kpb2_temp.json"
    if temporary.exists():
        raise ContentError("temporary build manifest already exists")
    try:
        write_json(temporary, book)
        metadata = make_pack(temporary, output, reviewed=True)
    finally:
        temporary.unlink(missing_ok=True)
    return {"checked_hash": current["checked_hash"],
            "package_sha256": metadata["sha256"],
            "package_bytes": metadata["package_bytes"],
            "output": str(output)}
