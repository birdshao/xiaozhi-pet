"""Content catalog (plan S4, §7.4): import, sha256-bound approval, derived text.

Packs are validated by the same parsers the tools and firmware use (KPB2,
KPG1, KPS1), stored as ``<packages>/<sha256>`` and only visible to devices,
manifests and page Q&A once a parent approved *that exact sha256*.
"""
from __future__ import annotations

import hashlib
import json
import mmap
import os
from pathlib import Path
import secrets
import struct
import sys
import time
from typing import Callable

from .db import connect, migrate

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:  # tools/ is a namespace package at the repo root
    sys.path.insert(0, str(ROOT))
from tools import game_pack, kpb2, sound_pack  # noqa: E402

MAX_UPLOAD_BYTES = 64 * 1024 * 1024
PACK_ID_RE = sound_pack.PACK_ID_RE
HEADER_CRC_OFFSET = {"KPB2": 92, "KPG1": 28, "KPS1": 28}
FINGERPRINT_KINDS = ("book", "soundpack", "game")
STALE_UPLOAD_SECONDS = 3600
HASH_CHUNK = 1024 * 1024
# Items the reviewing adult must tick (docs/V4_CONTENT_REVIEW_CHECKLIST.md): exactly
# these keys, all true. An unknown or missing key is an incomplete review.
_COMMON = {"source_licensed", "audio_listened", "no_identity_info", "compiled_validated"}
REQUIRED_CHECKLIST = {
    "book": _COMMON | {"pages_text_safe", "pages_audio_matches_text", "previews_checked",
                       "questions_checked"},
    "game": _COMMON | {"prompts_text_safe", "answers_correct"},
    "soundpack": _COMMON | {"phrases_text_safe"},
}


class ContentError(Exception):
    def __init__(self, status: int, code: str) -> None:
        self.status = status
        self.code = code
        super().__init__(code)


def audit(db, now, actor, action, target, result, ip=None):
    """Operation types only; never child content or tokens."""
    db.execute("INSERT INTO audit_log(at,actor,action,target,result,ip) VALUES(?,?,?,?,?,?)",
               (now, actor, action, str(target)[:80], result, ip))


def inspect(blob: bytes, pack_id: str | None = None) -> dict:
    """Validate with the existing tool parsers and return catalog metadata."""
    magic = bytes(blob[:4])
    if len(blob) == 0:
        raise ContentError(415, "UNSUPPORTED_PACK_FORMAT")
    try:
        if magic == b"KPB2":
            parsed = kpb2.parse(blob)
            meta = {"kind": "book", "content_id": parsed["content_id"],
                    "revision": parsed["revision"], "title": parsed["title"],
                    "path": f"/books/c{parsed['content_id']}.kpb", "parsed": parsed,
                    # V8: KPB2 v3 story graph and series, already validated by kpb2.parse
                    "branching": parsed["branching"], "series": parsed["series"]}
        elif magic == b"KPG1":
            parsed = game_pack.parse(blob)
            slug = pack_id or f"g{parsed['content_id']}"
            meta = {"kind": "game", "content_id": parsed["content_id"],
                    "revision": parsed["revision"], "title": f"游戏包 {slug}",
                    "path": f"/games/{slug}.kpg", "parsed": parsed}
        elif magic == b"KPS1":
            revision, clips = sound_pack.decode(blob)
            slug = pack_id or sound_pack.DEFAULT_PACK_ID
            meta = {"kind": "soundpack", "content_id": None, "revision": revision,
                    "title": f"声音包 {slug}", "path": f"/sounds/{slug}.kps",
                    "parsed": {"clips": len(clips)}}
        else:
            raise ContentError(415, "UNSUPPORTED_PACK_FORMAT")
    except ContentError:
        raise
    except (ValueError, struct.error, UnicodeDecodeError, KeyError, IndexError, OverflowError):
        raise ContentError(422, "PACK_INVALID") from None
    if pack_id is not None and (meta["kind"] == "book" or not PACK_ID_RE.fullmatch(pack_id)):
        raise ContentError(422, "PACK_ID_INVALID")
    meta["header_crc"] = struct.unpack_from("<I", blob, HEADER_CRC_OFFSET[magic.decode()])[0]
    return meta


def package_view(row) -> dict:
    return {"package_id": row["package_id"], "kind": row["kind"], "content_id": row["content_id"],
            "revision": row["revision"], "sha256": row["sha256"], "header_crc": row["header_crc"],
            "bytes": row["bytes"], "path": row["path"], "title": row["title"],
            "status": row["status"], "created_at": row["created_at"],
            "reviewed_by": row["reviewed_by"], "reviewed_at": row["reviewed_at"]}


