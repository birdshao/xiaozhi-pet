#!/usr/bin/env python3
"""Package the build's generated_assets.bin (assets partition image) with a manifest.

The wake word detector is chosen at run time from the srmodels inside the *assets*
partition (upstream afe_audio_engine.cc), so flashing the app alone does not change
the wake word. The manifest ties this image to the app image of the same build.
Never flashes anything.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path

ASSETS_OFFSET = 0x800000
ASSETS_PARTITION_BYTES = 8 * 1024 * 1024
OFFICIAL_ASSETS_BYTES = 2_900_513
OFFICIAL_ASSETS_SHA256 = "7476fd375dec28785a076143fe359fb5ea8fad3c6e420b8dabb8f0c158a2fc3e"
NAME = "xiaozhi-pet-assets.bin"


def package(assets: Path, app_manifest: Path, out_dir: Path, wake_word: str, run_id: str) -> dict:
    if not re.fullmatch(r"[0-9]+", run_id):
        raise ValueError("Expected GitHub Actions run ID")
    data = assets.read_bytes()
    if not 0 < len(data) <= ASSETS_PARTITION_BYTES:
        raise ValueError(f"assets image {len(data)} B does not fit the {ASSETS_PARTITION_BYTES} B partition")
    app = json.loads(app_manifest.read_text(encoding="utf-8"))
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / NAME).write_bytes(data)
    manifest = {
        "schema_version": 1,
        "status": "BUILD_ONLY_NOT_AUTHORIZED_FOR_FLASH",
        "image_kind": "assets_partition",
        "image_filename": NAME,
        "image_size": len(data),
        "image_sha256": hashlib.sha256(data).hexdigest(),
        "flash_offset": hex(ASSETS_OFFSET),
        "partition_size": ASSETS_PARTITION_BYTES,
        "wake_word": wake_word,
        "paired_app_sha256": app["image_sha256"],
        "paired_app_filename": app["image_filename"],
        "github_run_id": run_id,
        "official_assets_rollback": {"size": OFFICIAL_ASSETS_BYTES, "sha256": OFFICIAL_ASSETS_SHA256},
        "write_only_this_partition": True,
        "safe_to_flash_without_checks": False,
    }
    (out_dir / "assets-manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return manifest


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--assets", required=True, type=Path)
    p.add_argument("--app-manifest", required=True, type=Path)
    p.add_argument("--out", required=True, type=Path)
    p.add_argument("--wake-word", required=True)
    p.add_argument("--run-id", required=True)
    a = p.parse_args()
    m = package(a.assets, a.app_manifest, a.out, a.wake_word, a.run_id)
    print(f"ASSETS_BUILD_OK: bytes={m['image_size']} sha256={m['image_sha256']}")


if __name__ == "__main__":
    main()
