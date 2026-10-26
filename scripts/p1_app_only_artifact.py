#!/usr/bin/env python3
"""Package a build-only ESP32-S3 application image for OTA-slot review.

Never outputs merged Flash, bootloader, partition table, NVS or shared assets.
Does not talk to hardware or authorize flashing.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct

BOARD = "waveshare/esp32-s3-epaper-3.97"
SLOT_BYTES = 0x3F0000
APP_DESC_MAGIC = 0xABCD5432
NAME = "xiaozhi-pet-esp32s3-app.bin"


def metadata(image: bytes) -> dict:
    if not 100_000 < len(image) <= SLOT_BYTES:
        raise ValueError("Application size exceeds the OTA slot or is unexpectedly small")
    if image[0] != 0xE9 or not (1 <= image[1] <= 16):
        raise ValueError("Not an ESP-IDF application image")
    # App descriptor must immediately follow the first segment header.
    # A merged whole-Flash image starts with a bootloader and fails here.
    if struct.unpack_from("<I", image, 32)[0] != APP_DESC_MAGIC:
        raise ValueError("Missing ESP application descriptor; merged image forbidden")
    version = image[48:80].split(b"\x00", 1)[0].decode("ascii")
    project = image[80:112].split(b"\x00", 1)[0].decode("ascii")
    if not re.fullmatch(r"[A-Za-z0-9._+\-]{1,32}", version):
        raise ValueError("Unrecognized application version")
    if not re.fullmatch(r"[A-Za-z0-9._+\-]{1,32}", project):
        raise ValueError("Unrecognized application name")
    return {
        "image_size": len(image),
        "image_sha256": hashlib.sha256(image).hexdigest(),
        "app_version": version,
        "app_project": project,
    }


def package(image_file: Path, out_dir: Path, upstream_commit: str,
            pet_commit: str, run_id: str) -> dict:
    for value in (upstream_commit, pet_commit):
        if not re.fullmatch(r"[a-f0-9]{40}", value):
            raise ValueError("Expected immutable full 40-character git commit SHA")
    if not re.fullmatch(r"[0-9]+", run_id):
        raise ValueError("Expected GitHub Actions run ID")
    info = metadata(image_file.read_bytes())
    out_dir.mkdir(parents=True, exist_ok=True)
    output = out_dir / NAME
    if output.resolve() == image_file.resolve():
        raise ValueError("Source and output must differ")
    shutil.copyfile(image_file, output)
    if hashlib.sha256(output.read_bytes()).hexdigest() != info["image_sha256"]:
        output.unlink(missing_ok=True)
        raise ValueError("App-only copy SHA256 mismatch")
    manifest = {
        "schema_version": 1,
        "status": "BUILD_ONLY_NOT_AUTHORIZED_FOR_FLASH",
        "device": BOARD,
        "chip": "ESP32-S3",
        "image_kind": "application_only",
        "image_filename": NAME,
        "maximum_single_ota_partition_bytes": SLOT_BYTES,
        **info,
        "upstream_commit": upstream_commit,
        "xiaozhi_pet_commit": pet_commit,
        "github_run_id": run_id,
        "contains_bootloader": False,
        "contains_partition_table": False,
        "contains_assets": False,
        "contains_nvs": False,
        "safe_to_flash_without_checks": False,
    }
    (out_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--upstream-commit", required=True)
    parser.add_argument("--pet-commit", required=True)
    parser.add_argument("--run-id", required=True)
    args = parser.parse_args()
    manifest = package(args.image, args.out, args.upstream_commit,
                       args.pet_commit, args.run_id)
    print(f"APP_ONLY_BUILD_OK: bytes={manifest['image_size']} sha256={manifest['image_sha256']}")
    print("NO_FLASH_WRITE; no merged image, NVS, assets, partition table or bootloader published")


if __name__ == "__main__":
    main()
