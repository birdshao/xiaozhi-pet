#!/usr/bin/env python3
"""Decode an ESP32-S3 panic from a serial log (no board needed).

Reads the ``Backtrace:`` line (and the register dump's PC / error address)
of a sanitized log and turns the code addresses into function names and
source lines with the IDF toolchain's ``xtensa-esp32s3-elf-addr2line``
against the build's ELF. CI uploads that ELF as the review-only artifact
``pet-ota1-elf-debug`` (see .github/workflows/p1-integrated-firmware.yml);
the log's ``ELF file SHA256: <prefix>`` must match the ELF's app SHA, which
this tool checks when it can.

    python tools/decode_panic.py selftest/prev/run1-panic.log --elf xiaozhi.elf
    python tools/decode_panic.py panic.log          # only lists what it found

``Dbus write to cache rejected`` means a store hit an address mapped from
flash (read-only): a const object or an ``esp_partition_mmap`` region (the
XiaoZhi assets / srmodels partition is mapped that way), not a bug in a
RAM-only structure.
"""
from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

ADDR2LINE = "xtensa-esp32s3-elf-addr2line"
_BACKTRACE = re.compile(r"^Backtrace:\s*(.*)$", re.MULTILINE)
_FRAME = re.compile(r"0x([0-9a-fA-F]{8}):0x[0-9a-fA-F]{8}")
_PANIC = re.compile(r"Core\s+(\d+) panic'ed \(([^)]*)\)")
_ERR_ADDR = re.compile(r"error address:\s*(0x[0-9a-fA-F]+)")
_SHA = re.compile(r"^ELF file SHA256:\s*([0-9a-f]+)", re.MULTILINE)
_REG = re.compile(r"\b(PC|A0)\s*:\s*0x([0-9a-fA-F]{8})")


def parse_panic(log: str) -> dict:
    """What a panic log says: reason, error address, ELF SHA prefix, PC and
    the backtrace's code addresses (``0x4...``), in call order."""
    panic = _PANIC.search(log)
    err = _ERR_ADDR.search(log)
    sha = _SHA.search(log)
    bt = _BACKTRACE.search(log)
    regs = {name: "0x" + value for name, value in _REG.findall(log)}
    return {
        "core": int(panic.group(1)) if panic else None,
        "reason": panic.group(2) if panic else None,
        "error_address": err.group(1) if err else None,
        "elf_sha_prefix": sha.group(1) if sha else None,
        "pc": regs.get("PC"),
        "backtrace": ["0x" + a.lower() for a in _FRAME.findall(bt.group(1))] if bt else [],
    }


def flash_mapped(address: str) -> bool:
    """True for the S3 data-cache window (0x3C000000..0x3DFFFFFF) where flash
    (rodata, mmap'd partitions) and PSRAM are mapped: a store there that the
    cache rejects hit read-only flash."""
    return 0x3C000000 <= int(address, 16) < 0x3E000000


def decode(addresses: list[str], elf: Path, tool: str | None = None) -> str:
    exe = tool or shutil.which(ADDR2LINE)
    if not exe:
        raise FileNotFoundError(f"{ADDR2LINE} not found: source the IDF export.sh first")
    done = subprocess.run([exe, "-pfiaC", "-e", str(elf), *addresses],
                          capture_output=True, text=True, check=True)
    return done.stdout


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("log", type=Path)
    parser.add_argument("--elf", type=Path, help="the build's xiaozhi.elf")
    args = parser.parse_args()
    found = parse_panic(args.log.read_text(encoding="utf-8", errors="replace"))
    for key, value in found.items():
        print(f"{key}: {value}")
    if found["error_address"] and flash_mapped(found["error_address"]):
        print("note: the error address is in the flash/PSRAM cache window; a rejected "
              "write there is a store to read-only flash (const data or an mmap'd partition)")
    if args.elf:
        addresses = ([found["pc"]] if found["pc"] else []) + found["backtrace"]
        print(decode(addresses, args.elf))
    return 0


if __name__ == "__main__":
    sys.exit(main())
