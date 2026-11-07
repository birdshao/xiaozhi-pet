"""Stage imported picture books for real KPB2 packaging, then require review.

V1 accepts externally provided images and 24 kHz mono PCM16 WAVs. It does NOT
synthesize speech, retrieve network images, approve AI content or alter firmware.
No .kpb is produced without a separate, explicit, hash-bound human review file.
"""
from __future__ import annotations

import copy
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import wave
from typing import Any

from content_pipeline.audio_qc import analyze_wav
from content_pipeline.schema import (ContentError, adaptation_terms, canonical_json,
                                     content_digest, safe_relpath, validate_document,
                                     write_json)
from tools import kpb2

WIDTH, HEIGHT = 800, 480
SAMPLE_RATE = 24000
MAX_IMAGE_INPUT = 20 * 1024 * 1024
MAX_WAV_INPUT = SAMPLE_RATE * 2 * 60 + 1024
# Narration provenance that needs a voice_terms_cleared attestation: online TTS
# or no audio.manifest.json at all (unknown origin fails closed).
VOICE_PROVENANCE_OPEN = frozenset({None, "edge-tts"})
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
            # Transparent pixels are white, as in tools/kpb2.py luminance();
            # a plain RGB convert would turn them black on the e-paper.
            rgba = source.convert("RGBA")
            backdrop = Image.new("RGBA", rgba.size, (255, 255, 255, 255))
            backdrop.alpha_composite(rgba)
            contained = ImageOps.contain(
                backdrop.convert("RGB"), (WIDTH, HEIGHT), method=Image.Resampling.LANCZOS)
            canvas = Image.new("RGB", (WIDTH, HEIGHT), "white")
            canvas.paste(contained, ((WIDTH - contained.width) // 2,
                                    (HEIGHT - contained.height) // 2))
            canvas.save(target, format="PNG")
    except (OSError, ValueError, Image.DecompressionBombError) as exc:
        raise ContentError(f"cannot render {image_path.name}: {exc}") from exc


def _write_epaper_previews(stage_dir: Path, image_name: str, page: int) -> None:
    """Use the *same* KPB2 quantization/dither as the shipped reader pack."""
    from PIL import Image
    mono, gray = kpb2.page_frames(stage_dir / image_name, "grey")
    if gray is None:
        raise ContentError("EPD preview: expected 4-level gray page")
    pdir = stage_dir / "preview"
    pdir.mkdir(exist_ok=True)
    mono_pixels = kpb2.unpack_bits(mono)
    gray_pixels = kpb2.unpack_grey(gray)
    Image.frombytes("L", (WIDTH, HEIGHT),
                    bytes(255 if v else 0 for v in mono_pixels)).save(
                        pdir / f"page_{page:02d}_mono.png")
    Image.frombytes("L", (WIDTH, HEIGHT),
                    bytes(v * 85 for v in gray_pixels)).save(
                        pdir / f"page_{page:02d}_grey.png")


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


def _license_name(license_id: str) -> str:
    if not license_id.startswith("CC-BY"):
        return license_id
    name, version = license_id.rsplit("-", 1)
    return f"{name.replace('CC-BY', 'CC BY', 1)} {version}"


def credits_record(doc: dict) -> dict:
    """Attribution carried into the shipped manifest (CC BY 4.0 s.3(a)(1))."""
    source = doc["source"]
    if source["license_id"] == "unknown":  # staging only; building refuses it
        terms = {"license_id": "unknown", "license_url": None, "share_alike": False}
    else:
        terms = adaptation_terms(source["license_id"])
    creators = source["creators"]
    modified = [{"translation": "translated"}.get(t["kind"], t["kind"])
                for t in doc["transformations"]] + ["resized", "narrated"]
    return {
        "title": doc["title"], "locale": doc["locale"],
        "creators": {k: list(creators.get(k, [])) for k in
                     ("text", "illustration", "design", "translation")},
        "license_id": terms["license_id"], "license_url": terms["license_url"],
        "share_alike": terms["share_alike"],
        "source_provider": source["provider"],
        "source_url": source["source_url"], "source_ref": source["source_ref"],
        "modified": modified,
    }


def _short_url(record: dict) -> str:
    """Source URL without scheme; a full pinned git SHA is shortened (GitHub resolves it)."""
    url = record["source_url"].removeprefix("https://")
    short = record["source_ref"][:7]
    url = url.replace(record["source_ref"], short)
    return url if short in url else f"{url} ({short})"


def credits_text(record: dict) -> str:
    """Single-line text for the generated final credits page (<=320 chars)."""
    return " ".join(credits_parts(record))


def credits_parts(record: dict) -> list[str]:
    """Credit fields; the page image draws each on its own line(s)."""
    zh = record["locale"].lower().startswith("zh")
    names = record["creators"]
    labels = ({"text": "文", "illustration": "图", "design": "设计", "translation": "译"}
              if zh else {"text": "Text", "illustration": "Illustrations",
                          "design": "Design", "translation": "Translation"})
    parts = [f"《{record['title']}》" if zh else record["title"]]
    for key in ("text", "illustration", "design", "translation"):
        if names[key]:
            joined = ("、" if zh else ", ").join(names[key])
            parts.append(f"{labels[key]}：{joined}" if zh else f"{labels[key]}: {joined}")
    licence = _license_name(record["license_id"])
    if record["share_alike"]:
        licence += "（相同方式共享）" if zh else " (ShareAlike)"
    if record["license_url"]:
        licence += " " + record["license_url"].removeprefix("https://").rstrip("/")
    modified_zh = {"translated": "翻译", "resized": "缩放", "narrated": "配音"}
    if zh:
        parts.append(f"许可：{licence}")
        parts.append(f"来源：{_short_url(record)}")
        parts.append("已修改：" + "、".join(modified_zh[m] for m in record["modified"]))
    else:
        parts.append(f"Licence: {licence}")
        parts.append(f"Source: {_short_url(record)}")
        parts.append("Modified: " + ", ".join(record["modified"]))
    return parts


CJK_FONTS = (
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
    "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
    "C:/Windows/Fonts/msyh.ttc", "C:/Windows/Fonts/simhei.ttf",
    "/System/Library/Fonts/PingFang.ttc", "/System/Library/Fonts/STHeiti Light.ttc",
)


def _credits_font(zh: bool, size: int):
    from PIL import ImageFont
    if not zh:  # bundled Latin font keeps the English layout deterministic
        try:
            return ImageFont.load_default(size=size)
        except TypeError:  # Pillow < 10.1
            return ImageFont.load_default()
    candidates = [os.environ["LCP_CJK_FONT"]] if os.environ.get("LCP_CJK_FONT") else []
    for path in candidates + list(CJK_FONTS):
        if Path(path).is_file():
            try:
                return ImageFont.truetype(path, size)
            except OSError:
                continue
    raise ContentError("stage: the credits page needs a CJK font; install Noto Sans CJK "
                       "or set LCP_CJK_FONT to a .ttf/.ttc/.otf file")


def _wrap(draw, text: str, font, width: int, zh: bool) -> list[str]:
    tokens = list(text) if zh else [w + " " for w in text.split(" ")]
    lines, current = [], ""
    for token in tokens:
        if draw.textlength((current + token).rstrip(), font=font) <= width:
            current += token
            continue
        if current:
            lines.append(current.rstrip())
            current = ""
        for char in token:  # one over-long word, e.g. a URL: break by character
            if current and draw.textlength(current + char, font=font) > width:
                lines.append(current)
                current = ""
            current += char
    if current.strip():
        lines.append(current.rstrip())
    return lines


def _credits_page_files(stage_dir: Path, number: int, parts: list[str], zh: bool) -> tuple[str, str]:
    """Framed page with the credits drawn in (the reader never draws page text)
    and a 0.4 s silent WAV."""
    from PIL import Image, ImageDraw
    canvas = Image.new("RGB", (WIDTH, HEIGHT), "white")
    draw = ImageDraw.Draw(canvas)
    draw.rectangle((16, 16, WIDTH - 17, HEIGHT - 17), outline="black", width=4)
    font = _credits_font(zh, 26 if zh else 24)
    line_height = 34 if zh else 30
    gap = 8
    lines: list[str | None] = []
    for part in parts:
        lines += _wrap(draw, part, font, WIDTH - 2 * 44, zh) + [None]  # None = field gap
    used = sum(gap if line is None else line_height for line in lines[:-1])
    if 32 + used > HEIGHT - 40:
        raise ContentError("stage: credits text does not fit on one 800x480 page")
    y = 32
    for line in lines[:-1]:
        if line is None:
            y += gap
            continue
        draw.text((44, y), line, fill="black", font=font)
        y += line_height
    image, audio = f"page_{number:02d}.png", f"page_{number:02d}.wav"
    canvas.save(stage_dir / image, format="PNG")
    with wave.open(str(stage_dir / audio), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(SAMPLE_RATE)
        out.writeframes(b"\0\0" * (SAMPLE_RATE * 2 // 5))
    return image, audio


def _kpb2_preflight(doc: dict, book_id: str, record: dict) -> None:
    """Run the exact checks tools/kpb2.py applies, so failures precede review."""
    try:
        if not kpb2.BOOK_RE.fullmatch(book_id):
            raise ValueError("Invalid book ID")
        kpb2.reviewed_text(doc["title"], 80, "title")
        for page in doc["pages"]:
            kpb2.reviewed_text(page["text"], 320, f"page {page['number']} text")
        kpb2.reviewed_text(credits_text(record), 320, "credits page text")
        if len(doc["pages"]) + 1 > kpb2.MAX_PAGES:
            raise ValueError("too many pages to add the credits page")
    except ValueError as exc:
        raise ContentError(f"stage: would fail KPB2 build: {exc}") from exc


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
    previews = {}
    for page in book["pages"]:
        for mode in ("mono", "grey"):
            rel = f"preview/page_{page['number']:02d}_{mode}.png"
            preview = _asset(stage, rel, max_size=MAX_IMAGE_INPUT)
            previews[rel] = _sha_file(preview, MAX_IMAGE_INPUT)
    return {
        "schema_version": 1,
        "lcir_digest": content_digest(lcir),
        "book_json_sha256": _hash(book_file.read_bytes()),
        "lcir_json_sha256": _hash(lcir_file.read_bytes()),
        "attribution_sha256": _hash(attribution_file.read_bytes()),
        "stage_files": files,
        "preview_files": previews,
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
    if "-ND" in doc["source"]["license_id"]:
        raise ContentError("stage: NoDerivatives licence forbids narrated/translated adaptation")
    credits = credits_record(doc)
    _kpb2_preflight(doc, book_id, credits)
    # Do not accidentally overwrite source assets if the stage path is an
    # ancestor of the asset directory or the asset directory itself.
    base = assets_file.parent
    output_root, asset_root = stage_dir.resolve(), base.resolve(strict=True)
    if asset_root == output_root or asset_root.is_relative_to(output_root):
        raise ContentError("stage directory must not contain the source asset directory")
    audio_provenance = None
    original_audio_manifest = base / "audio.manifest.json"
    if original_audio_manifest.exists():
        audio_provenance = json.loads(original_audio_manifest.read_text(encoding="utf-8"))
        if audio_provenance.get("schema_version") != "BOOKAUDIO1" or \
                audio_provenance.get("status") not in ("draft", "test_only") or \
                audio_provenance.get("item_uid") != doc["item_uid"] or \
                audio_provenance.get("content_digest") != content_digest(doc):
            raise ContentError("book: audio manifest is stale or invalid")
    stage_dir.mkdir(parents=True, exist_ok=True)
    pages = []
    provenance_assets = []
    for page in doc["pages"]:
        n = page["number"]
        assets = mapping[str(n)]
        img = _asset(base, assets["image"], max_size=MAX_IMAGE_INPUT)
        wav = _asset(base, assets["audio"], max_size=MAX_WAV_INPUT)
        _verify_wav(wav)
        quality = analyze_wav(wav, max_seconds=60)  # hard-fails near-silent narration
        if img.is_relative_to(output_root) or wav.is_relative_to(output_root):
            raise ContentError("source assets must not be inside stage directory")
        provenance_assets.append({
            "page": n, "image_ref": page["image_ref"],
            "image_source": assets["image"], "image_sha256": _sha_file(img, MAX_IMAGE_INPUT),
            "audio_source": assets["audio"], "audio_sha256": _sha_file(wav, MAX_WAV_INPUT),
            "audio_quality": quality,
        })
        image_target = f"page_{n:02d}.png"
        audio_target = f"page_{n:02d}.wav"
        _render(img, stage_dir / image_target)
        _write_epaper_previews(stage_dir, image_target, n)
        shutil.copyfile(wav, stage_dir / audio_target)
        pages.append({
            "number": n, "text": page["text"], "approved": False,
            "image": image_target, "image_mode": "grey", "audio": audio_target,
            "questions": [],
        })
    credits_number = len(pages) + 1
    credits_image, credits_audio = _credits_page_files(
        stage_dir, credits_number, credits_parts(credits),
        credits["locale"].lower().startswith("zh"))
    _write_epaper_previews(stage_dir, credits_image, credits_number)
    pages.append({
        "number": credits_number, "text": credits_text(credits), "approved": False,
        "image": credits_image, "image_mode": "grey", "audio": credits_audio,
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
        "audio_test_only": audio_provenance is not None and
                           audio_provenance["status"] == "test_only",
        "audio_provider": audio_provenance.get("provider") if audio_provenance else None,
        "credits": credits,
        "notice": "Check source/license/illustration/audio before distributing.",
    })
    manifest = _core_manifest(stage_dir)
    manifest["checked_hash"] = _hash(canonical_json(manifest))
    write_json(stage_dir / "candidate.json", manifest)
    return manifest


def book_pages_end_with_credits(stage_dir: Path, record: dict) -> bool:
    book = json.loads((stage_dir / "book.json").read_text(encoding="utf-8"))
    return bool(book["pages"]) and book["pages"][-1]["text"] == credits_text(record)


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
    # voice_terms_cleared is optional, but mandatory for edge-tts narration.
    allowed = required | {"voice_terms_cleared"}
    if not isinstance(attest, dict) or not required <= set(attest) <= allowed or any(
            attest[k] is not True for k in attest):
        raise ContentError("review: all five human checks required")
    doc = json.loads((stage_dir / "lcir.json").read_text(encoding="utf-8"))
    if doc["source"]["license_id"] == "unknown":
        raise ContentError("review: exact source license required")
    adaptation_terms(doc["source"]["license_id"])  # refuses ND (derivative) licences
    attribution = json.loads((stage_dir / "attribution.json").read_text(encoding="utf-8"))
    if attribution.get("audio_test_only") is True:
        raise ContentError("review: synthetic test-only narration cannot be published")
    if attribution.get("audio_provider") in VOICE_PROVENANCE_OPEN and \
            attest.get("voice_terms_cleared") is not True:
        raise ContentError(
            "review: narration provider is edge-tts or unrecorded (no audio.manifest.json); "
            "attestations.voice_terms_cleared is required")
    credits = credits_record(doc)
    if attribution.get("credits") != credits or \
            not book_pages_end_with_credits(stage_dir, credits):
        raise ContentError("review: credits record or credits page does not match source")
    if not doc["source"]["creators"]["text"] or not doc["source"]["creators"]["illustration"]:
        raise ContentError("review: verified text and illustration credits required")
    # No approved manifest is saved to the stage. The trusted build is temporary
    # and calls the existing story_pack implementation, not a second encoder.
    from tools.story_pack import make_pack
    book = copy.deepcopy(json.loads((stage_dir / "book.json").read_text(encoding="utf-8")))
    book["reviewer"] = review["reviewer"].strip()
    book["credits"] = credits
    book["checked_hash"] = current["checked_hash"]
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
