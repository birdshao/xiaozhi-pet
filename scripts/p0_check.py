#!/usr/bin/env python3
"""Verify pinned source metadata and board configuration without flashing a device.

For code-only checks:
  python3 scripts/p0_check.py --metadata-only
For a pinned source checkout:
  python3 scripts/p0_check.py --source .upstream/xiaozhi-esp32
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MANIFEST = ROOT / "version-lock.json"
SHA_PATTERN = re.compile(r"^[0-9a-f]{40}$")


def validate_manifest(data: dict) -> None:
    if data.get("schema_version") != 1:
        raise ValueError("Unsupported version-lock.json schema_version")
    for key in ("firmware", "voice_server"):
        entry = data.get(key, {})
        if not isinstance(entry, dict):
            raise ValueError(f"{key} must be an object")
        if not entry.get("repository", "").startswith("https://github.com/"):
            raise ValueError(f"{key} repository must be a public HTTPS GitHub URL")
        if not entry.get("tag"):
            raise ValueError(f"{key} tag missing")
        if not SHA_PATTERN.fullmatch(entry.get("commit", "")):
            raise ValueError(f"{key} commit must be a full 40-character SHA")
    board = data.get("board", {})
    if (board.get("source_dir"), board.get("variant"), board.get("target")) != (
        "waveshare/esp32-s3-epaper-3.97", "esp32-s3-epaper-3.97", "esp32s3"
    ):
        raise ValueError("Unsupported P0 board identity")
    if (board.get("width"), board.get("height")) != (800, 480):
        raise ValueError("Unexpected EPD dimensions")
    if data.get("esp_idf") != "6.1":
        raise ValueError("P0 toolchain must use pinned ESP-IDF 6.1")


def inspect_source(source: Path, manifest: dict) -> dict:
    if not (source / ".git").exists():
        raise ValueError(f"Expected a git checkout at: {source}")
    actual = subprocess.check_output(
        ["git", "-C", str(source), "rev-parse", "HEAD"],
        text=True,
        stderr=subprocess.PIPE,
    ).strip()
    expected = manifest["firmware"]["commit"]
    if actual != expected:
        raise ValueError(f"Unpinned firmware checkout: {actual}; expected {expected}")

    board_dir = source / "main" / "boards" / manifest["board"]["source_dir"]
    required = (
        "config.json", "config.h", "custom_lcd_display.h",
        "custom_lcd_display.cc", "waveshare-s3-epaper-3.97.cc",
    )
    missing = [name for name in required if not (board_dir / name).is_file()]
    if missing:
        raise ValueError(f"Missing board files: {missing}")

    cfg = json.loads((board_dir / "config.json").read_text(encoding="utf-8"))
    if cfg.get("target") != manifest["board"]["target"]:
        raise ValueError("Board config target differs from manifest")
    builds = cfg.get("builds", [])
    if not any(b.get("name") == manifest["board"]["variant"] for b in builds):
        raise ValueError("Expected board build variant is missing")
    header = (board_dir / "config.h").read_text(encoding="utf-8")
    for value in ("EXAMPLE_LCD_WIDTH   800", "EXAMPLE_LCD_HEIGHT  480"):
        if value not in header:
            raise ValueError(f"Unexpected ePaper dimensions in config.h ({value})")
    return {
        "source_commit": actual,
        "board_directory": str(board_dir),
        "board_files": list(required),
        "result": "source_static_checks_passed",
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, help="Pinned xiaozhi-esp32 git checkout")
    parser.add_argument("--metadata-only", action="store_true")
    args = parser.parse_args(argv)
    if bool(args.source) == bool(args.metadata_only):
        parser.error("select exactly one of --source and --metadata-only")

    try:
        manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
        validate_manifest(manifest)
        result = {"manifest": "valid", "board": manifest["board"]["variant"]}
        if args.source:
            result.update(inspect_source(args.source.resolve(), manifest))
        else:
            result["source_static_checks"] = "not_run"
        print(json.dumps(result, indent=2, ensure_ascii=False))
        return 0
    except (OSError, ValueError, subprocess.CalledProcessError, json.JSONDecodeError) as exc:
        print(f"P0 preflight failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
