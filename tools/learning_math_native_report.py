"""Validate *actual firmware-rendered* GameArt P4 images and export PNG previews.

The C++ integration test renders real 800x480 1-bit SceneComposer frames.
This tool does not approximate art with HTML/SVG or claim actual EPD response.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from content_pipeline.schema import ContentError, write_json

PREVIEWS = (
    "math_shape_1bit",
    "math_count_1bit",
    "math_size_1bit",
    "math_selected_1bit",
    "math_right_1bit",
)


def make_preview_report(source: Path, output: Path) -> dict:
    try:
        from PIL import Image
    except ImportError as exc:
        raise ContentError("device previews: Pillow required") from exc
    source, output = Path(source), Path(output)
    output.mkdir(parents=True, exist_ok=True)
    files = []
    unique = set()
    for name in PREVIEWS:
        pbm = source / f"{name}.pbm"
        if not pbm.is_file() or pbm.stat().st_size > 100_000:
            raise ContentError(f"device previews: missing or oversize {name}")
        binary = pbm.read_bytes()
        if not binary.startswith(b"P4\n800 480\n"):
            raise ContentError(f"device previews: wrong format or dimensions for {name}")
        try:
            with Image.open(pbm) as raw:
                if raw.format != "PPM" or raw.size != (800, 480):
                    raise ContentError(f"device previews: not the 800x480 P4 frame: {name}")
                rendered = raw.convert("L")
                histogram = rendered.histogram()
                black = sum(histogram[:128])
                if black < 300 or black > 300000:
                    raise ContentError(f"device previews: suspicious solid/blank scene: {name}")
                png = output / f"{name}.png"
                rendered.save(png, format="PNG")
        except OSError as exc:
            raise ContentError(f"device previews: corrupt PBM {name}") from exc
        sha = hashlib.sha256(binary).hexdigest()
        unique.add(sha)
        files.append({
            "scene": name,
            "pbm_sha256": sha,
            "png_sha256": hashlib.sha256(png.read_bytes()).hexdigest(),
            "black_pixels": black,
            "size": [800, 480],
        })
    if len(unique) != len(PREVIEWS):
        raise ContentError("device previews: expected five distinct scenes")
    result = {
        "schema_version": "MATHDEVICEHOST1",
        "status": "test_only",
        "engine": "firmware SceneComposer::Render via native C++",
        "resolution": [800, 480],
        "scenes": files,
        "notice": (
            "Real firmware 1-bit scene pixels, not an ESP32/EPD capture; "
            "KPG1 voices are synthetic tones. No adult approval or firmware flashing."),
    }
    write_json(output / "device-scenes.json", result)
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    result = make_preview_report(args.source, args.output)
    print(json.dumps({
        "status": result["status"], "scenes": len(result["scenes"]),
        "engine": result["engine"],
    }, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
