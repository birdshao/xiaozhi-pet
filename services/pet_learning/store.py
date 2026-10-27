"""SQLite-backed, deterministic pet projection and approved learning content.

The ESP32 remains the authority for *immediate* local actions. This service
provides durable replay protection, approved content and voice read-models.
No raw audio, ASR transcript or unreviewed AI reply is stored here.
"""
from __future__ import annotations

from contextlib import contextmanager
import hashlib
import hmac
import json
from pathlib import Path
import re
import sqlite3
from typing import Any


DEVICE_RE = re.compile(r"^[a-zA-Z0-9][a-zA-Z0-9_-]{2,63}$")
BOOK_RE = re.compile(r"^[a-z0-9][a-z0-9_-]{2,63}$")
EVENTS = frozenset({
    "PET_PICKUP", "PET_FEED", "STORY_OPEN", "PAGE_NEXT", "STORY_STOP",
    "VOICE_STARTED", "VOICE_ENDED", "QUIET_HOURS_START", "QUIET_HOURS_END",
})
MAX_UINT32 = (1 << 32) - 1


class EventRejected(ValueError):
    def __init__(self, code: str) -> None:
        self.code = code
        super().__init__(code)


def _hash_json(value: dict[str, Any]) -> str:
    return hashlib.sha256(
        json.dumps(value, sort_keys=True, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    ).hexdigest()


def _response(message_id: str, accepted: bool, version: int, seq: int,
              reason: str = "") -> dict[str, Any]:
    return {
        "message_id": message_id,
        "seq": seq,
        "accepted": accepted,
        "server_state_version": version,
        "reason_code": reason or None,
    }


class PetStore:
    """Small single-host MVP. Each operation has its own SQLite transaction."""

    def __init__(self, filename: str | Path) -> None:
        self.path = str(filename)
        if self.path != ":memory:":
            Path(self.path).parent.mkdir(parents=True, exist_ok=True)
        with self._connect() as db:
            db.executescript("""
                CREATE TABLE IF NOT EXISTS devices (
                    device_id TEXT PRIMARY KEY, token_sha TEXT NOT NULL,
                    seq INTEGER NOT NULL DEFAULT 0,
                    version INTEGER NOT NULL DEFAULT 0,
                    interaction_count INTEGER NOT NULL DEFAULT 0,
                    active_book TEXT, active_page INTEGER NOT NULL DEFAULT 1,
                    mode TEXT NOT NULL DEFAULT 'HOME', quiet INTEGER NOT NULL DEFAULT 0,
                    open_chat INTEGER NOT NULL DEFAULT 0
                );
                CREATE TABLE IF NOT EXISTS event_log (
                    device_id TEXT NOT NULL,
                    message_id TEXT NOT NULL,
                    seq INTEGER NOT NULL,
                    payload_sha TEXT NOT NULL,
                    accepted_version INTEGER NOT NULL,
                    PRIMARY KEY(device_id, message_id),
                    UNIQUE(device_id, seq),
                    FOREIGN KEY(device_id) REFERENCES devices(device_id)
                );
                CREATE TABLE IF NOT EXISTS books (
                    book_id TEXT PRIMARY KEY, title TEXT NOT NULL
                );
                CREATE TABLE IF NOT EXISTS pages (
                    book_id TEXT NOT NULL, page INTEGER NOT NULL,
                    text TEXT NOT NULL, approved INTEGER NOT NULL DEFAULT 0,
                    PRIMARY KEY(book_id, page),
                    FOREIGN KEY(book_id) REFERENCES books(book_id)
                );
            """)

    @contextmanager
    def _connect(self):
        db = sqlite3.connect(self.path, timeout=5.0, isolation_level=None)
        try:
            db.row_factory = sqlite3.Row
            db.execute("PRAGMA foreign_keys=ON")
            db.execute("PRAGMA busy_timeout=5000")
            with db:
                yield db
        finally:
            db.close()

    def enroll_device(self, device_id: str, token: str) -> None:
        """Local trusted provisioning only; no network endpoint."""
        if not DEVICE_RE.fullmatch(device_id) or len(token) < 32:
            raise ValueError("device ID invalid or credential too short (min 32 characters)")
        digest = hashlib.sha256(token.encode("utf-8")).hexdigest()
        with self._connect() as db:
            db.execute("BEGIN IMMEDIATE")
            if db.execute("SELECT 1 FROM devices WHERE device_id=?", (device_id,)).fetchone():
                raise ValueError("Device already provisioned; no silent credential replacement")
            db.execute("INSERT INTO devices(device_id,token_sha) VALUES(?,?)", (device_id, digest))
            db.commit()

    def authenticated(self, device_id: str, token: str) -> bool:
        if not DEVICE_RE.fullmatch(device_id) or not token or len(token) > 256:
            return False
        with self._connect() as db:
            row = db.execute("SELECT token_sha FROM devices WHERE device_id=?", (device_id,)).fetchone()
        return bool(row and hmac.compare_digest(
            row["token_sha"], hashlib.sha256(token.encode("utf-8")).hexdigest()))

    def add_approved_page(self, book_id: str, title: str, page: int,
                          text: str, *, approved: bool = False) -> None:
        """A trusted parent/admin import step; never exposed as an HTTP route."""
        if not BOOK_RE.fullmatch(book_id) or not 1 <= page <= 1000:
            raise ValueError("Invalid book/page")
        if not title.strip() or len(title) > 80 or not text.strip() or len(text) > 320:
            raise ValueError("Title or page text outside reviewed content bounds")
        with self._connect() as db:
            db.execute("BEGIN IMMEDIATE")
            db.execute("INSERT INTO books(book_id,title) VALUES(?,?) "
                       "ON CONFLICT(book_id) DO UPDATE SET title=excluded.title",
                       (book_id, title.strip()))
            db.execute("INSERT INTO pages(book_id,page,text,approved) VALUES(?,?,?,?) "
                       "ON CONFLICT(book_id,page) DO UPDATE SET "
                       "text=excluded.text, approved=excluded.approved",
                       (book_id, page, text.strip(), int(approved)))
            db.commit()

    def page(self, book_id: str, page: int) -> dict[str, Any] | None:
        if not BOOK_RE.fullmatch(book_id) or not 1 <= page <= 1000:
            return None
        with self._connect() as db:
            row = db.execute(
                "SELECT b.book_id,b.title,p.page,p.text FROM pages p "
                "JOIN books b ON b.book_id=p.book_id "
                "WHERE p.book_id=? AND p.page=? AND p.approved=1",
                (book_id, page),
            ).fetchone()
        return dict(row) if row else None

    def snapshot(self, device_id: str) -> dict[str, Any]:
        with self._connect() as db:
            row = db.execute(
                "SELECT device_id,seq,version,interaction_count,active_book,"
                "active_page,mode,quiet,open_chat FROM devices WHERE device_id=?",
                (device_id,),
            ).fetchone()
        if row is None:
            raise ValueError("Unknown device")
        value = dict(row)
        value["quiet"] = bool(value["quiet"])
        value["open_chat_enabled"] = bool(value.pop("open_chat"))
        return value

    def apply_event(self, device_id: str, event: dict[str, Any]) -> dict[str, Any]:
        """Exactly-once server projection for a durable device event.

        A duplicate (same device/message_id/payload) returns its *original*
        version even after later events. Sequence gaps and changed retries do
        not mutate state or consume a device sequence number.
        """
        msg = event["message_id"]
        seq = event["seq"]
        action = event["payload"]["event"]
        digest = _hash_json(event)
        with self._connect() as db:
            db.execute("BEGIN IMMEDIATE")
            row = db.execute("SELECT * FROM devices WHERE device_id=?", (device_id,)).fetchone()
            if row is None:
                raise ValueError("Unknown device")
            prior = db.execute(
                "SELECT payload_sha,accepted_version,seq FROM event_log "
                "WHERE device_id=? AND message_id=?",
                (device_id, msg),
            ).fetchone()
            if prior:
                if prior["payload_sha"] != digest:
                    return _response(msg, False, row["version"], seq, "MESSAGE_ID_CONFLICT")
                return _response(msg, True, prior["accepted_version"], prior["seq"], "DUPLICATE")
            if seq != row["seq"] + 1:
                return _response(msg, False, row["version"], seq, "SEQUENCE_GAP_OR_REPLAY")
            if action not in EVENTS:
                return _response(msg, False, row["version"], seq, "UNKNOWN_EVENT")

            count, mode, book, page, quiet = (
                row["interaction_count"], row["mode"], row["active_book"],
                row["active_page"], bool(row["quiet"]),
            )
            if action == "PET_FEED":
                if quiet or count == MAX_UINT32:
                    return _response(msg, False, row["version"], seq, "ACTION_UNAVAILABLE")
                count += 1
            elif action == "STORY_OPEN":
                book = event["payload"].get("book_id")
                if quiet or not isinstance(book, str) or self.page(book, 1) is None:
                    return _response(msg, False, row["version"], seq, "BOOK_NOT_APPROVED")
                page, mode = 1, "STORY"
            elif action == "PAGE_NEXT":
                if mode != "STORY" or self.page(book, page + 1) is None:
                    return _response(msg, False, row["version"], seq, "PAGE_NOT_APPROVED")
                page += 1
            elif action == "STORY_STOP":
                if mode != "STORY":
                    return _response(msg, False, row["version"], seq, "NOT_IN_STORY")
                mode = "HOME"
            elif action == "QUIET_HOURS_START":
                quiet, mode = True, "QUIET"
            elif action == "QUIET_HOURS_END":
                quiet, mode = False, "HOME"
            elif action == "VOICE_STARTED":
                if quiet:
                    return _response(msg, False, row["version"], seq, "QUIET_HOURS")
                mode = "VOICE"
            elif action == "VOICE_ENDED":
                mode = "HOME" if not quiet else "QUIET"
            elif action == "PET_PICKUP" and quiet:
                return _response(msg, False, row["version"], seq, "QUIET_HOURS")

            version = row["version"] + 1
            db.execute(
                "UPDATE devices SET seq=?,version=?,interaction_count=?,active_book=?,"
                "active_page=?,mode=?,quiet=? WHERE device_id=?",
                (seq, version, count, book, page, mode, int(quiet), device_id),
            )
            db.execute(
                "INSERT INTO event_log(device_id,message_id,seq,payload_sha,accepted_version) "
                "VALUES(?,?,?,?,?)",
                (device_id, msg, seq, digest, version),
            )
            db.commit()
            return _response(msg, True, version, seq)
