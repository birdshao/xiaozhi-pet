"""Create a visible/audible diagnostic SD book for Waveshare ePaper 3.97.

NON-CHILD CONTENT: two very different monochrome pictures and gentle synthetic
tones, not narrated content or a parent-approved book. Test use only.
The fixed internal approval bits are exclusively for KPB1 packaging tests.
Do not upload or distribute the fixture as a child story resource.

Run:
  python -m tools.make_story_sd_fixture --output build/story-smoke
Copy build/story-smoke/books/forest_friends.kpb to FAT32 /books/ on TF.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import tempfile
import wave

from PIL import Image, ImageDraw

from tools.story_pack import HEIGHT, WIDTH, make_pack


def _image(output: Path, page: int) -> None:
    image = Image.new("RGB", (WIDTH, HEIGHT), "white")
    draw = ImageDraw.Draw(image)
    draw.rectangle((30, 30, 770, 450), outline="black", width=9)
    if page == 1:
        # One unmistakable bold LEFT circle.
        draw.ellipse((95, 105, 385, 395), fill="black")
        draw.rectangle((455, 105, 705, 395), outline="black", width=6)
    else:
        # Two unmistakable bold RIGHT squares.
        draw.ellipse((95, 105, 385, 395), outline="black", width=6)
        draw.rectangle((455, 112, 565, 390), fill="black")
        draw.rectangle((590, 112, 700, 390), fill="black")
    draw.text((48, 52), f"SD / EPD / PCM TEST {page}", fill="black")
    image.save(output, "PNG")


def _wav(output: Path, page: int) -> None:
    # Gentle 20%-peak beep near 440/659 Hz for 0.50 seconds, with 30 ms fade.
    # Never used as music, TTS, or a child-facing learning resource.
    rate = 24000
    samples = rate // 2
    pitch = 440 if page == 1 else 659
    pcm = bytearray()
    for n in range(samples):
        ramp = min(1.0, n / 720, (samples - 1 - n) / 720)
        value = round(0.14 * max(0.0, ramp) * math.sin(2 * math.pi * pitch * n / rate) * 32767)
        pcm.extend(struct.pack("<h", value))
    with wave.open(str(output), "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(rate)
        wf.writeframes(pcm)


def build(output: Path) -> dict:
    output = output.resolve()
    target = output / "books" / "forest_friends.kpb"
    output.mkdir(parents=True, exist_ok=True)
    # Private transient source directory; no raw images/tonal recordings need
    # to be distributed alongside SD fixture.
    with tempfile.TemporaryDirectory(prefix="story_smoke_") as workspace:
        root = Path(workspace)
        pages = []
        for page in (1, 2):
            pic = f"{page}.png"
            wav = f"{page}.wav"
            _image(root / pic, page)
            _wav(root / wav, page)
            pages.append({
                "number": page,
                "text": f"HARDWARE SMOKE TEST ONLY - PAGE {page}",
                "approved": True,  # fixed synthetic fixture, NEVER child content
                "image": pic,
                "audio": wav,
            })
        manifest = {
            "schema_version": 1,
            "book_id": "forest_friends",
            "title": "HARDWARE SMOKE TEST ONLY",
            "approved": True,  # fixture packaging compatibility only
            "pages": pages,
        }
        source = root / "book.json"
        source.write_text(json.dumps(manifest, ensure_ascii=False), encoding="utf-8")
        result = make_pack(source, target, reviewed=True)
    readme = (
        "WAVESHARE 3.97 HARDWARE SMOKE TEST ONLY\n"
        "Not a child's story; synthetic diagnostic tones only.\n"
        "Copy books/forest_friends.kpb onto a FAT32 TF card at /books/forest_friends.kpb.\n"
        "BOOT long press: Pet mode; double click: story; single click: next page;\n"
        "double click: stop; triple click on home: feed.\n"
        "PAGE1: LEFT black circle, 440 Hz tone; PAGE2: TWO right black bars, 659 Hz tone.\n"
        "Review the device volume before playback. Do not use with young children.\n"
        "Do not interpret checksum as content authentication.\n"
        f"SHA256: {result['sha256']}\n"
    )
    (output / "README-SMOKE-ONLY.txt").write_text(readme, encoding="utf-8")
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    result = build(args.output)
    print("DIAGNOSTIC_ONLY", result["sha256"], result["package_bytes"])


if __name__ == "__main__":
    main()
