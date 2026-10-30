"""Build a reviewed offline picture+PCM story book for ESP32-S3 SD FAT.

Binary KPB1 layout is fixed and documented in docs/P4_OFFLINE_STORY_PACK.md.
It is intentionally NOT executable firmware, and cannot bypass adult review.
Usage:
  python -m tools.story_pack render --title ... --text ... --font ... --output page.png
  python -m tools.story_pack pack --source book.json --output story.kpb --reviewed

Inputs: 800x480 PNG, 16kHz mono 16-bit little-endian uncompressed WAV.
Output: 1-bit white=1 800x480 page plus PCM16 stream (no RIFF) per page.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import tempfile
import wave
import zlib

WIDTH, HEIGHT = 800, 480
FRAME_BYTES = WIDTH * HEIGHT // 8  # 48 KiB
MAX_PAGES = 32
MAX_AUDIO_SECONDS = 60
MAX_PACKAGE_BYTES = 64 * 1024 * 1024
HEADER = struct.Struct("<4sHHHHIIIII")  # exactly 32 B
RECORD = struct.Struct("<HHIIIIIIIHHIII16s")  # exactly 64 B
BOOK_RE = re.compile(r"^[a-z0-9][a-z0-9_-]{2,63}$")
MARKER = b"KPB1"


def _read_json(path: Path) -> dict:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise ValueError("Book source must be a JSON object")
    return data


def _approved_text(value: object, max_chars: int) -> str:
    if not isinstance(value, str) or not value.strip() or len(value) > max_chars:
        raise ValueError("Reviewed text is missing or too long")
    if any(ord(ch) < 32 for ch in value) or "\u202e" in value:
        raise ValueError("Controls and bidirectional override forbidden in reviewed text")
    return value.strip()


def _asset(root: Path, path: object, expected: str) -> Path:
    if not isinstance(path, str) or not path or "\\" in path:
        raise ValueError(f"Invalid {expected} path")
    relative = Path(path)
    if relative.is_absolute() or any(part in (".", "..") for part in relative.parts):
        raise ValueError(f"{expected} must be a relative in-project path")
    full = (root / relative).resolve()
    if not full.is_relative_to(root.resolve()) or not full.is_file():
        raise ValueError(f"{expected} asset missing or outside book directory")
    if full.suffix.lower() != expected:
        raise ValueError(f"{expected} extension is required")
    return full


def _bitmap(path: Path) -> bytes:
    from PIL import Image

    with Image.open(path) as image:
        image.load()
        if image.size != (WIDTH, HEIGHT):
            raise ValueError("Book page must be exactly 800 x 480")
        # Threshold rather than implicit error diffusion: deterministic EPD.
        raw = image.convert("L").point(lambda x: 255 if x >= 170 else 0).convert("1")
        data = raw.tobytes()
    if len(data) != FRAME_BYTES:
        raise ValueError("Unexpected monochrome framebuffer stride")
    return data


def _pcm(path: Path) -> bytes:
    with wave.open(str(path), "rb") as wf:
        if wf.getcomptype() != "NONE" or wf.getnchannels() != 1 or \
                wf.getsampwidth() != 2 or wf.getframerate() != 16000:
            raise ValueError("Audio must be uncompressed 16 kHz mono PCM16 WAV")
        frames = wf.getnframes()
        if not 0 < frames <= MAX_AUDIO_SECONDS * 16000:
            raise ValueError("WAV length must be >0 and <=60 seconds")
        data = wf.readframes(frames)
        if len(data) != frames * 2:
            raise ValueError("Truncated WAV")
        return data


def _crc(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def make_pack(source: Path, output: Path, *, reviewed: bool) -> dict:
    """Trusted offline operation. Never publishes without explicit review."""
    if not reviewed:
        raise ValueError("Explicit --reviewed required; no implicit publishing")
    source = source.resolve()
    output = output.resolve()
    doc = _read_json(source)
    if set(doc) != {"schema_version", "book_id", "title", "approved", "pages"} or \
            doc["schema_version"] != 1 or doc["approved"] is not True:
        raise ValueError("Book manifest must be approved schema v1")
    book_id = doc["book_id"]
    if not isinstance(book_id, str) or not BOOK_RE.fullmatch(book_id):
        raise ValueError("Invalid book ID")
    title = _approved_text(doc["title"], 80)
    pages = doc["pages"]
    if not isinstance(pages, list) or not (1 <= len(pages) <= MAX_PAGES):
        raise ValueError("Package has invalid page count")
    root = source.parent
    records: list[bytes] = []
    payload = bytearray()
    data_start = HEADER.size + len(pages) * RECORD.size
    detail = []
    for i, item in enumerate(pages, 1):
        if not isinstance(item, dict) or set(item) != \
                {"number", "text", "approved", "image", "audio"} or \
                item["number"] != i or type(item["number"]) is not int or \
                item["approved"] is not True:
            raise ValueError("Every numbered page requires explicit review")
        text = _approved_text(item["text"], 320)
        img_path = _asset(root, item["image"], ".png")
        wav_path = _asset(root, item["audio"], ".wav")
        image = _bitmap(img_path)
        audio = _pcm(wav_path)
        utf8 = text.encode("utf-8")
        offsets = []
        for b in (image, audio, utf8):
            while len(payload) % 4:
                payload.append(0)
            offset = data_start + len(payload)
            payload.extend(b)
            offsets.append(offset)
        if data_start + len(payload) > MAX_PACKAGE_BYTES:
            raise ValueError("Package too large for supported SD resources")
        entry = RECORD.pack(
            i, 0,
            offsets[0], len(image), _crc(image),
            offsets[1], len(audio), _crc(audio),
            16000, 1, 16,
            offsets[2], len(utf8), _crc(utf8),
            b"\0" * 16
        )
        records.append(entry)
        detail.append({"page": i, "text": text, "image_sha256": hashlib.sha256(image).hexdigest(),
                       "pcm_sha256": hashlib.sha256(audio).hexdigest(),
                       "duration_ms": len(audio) * 1000 // 32000})
    toc = b"".join(records)
    total = data_start + len(payload)
    # CRC protects the header from accidental SD corruption (not authenticity).
    header0 = HEADER.pack(MARKER, 1, WIDTH, HEIGHT, len(pages),
                          HEADER.size, len(toc), data_start, total, 0)
    header = header0[:28] + struct.pack("<I", _crc(header0[:28]))
    result = header + toc + payload
    if len(result) != total:
        raise AssertionError("Internal layout mismatch")

    # Must never overwrite the source manifest / imported images by accident.
    if output == source or output.is_relative_to(root) and output in (
            _asset(root, p["image"], ".png") for p in pages):
        raise ValueError("Output would overwrite a source file")
    output.parent.mkdir(parents=True, exist_ok=True)
    sidecar = output.with_suffix(".manifest.json")
    if sidecar == source:
        raise ValueError("Manifest sidecar would overwrite source")

    metadata = {"schema_version": 1, "format": "KPB1",
                "book_id": book_id, "title": title, "approved": True,
                "width": WIDTH, "height": HEIGHT,
                "pages": detail, "package_bytes": len(result),
                "sha256": hashlib.sha256(result).hexdigest(),
                "integrity": "CRC32 per payload; no cryptographic authentication"}
    # Atomic file replacement to avoid leaving a partially written release.
    # Build into a temporary sibling in the same directory.
    def atomic_write(target: Path, content: bytes) -> None:
        fd, tmp = tempfile.mkstemp(prefix=".story-", dir=str(target.parent))
        try:
            with os.fdopen(fd, "wb") as writer:
                writer.write(content)
                writer.flush()
                os.fsync(writer.fileno())
            os.replace(tmp, target)
        finally:
            if os.path.exists(tmp):
                os.unlink(tmp)
    atomic_write(output, result)
    atomic_write(sidecar, (json.dumps(metadata, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    return metadata


def render_page(output: Path, font: Path, title: str, text: str, page: int) -> None:
    from PIL import Image, ImageDraw, ImageFont

    if not 1 <= page <= 1000:
        raise ValueError("Page must be in 1..1000")
    _approved_text(title, 80)
    _approved_text(text, 320)
    if not font.is_file():
        raise ValueError("Provide an existing licensed CJK TTF/OTF font")
    image = Image.new("RGB", (WIDTH, HEIGHT), "white")
    draw = ImageDraw.Draw(image)
    title_font = ImageFont.truetype(str(font), 38)
    body_font = ImageFont.truetype(str(font), 38)
    draw.rounded_rectangle((30, 26, 770, 452), radius=24, outline="black", width=5)
    draw.text((64, 55), title, fill="black", font=title_font)
    draw.line((64, 123, 736, 123), fill="black", width=3)
    # Wrap by rendered width rather than code point count, important for CJK.
    lines = []
    current = ""
    for ch in text:
        trial = current + ch
        if draw.textlength(trial, font=body_font) > 650:
            if current:
                lines.append(current)
            current = ch
        else:
            current = trial
    if current:
        lines.append(current)
    if len(lines) > 6:
        raise ValueError("Story text too long for legible 800x480 page; split it")
    for i, line in enumerate(lines):
        draw.text((66, 158 + i * 50), line, fill="black", font=body_font)
    draw.text((650, 400), f"{page}", fill="black", font=title_font)
    output.parent.mkdir(parents=True, exist_ok=True)
    image.save(output, format="PNG")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="cmd", required=True)
    pack = sub.add_parser("pack")
    pack.add_argument("--source", type=Path, required=True)
    pack.add_argument("--output", type=Path, required=True)
    pack.add_argument("--reviewed", action="store_true",
                      help="Trusted adult has reviewed ALL text, images, and WAV audio")
    render = sub.add_parser("render")
    render.add_argument("--font", type=Path, required=True)
    render.add_argument("--title", required=True)
    render.add_argument("--text", required=True)
    render.add_argument("--page", type=int, required=True)
    render.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.cmd == "render":
        render_page(args.output, args.font, args.title, args.text, args.page)
        print("PAGE_IMAGE_READY_FOR_MANUAL_REVIEW")
    else:
        data = make_pack(args.source, args.output, reviewed=args.reviewed)
        print("REVIEWED_STORY_PACK_CREATED", data["sha256"], data["package_bytes"])


if __name__ == "__main__":
    main()
