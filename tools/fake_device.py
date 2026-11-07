#!/usr/bin/env python3
"""Reference downloader for the device firmware (plan S4, D10).

Pulls the manifest, compares it with a directory standing in for the TF card,
downloads with ``Range`` resume into ``<path>.part`` while hashing, renames only
after the sha256 matches, removes cancelled packs, then reports results and
the card inventory (``sd_free_kib``). Stdlib only; the token comes from the
environment, never argv:

    KID_PET_DEVICE_TOKEN=... python tools/fake_device.py --base https://pets.example \
        --device-id pet-001 --card ./card
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import struct
import sys
import urllib.error
import urllib.request
from pathlib import Path

MAX_ATTEMPTS = 3
SPACE_RESERVE_KIB = 1024
CHUNK = 16 * 1024
# (magic, header CRC offset, content_id offset/format, revision offset/format) per pack type
LAYOUT = {b"KPB2": (92, ("<H", 16), ("<H", 18)),
          b"KPG1": (28, ("<H", 10), ("<I", 12)),
          b"KPS1": (28, None, ("<I", 12))}
KIND = {b"KPB2": "book", b"KPG1": "game", b"KPS1": "soundpack"}


class Response:
    def __init__(self, status: int, headers: dict, chunks) -> None:
        self.status, self.headers, self.chunks = status, {k.lower(): v for k, v in headers.items()}, chunks

    def read(self) -> bytes:
        return b"".join(self.chunks)


def urllib_transport(base: str):
    def send(method: str, path: str, headers: dict, body: bytes | None = None) -> Response:
        request = urllib.request.Request(base.rstrip("/") + path, data=body, method=method, headers=headers)
        try:
            resp = urllib.request.urlopen(request, timeout=30)  # noqa: S310 - https only in production
        except urllib.error.HTTPError as error:
            resp = error
        def chunks():
            while True:
                data = resp.read(CHUNK)
                if not data:
                    return
                yield data
        return Response(resp.status if hasattr(resp, "status") else resp.code, dict(resp.headers), chunks())
    return send


def card_file(card: Path, path: str) -> Path:
    target = (card / path.lstrip("/")).resolve()
    if card.resolve() not in target.parents:
        raise ValueError("path escapes the card")
    return target


def fingerprint(file: Path) -> dict | None:
    """What the firmware reads from a pack header (never trusts the file name)."""
    try:
        with file.open("rb") as handle:
            head = handle.read(96)
        layout = LAYOUT.get(head[:4])
        if layout is None or len(head) < layout[0] + 4:
            return None
        crc_at, cid, rev = layout
        return {"kind": KIND[head[:4]], "header_crc": struct.unpack_from("<I", head, crc_at)[0],
                "content_id": struct.unpack_from(cid[0], head, cid[1])[0] if cid else None,
                "revision": struct.unpack_from(rev[0], head, rev[1])[0], "bytes": file.stat().st_size}
    except (OSError, struct.error):
        return None


def same_pack(found: dict | None, entry: dict) -> bool:
    return bool(found and found["kind"] == entry["kind"] and found["revision"] == entry["revision"]
                and found["header_crc"] == entry["header_crc"]
                and (entry["kind"] == "soundpack" or found["content_id"] == entry["content_id"]))


class FakeDevice:
    def __init__(self, send, device_id: str, token: str, card: Path, *, sd_free_kib: int | None = None,
                 max_bytes: int | None = None, log=print) -> None:
        self.send, self.device_id, self.card, self.log = send, device_id, Path(card), log
        self.sd_free_kib, self.max_bytes = sd_free_kib, max_bytes  # max_bytes: simulate a dropped link
        self.auth = {"Authorization": "Bearer " + token}

    def call(self, method: str, path: str, payload=None, extra=None) -> Response:
        headers = {**self.auth, **(extra or {})}
        body = None
        if payload is not None:
            body = json.dumps(payload).encode()
            headers["Content-Type"] = "application/json"
        return self.send(method, path, headers, body)

    def free_kib(self) -> int:
        return self.sd_free_kib if self.sd_free_kib is not None else shutil.disk_usage(self.card).free // 1024

    def inventory(self) -> list[dict]:
        found = []
        for sub in ("books", "games", "sounds"):
            for file in sorted((self.card / sub).glob("*")) if (self.card / sub).is_dir() else []:
                info = fingerprint(file)
                if info:
                    found.append(info)
        return found

    def report(self, sha: str, state: str, reason: str | None = None, attempts: int = 0) -> None:
        body = {"sha256": sha, "state": state, "attempts": attempts}
        if reason:
            body["reason"] = reason
        self.call("POST", f"/v1/devices/{self.device_id}/content/report", body)

    def download(self, entry: dict) -> str | None:
        """One attempt: resume ``.part``, verify, rename. Returns an error code or None."""
        final = card_file(self.card, entry["path"])
        part = final.with_name(final.name + ".part")
        final.parent.mkdir(parents=True, exist_ok=True)
        have = part.stat().st_size if part.exists() else 0
        if have > entry["bytes"]:
            part.unlink()
            have = 0
        digest = hashlib.sha256()
        if have:  # the firmware keeps the running hash instead; re-reading is the portable way
            with part.open("rb") as old:
                while block := old.read(CHUNK):
                    digest.update(block)
        extra = {"X-Device-Id": self.device_id}
        if have:
            extra["Range"] = f"bytes={have}-"
            extra["If-Range"] = f'"{entry["sha256"]}"'
        response = self.call("GET", entry["url"], extra=extra)
        if response.status == 416:  # our .part is not usable
            part.unlink(missing_ok=True)
            return "RANGE_RESET"
        if response.status == 200 and have:  # server ignored Range: start over
            part.unlink()
            digest, have = hashlib.sha256(), 0
        elif response.status not in (200, 206):
            return f"HTTP_{response.status}"
        if response.status == 206 and response.headers.get("content-range", "").split("/")[0] \
                != f"bytes {have}-{entry['bytes'] - 1}":
            return "BAD_CONTENT_RANGE"
        written = have
        with part.open("ab" if have else "wb") as out:
            for chunk in response.chunks:
                if self.max_bytes is not None and written + len(chunk) > self.max_bytes:
                    return "INTERRUPTED"
                out.write(chunk)
                digest.update(chunk)
                written += len(chunk)
        if written != entry["bytes"] or digest.hexdigest() != entry["sha256"]:
            part.unlink(missing_ok=True)
            return "SHA256_MISMATCH"
        os.replace(part, final)  # only a verified file ever gets the real name
        return None

    def run(self) -> dict:
        got = self.call("GET", f"/v1/devices/{self.device_id}/content/manifest")
        if got.status != 200:
            raise RuntimeError(f"manifest HTTP {got.status}")
        manifest = json.loads(got.read())
        result = {"installed": [], "failed": {}, "removed": [], "skipped": []}
        for entry in manifest["install"]:
            target = card_file(self.card, entry["path"])
            if same_pack(fingerprint(target), entry):
                result["skipped"].append(entry["sha256"])
                self.report(entry["sha256"], "installed")
                continue
            if entry["bytes"] // 1024 + SPACE_RESERVE_KIB > self.free_kib():
                result["failed"][entry["sha256"]] = "NO_SPACE"
                self.report(entry["sha256"], "failed", "NO_SPACE")
                continue
            self.report(entry["sha256"], "downloading")
            error = "NOT_TRIED"
            for attempt in range(1, MAX_ATTEMPTS + 1):
                error = self.download(entry)
                if error in (None, "INTERRUPTED"):
                    break
            if error is None:
                result["installed"].append(entry["sha256"])
                self.report(entry["sha256"], "installed", attempts=attempt)
            elif error == "INTERRUPTED":
                result["failed"][entry["sha256"]] = error  # keep .part; resume next run
            else:
                result["failed"][entry["sha256"]] = error
                self.report(entry["sha256"], "failed", error, attempt)
        for entry in manifest["remove"]:
            target = card_file(self.card, entry["path"])
            if same_pack(fingerprint(target), entry):  # never delete a file we did not install
                target.unlink()
            if not target.exists():
                result["removed"].append(entry["sha256"])
                self.report(entry["sha256"], "removed")
        inv = self.call("PUT", f"/v1/devices/{self.device_id}/content/inventory",
                        {"items": self.inventory(), "sd_free_kib": self.free_kib()})
        result["inventory_status"] = inv.status
        return result


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--base", required=True, help="server origin, e.g. https://pets.example")
    parser.add_argument("--device-id", required=True)
    parser.add_argument("--card", required=True, help="directory standing in for the TF card")
    parser.add_argument("--sd-free-kib", type=int, help="report this instead of the real free space")
    args = parser.parse_args(argv)
    token = os.environ.get("KID_PET_DEVICE_TOKEN", "")
    if len(token) < 32:
        print("KID_PET_DEVICE_TOKEN missing or too short", file=sys.stderr)
        return 2
    result = FakeDevice(urllib_transport(args.base), args.device_id, token, Path(args.card),
                        sd_free_kib=args.sd_free_kib).run()
    print(json.dumps(result, indent=2))
    return 1 if result["failed"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