class ContentCatalog:
    def __init__(self, database, packages_dir=None, *, clock=time.time) -> None:
        self.database = database
        self.clock = clock
        self.packages_dir = Path(packages_dir or os.environ.get("KID_PET_PACKAGES_DIR")
                                 or Path(str(database)).parent / "packages")
        migrate(database)
        self.packages_dir.mkdir(parents=True, exist_ok=True)
        self.cleanup_spool()
        self._sync_hooks: list[Callable[[str], None]] = []

    # -- SYNC_CONTENT hook -------------------------------------------------
    def on_sync_needed(self, callback: Callable[[str], None]) -> None:
        """S2 registers ``lambda device_id: commands.enqueue(device_id, 'SYNC_CONTENT')``.

        Called after commit for each device whose manifest changed. Until a
        command channel exists, devices poll the manifest on snapshot cadence.
        """
        self._sync_hooks.append(callback)

    def notify(self, device_ids) -> None:
        for device_id in sorted(set(device_ids)):
            for callback in self._sync_hooks:
                try:
                    callback(device_id)
                except Exception:  # a command-channel failure must not undo the commit
                    pass

    def package_path(self, sha256: str) -> Path:
        return self.packages_dir / sha256

    # -- import ------------------------------------------------------------
    def cleanup_spool(self) -> None:
        """Drop upload temp files an earlier crash left behind (older than an hour)."""
        cutoff = self.clock() - STALE_UPLOAD_SECONDS
        for stale in self.packages_dir.glob(".upload-*"):
            try:
                if stale.stat().st_mtime < cutoff:
                    stale.unlink()
            except OSError:
                pass

    def new_spool(self) -> Path:
        return self.packages_dir / f".upload-{secrets.token_hex(8)}"

    def import_blob(self, blob: bytes, **kw) -> tuple[dict, bool]:
        """Convenience for small in-memory packs (CLI, tests)."""
        spool = self.new_spool()
        spool.write_bytes(blob)
        return self.import_file(spool, hashlib.sha256(blob).hexdigest(), len(blob), **kw)

    def import_file(self, spool: Path, sha: str, size: int, *, pack_id: str | None = None,
                    actor: str = "parent", ip: str | None = None) -> tuple[dict, bool]:
        """Validate a spooled file, store it content-addressed and register a ``draft``.

        Takes ownership of ``spool`` (moved into place or deleted). ``sha``/``size``
        were computed while spooling, so nothing is held in memory here.
        """
        try:
            if size > MAX_UPLOAD_BYTES:
                raise ContentError(413, "PACK_TOO_LARGE")
            with open(spool, "rb") as handle:
                view = mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ) if size else b""
                try:
                    meta = inspect(view, pack_id)
                finally:
                    if size:
                        view.close()
            meta.pop("parsed", None)
        except ContentError as error:
            spool.unlink(missing_ok=True)
            with connect(self.database) as db:
                audit(db, int(self.clock()), actor, "content.import", "-", error.code, ip)
            raise
        target = self.package_path(sha)
        if target.exists():
            spool.unlink(missing_ok=True)
        else:
            os.replace(spool, target)
        now = int(self.clock())
        with connect(self.database) as db:
            db.execute("BEGIN IMMEDIATE")
            row = db.execute("SELECT * FROM content_packages WHERE sha256=?", (sha,)).fetchone()
            created = row is None
            if created:
                db.execute(
                    "INSERT INTO content_packages(kind,content_id,revision,sha256,header_crc,bytes,"
                    "path,title,created_at) VALUES(?,?,?,?,?,?,?,?,?)",
                    (meta["kind"], meta["content_id"], meta["revision"], sha, meta["header_crc"],
                     size, meta["path"], meta["title"], now))
                row = db.execute("SELECT * FROM content_packages WHERE sha256=?", (sha,)).fetchone()
            audit(db, now, actor, "content.import", sha[:16], "created" if created else "duplicate", ip)
            return package_view(row), created

    # -- queries -----------------------------------------------------------
    def get(self, package_id: int) -> dict | None:
        with connect(self.database) as db:
            row = db.execute("SELECT * FROM content_packages WHERE package_id=?", (package_id,)).fetchone()
        return package_view(row) if row else None

    def list(self) -> list[dict]:
        with connect(self.database) as db:
            rows = db.execute("SELECT * FROM content_packages ORDER BY package_id").fetchall()
        return [package_view(r) for r in rows]

    def page_text(self, content_id: int, revision: int, page: int, header_crc: int) -> str | None:
        """Reviewed page text for page Q&A (plan §6.3); ``None`` if not approved.

        ``header_crc`` is the fingerprint the device reported (required); a mismatch
        means its copy is not the reviewed file, so no text is returned.
        """
        with connect(self.database) as db:
            row = db.execute(
                "SELECT p.text FROM content_pages p JOIN content_packages k USING(package_id) "
                "WHERE k.kind='book' AND k.status='approved' AND k.content_id=? AND k.revision=? "
                "AND k.header_crc=? AND p.page=?", (content_id, revision, header_crc, page)).fetchone()
        return row["text"] if row else None

    def approved_by_fingerprint(self, kind: str, revision: int, header_crc: int,
                                content_id: int | None = None) -> dict | None:
        """KPB2/KPG1 match on content_id; KPS1 (no id) on revision + header CRC."""
        sql = "SELECT * FROM content_packages WHERE kind=? AND status='approved' AND revision=? AND header_crc=?"
        args: list = [kind, revision, header_crc]
        if kind != "soundpack":
            sql += " AND content_id=?"
            args.append(content_id)
        with connect(self.database) as db:
            row = db.execute(sql, args).fetchone()
        return package_view(row) if row else None

    def like_names(self) -> dict[int, dict]:
        """``{slot: {"key", "label", "options": {option: label}}}`` from approved books."""
        out: dict[int, dict] = {}
        with connect(self.database) as db:
            for r in db.execute(
                    "SELECT DISTINCT t.slot,t.key,t.label FROM like_topics t JOIN content_packages k "
                    "USING(package_id) WHERE k.status='approved'"):
                out[r["slot"]] = {"key": r["key"], "label": r["label"], "options": {}}
            for r in db.execute(
                    "SELECT DISTINCT o.slot,o.option,o.label FROM like_options o JOIN content_packages k "
                    "USING(package_id) WHERE k.status='approved'"):
                out.setdefault(r["slot"], {"key": "", "label": "", "options": {}})["options"][r["option"]] = r["label"]
        return out

    # -- review ------------------------------------------------------------
    def approve(self, package_id: int, sha256: str, checklist: dict[str, bool],
                reviewer: str, *, ip: str | None = None) -> dict:
        """Approve exactly ``sha256``; supersede older approved packs at the same path."""
        now = int(self.clock())
        notify: list[str] = []
        with connect(self.database) as db:
            row = db.execute("SELECT * FROM content_packages WHERE package_id=?", (package_id,)).fetchone()
        if row is None:
            raise ContentError(404, "PACKAGE_NOT_FOUND")
        if row["sha256"] != sha256:
            with connect(self.database) as db:
                audit(db, now, "parent", "content.approve", row["sha256"][:16], "sha_mismatch", ip)
            raise ContentError(409, "SHA256_MISMATCH")
        if row["status"] == "approved":
            raise ContentError(409, "ALREADY_APPROVED")
        if set(checklist) != REQUIRED_CHECKLIST[row["kind"]] or not all(v is True for v in checklist.values()):
            raise ContentError(400, "CHECKLIST_INCOMPLETE")
        # Re-hash and re-parse the stored bytes (up to 64 MiB) before taking the write
        # lock: approval covers the file, not a label.
        parsed = self._verify_file(sha256, row)
        with connect(self.database) as db:
            db.execute("BEGIN IMMEDIATE")
            current = db.execute("SELECT * FROM content_packages WHERE package_id=?", (package_id,)).fetchone()
            if current["sha256"] != sha256 or current["status"] == "approved":
                raise ContentError(409, "ALREADY_APPROVED" if current["status"] == "approved" else "SHA256_MISMATCH")
            older = db.execute(
                "SELECT * FROM content_packages WHERE kind=? AND path=? AND status='approved' "
                "AND package_id!=?", (row["kind"], row["path"], package_id)).fetchall()
            for old in older:
                if old["content_id"] == row["content_id"] and row["revision"] <= old["revision"]:
                    raise ContentError(409, "REVISION_NOT_NEWER")
            db.execute("UPDATE content_packages SET status='approved',reviewed_by=?,reviewed_at=?,"
                       "checklist_json=? WHERE package_id=?",
                       (reviewer, now, json.dumps(checklist, sort_keys=True), package_id))
            for old in older:
                db.execute("UPDATE content_packages SET status='retired' WHERE package_id=?",
                           (old["package_id"],))
                # Devices that wanted the old version move to the new one automatically.
                for dev in db.execute("SELECT device_id FROM device_content WHERE package_id=? "
                                      "AND wanted='install'", (old["package_id"],)).fetchall():
                    db.execute("DELETE FROM device_content WHERE device_id=? AND package_id=?",
                               (dev["device_id"], old["package_id"]))
                    db.execute("INSERT OR IGNORE INTO device_content(device_id,package_id,wanted,state,"
                               "updated_at) VALUES(?,?,'install','pending',?)",
                               (dev["device_id"], package_id, now))
                    notify.append(dev["device_id"])
            self._derive(db, package_id, row["kind"], parsed)
            audit(db, now, "parent", "content.approve", sha256[:16], "success", ip)
            result = package_view(db.execute("SELECT * FROM content_packages WHERE package_id=?",
                                             (package_id,)).fetchone())
        self.notify(notify)
        return result

    def _verify_file(self, sha256: str, row) -> dict:
        digest = hashlib.sha256()
        try:
            with open(self.package_path(sha256), "rb") as handle:
                while block := handle.read(HASH_CHUNK):
                    digest.update(block)
                if digest.hexdigest() != sha256:
                    raise ContentError(500, "PACKAGE_FILE_CORRUPT")
                view = mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ)
        except OSError:
            raise ContentError(500, "PACKAGE_FILE_MISSING") from None
        try:
            return inspect(view, Path(row["path"]).stem if row["kind"] != "book" else None)["parsed"]
        finally:
            view.close()

    @staticmethod
    def _derive(db, package_id: int, kind: str, parsed: dict) -> None:
        db.execute("DELETE FROM content_pages WHERE package_id=?", (package_id,))
        db.execute("DELETE FROM like_topics WHERE package_id=?", (package_id,))
        db.execute("DELETE FROM like_options WHERE package_id=?", (package_id,))
        if kind != "book":
            return
        by_slot = {slot: (key, label, {i: (k, text) for k, (i, text) in options.items()})
                   for key, (slot, label, options) in kpb2.PREFERENCE_TOPICS.items()}
        for page in parsed["pages"]:
            db.execute("INSERT INTO content_pages(package_id,page,text) VALUES(?,?,?)",
                       (package_id, page["number"], page["text"]))
            for question in page["questions"]:
                slot = question["like_slot"]
                if not slot or slot not in by_slot:
                    continue
                key, label, options = by_slot[slot]
                db.execute("INSERT OR IGNORE INTO like_topics VALUES(?,?,?,?)", (package_id, slot, key, label))
                for option in question["options"]:
                    if option["like"] and option["id"] in options:
                        okey, olabel = options[option["id"]]
                        db.execute("INSERT OR IGNORE INTO like_options VALUES(?,?,?,?,?)",
                                   (package_id, slot, option["id"], okey, olabel))

    def retire(self, package_id: int, *, ip: str | None = None) -> dict:
        now = int(self.clock())
        notify: list[str] = []
        with connect(self.database) as db:
            db.execute("BEGIN IMMEDIATE")
            row = db.execute("SELECT * FROM content_packages WHERE package_id=?", (package_id,)).fetchone()
            if row is None:
                raise ContentError(404, "PACKAGE_NOT_FOUND")
            if row["status"] == "retired":
                raise ContentError(409, "ALREADY_RETIRED")
            db.execute("UPDATE content_packages SET status='retired' WHERE package_id=?", (package_id,))
            for dev in db.execute("SELECT device_id,state FROM device_content WHERE package_id=? "
                                  "AND wanted='install'", (package_id,)).fetchall():
                if dev["state"] in ("installed", "downloading"):
                    # Only packs the downloader put on the card are ever removed.
                    db.execute("UPDATE device_content SET wanted='remove',updated_at=? "
                               "WHERE device_id=? AND package_id=?", (now, dev["device_id"], package_id))
                else:
                    db.execute("DELETE FROM device_content WHERE device_id=? AND package_id=?",
                               (dev["device_id"], package_id))
                notify.append(dev["device_id"])
            audit(db, now, "parent", "content.retire", row["sha256"][:16], "success", ip)
            result = package_view(db.execute("SELECT * FROM content_packages WHERE package_id=?",
                                             (package_id,)).fetchone())
        self.notify(notify)
        return result


def main(argv=None) -> int:
    """``python -m services.pet_learning.content import-pack FILE [--pack-id ID]`` (trusted, local)."""
    import argparse
    from .config import Settings
    parser = argparse.ArgumentParser(description=main.__doc__)
    parser.add_argument("--db", default=Settings.load().database)
    sub = parser.add_subparsers(dest="command", required=True)
    imp = sub.add_parser("import-pack", help="Validate a KPB2/KPG1/KPS1 pack and store it as a draft")
    imp.add_argument("file")
    imp.add_argument("--pack-id")
    args = parser.parse_args(argv)
    size = Path(args.file).stat().st_size
    if size > MAX_UPLOAD_BYTES:
        raise SystemExit("PACK_TOO_LARGE")
    try:
        package, created = ContentCatalog(args.db).import_blob(
            Path(args.file).read_bytes(), pack_id=args.pack_id, actor="cli")
    except ContentError as error:
        raise SystemExit(error.code) from None
    print(("PACK_DRAFT_CREATED " if created else "PACK_ALREADY_IMPORTED ")
          + f"id={package['package_id']} sha256={package['sha256']} (review it in the parent panel)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
