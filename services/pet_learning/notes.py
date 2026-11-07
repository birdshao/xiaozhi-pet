"""Server S3 (D9, plan §7.5): parent notes, encrypted at rest.

Isolation contract: only the parent panel router (profile.py) imports this
module. The voice, summary, device, sync and command code never do, and
nothing here logs. The note, nickname and birthday are stored as one Fernet
token bound to the device id; the key lives in KID_PET_NOTES_KEY, outside the
database and its backups. Without the key notes are unreadable and
unwritable: there is no plaintext fallback.
"""
from __future__ import annotations

import json
import re
import sqlite3
from typing import Any

from cryptography.fernet import InvalidToken

from . import commands

MAX_NOTE_CHARS = 500
MAX_NICKNAME_CHARS = 20
_DAYS_IN_MONTH = (31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31)  # Feb 29 allowed: month-day only
_BIRTHDAY = re.compile(r"(0[1-9]|1[0-2])-(0[1-9]|[12][0-9]|3[01])")
_CONTROL = re.compile(r"[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]")


class NotesError(Exception):
    def __init__(self, status: int, code: str) -> None:
        self.status = status
        self.code = code
        super().__init__(code)


def validate(nickname: str | None, birthday: str | None, note: str | None) -> None:
    for value, limit in ((nickname, MAX_NICKNAME_CHARS), (note, MAX_NOTE_CHARS)):
        if value is not None and (len(value) > limit or _CONTROL.search(value)):
            raise NotesError(422, "NOTES_INVALID")
    if birthday is not None:
        match = _BIRTHDAY.fullmatch(birthday)
        if not match or int(match[2]) > _DAYS_IN_MONTH[int(match[1]) - 1]:
            raise NotesError(422, "NOTES_INVALID")  # month-day only, never a year


class ParentNotes:
    def __init__(self, settings) -> None:
        self.settings = settings

    def _cipher(self):
        try:
            return self.settings.notes_cipher()
        except ValueError:
            raise NotesError(503, "NOTES_KEY_MISSING") from None

    def get(self, db: sqlite3.Connection, device_id: str) -> dict[str, Any]:
        cipher = self._cipher()
        row = db.execute("SELECT payload_enc, updated_at FROM parent_notes WHERE device_id=?",
                         (device_id,)).fetchone()
        if row is None:
            return {"exists": False, "nickname": None, "birthday": None, "note": None, "updated_at": None}
        try:
            data = json.loads(cipher.decrypt(row[0].encode("ascii")))
        except (InvalidToken, ValueError):
            raise NotesError(500, "NOTES_UNREADABLE") from None
        if data.get("device_id") != device_id:  # a row moved to another device never decrypts
            raise NotesError(500, "NOTES_UNREADABLE")
        return {"exists": True, "nickname": data.get("nickname"), "birthday": data.get("birthday"),
                "note": data.get("note"), "updated_at": row[1]}

    def put(self, db: sqlite3.Connection, device_id: str, nickname: str | None, birthday: str | None,
            note: str | None, now: int, ip: str | None) -> dict[str, Any]:
        validate(nickname, birthday, note)
        cipher = self._cipher()
        if nickname is None and birthday is None and note is None:
            raise NotesError(422, "NOTES_INVALID")
        blob = json.dumps({"device_id": device_id, "nickname": nickname, "birthday": birthday,
                           "note": note}, ensure_ascii=False)
        db.execute("INSERT INTO parent_notes(device_id,payload_enc,updated_at) VALUES(?,?,?) "
                   "ON CONFLICT(device_id) DO UPDATE SET payload_enc=excluded.payload_enc,"
                   "updated_at=excluded.updated_at",
                   (device_id, cipher.encrypt(blob.encode("utf-8")).decode("ascii"), now))
        commands.audit(db, now, "parent", "notes.update", device_id, "success", ip)  # type only
        return {"exists": True, "nickname": nickname, "birthday": birthday, "note": note, "updated_at": now}

    def delete(self, db: sqlite3.Connection, device_id: str, now: int, ip: str | None) -> None:
        db.execute("DELETE FROM parent_notes WHERE device_id=?", (device_id,))
        commands.audit(db, now, "parent", "notes.delete", device_id, "success", ip)
