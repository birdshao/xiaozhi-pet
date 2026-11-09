#!/usr/bin/env python3
"""Cut the official assets image (2,900,513 B @ 0x800000) out of a full-flash backup.

Verifies the SHA-256 and writes a file only; it never touches the board. Flash it
yourself, writing only the assets partition:

    esptool --chip esp32s3 -p COMx write-flash 0x800000 <out>

Before flashing anything, archive what is on the board now:

    esptool --chip esp32s3 -p COMx read-flash 0x800000 2900513 assets-before.bin
"""
from __future__ import annotations

import argparse
import hashlib
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from scripts.p1_assets_artifact import (ASSETS_OFFSET, OFFICIAL_ASSETS_BYTES,
                                        OFFICIAL_ASSETS_SHA256)


def cut(image: Path, out: Path) -> str:
    with image.open("rb") as f:
        f.seek(ASSETS_OFFSET)
        data = f.read(OFFICIAL_ASSETS_BYTES)
    digest = hashlib.sha256(data).hexdigest()
    if len(data) != OFFICIAL_ASSETS_BYTES or digest != OFFICIAL_ASSETS_SHA256:
        raise ValueError(f"not the official assets image (got {len(data)} B, sha256 {digest})")
    out.write_bytes(data)
    return digest


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--image", required=True, type=Path, help="full-flash backup")
    p.add_argument("--out", required=True, type=Path)
    a = p.parse_args()
    try:
        print("OFFICIAL_ASSETS_OK sha256=" + cut(a.image, a.out))
    except (OSError, ValueError) as e:
        sys.exit(f"restore_official_assets rejected: {e}")
