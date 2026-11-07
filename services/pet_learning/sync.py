"""Server S2 (V5-05): device state snapshots, memory projection and clears.

V5_V6 plan §5.2 upstream snapshots, §5.3 commands, §5.4 tombstones, §7
memory APIs, §8 privacy. The device is the authority for memory; the server
keeps a read-only projection, accepts snapshots by monotonic generation, and
never lets a stale snapshot bring back memory the parent cleared.

Nothing here logs; audit rows carry operation type and device id only.
"""
from __future__ import annotations

import base64
import binascii
from datetime import datetime, timedelta, timezone
import hashlib
import hmac
import ipaddress
import json
import sqlite3
import time
from typing import Annotated, Any, Callable, Literal

from fastapi import APIRouter, Depends, Header, HTTPException, Query, Request
from fastapi.concurrency import run_in_threadpool
from fastapi.responses import JSONResponse
from pydantic import BaseModel, ConfigDict, Field, ValidationError

from . import commands
from .pairing import device_auth
from .auth import AuthError, CSRF_COOKIE, SESSION_COOKIE
from .db import connect, migrate
from .memory_codec import (CodecError, MILESTONES, Memory, decode_memory,
                           decode_parent_settings)
from .routers.parent import check_origin, client_ip

SCHEMA_VERSION = "2.0"
LEDGER_SCHEMAS = frozenset({3})
# Compact JSON at every schema maximum is 4440 bytes (test_worst_case_snapshot_fits_the_limit).
MAX_STATE_BYTES = 5120
MIN_INTERVAL = 10           # §9: state uploads at least 10 s apart per device
REGRESSION_WARNING = 3      # consecutive STALE snapshots -> "re-enroll" hint
CONFLICT_WARNING_SECONDS = 7 * 24 * 3600
AUDIT_RETENTION = 365 * commands.DAY_SECONDS
RETENTION_JOB = "s2_retention"
DEFAULT_DAYS = 30
AUDIT_COLLAPSE_SECONDS = 3600  # one row per repeated device rejection per hour
TOMBSTONED = frozenset({"pending", "rejected", "expired"})
NO_STORE = {"Cache-Control": "no-store"}
MAX_INVENTORY = 32
MAX_AWAITING = 8
MAX_RECEIPTS = 16


CommandId = Annotated[int, Field(ge=1, le=(1 << commands.ID_BITS) - 1)]


class DeviceModel(BaseModel):
    # Strict: no bools or floats for ints, no numeric strings, no extra keys.
    model_config = ConfigDict(extra="forbid", strict=True)


class PetView(DeviceModel):
    """kid_app::PetSnapshot (pet_controller.h), enum names in upper case."""
    mode: Literal["IDLE", "INTERACT", "STORY", "LISTENING", "SPEAKING", "QUIET"]
    mood: Literal["NEUTRAL", "HAPPY", "CURIOUS", "THINKING", "SLEEPY"]
    interaction_count: int = Field(ge=0, le=0xFFFFFFFF)
    active_page: int = Field(ge=1, le=0xFFFF)
    quiet: bool


class Battery(DeviceModel):
    pct: int = Field(ge=0, le=100)
    charging: bool


class InventoryItem(DeviceModel):
    kind: Literal["book", "soundpack", "game"]
    content_id: int = Field(ge=0, le=0xFFFF)
    revision: int = Field(ge=0, le=0xFFFF)
    header_crc: str = Field(pattern=r"^[0-9a-f]{8}$")


class Receipt(DeviceModel):
    command_id: CommandId
    result: Literal["applied", "rejected", "expired"]


class StateSnapshot(DeviceModel):
    schema_version: Literal["2.0"]
    generation: int = Field(ge=1, le=(1 << 63) - 1)
    ledger_schema: int = Field(ge=1, le=255)
    fw_version: str = Field(min_length=1, max_length=40, pattern=r"^[A-Za-z0-9._+-]+$")
    rtc_day: int = Field(ge=0, le=0xFFFFFFFF)  # local civil days since 1970-01-01; 0 = unknown
    pet: PetView
    memory_b64: str = Field(min_length=384, max_length=384)
    parent_settings_b64: str = Field(min_length=40, max_length=40)
    parent_settings_rev: int = Field(ge=0, le=0xFFFFFFFF)
    battery: Battery
    inventory: list[InventoryItem] = Field(max_length=MAX_INVENTORY)
    sd_free_kib: int | None = Field(default=None, ge=0, le=0xFFFFFFFF)
    awaiting_confirm: list[CommandId] = Field(max_length=MAX_AWAITING)
    applied_commands: list[Receipt] = Field(max_length=MAX_RECEIPTS)


class SyncError(Exception):
    def __init__(self, status: int, code: str) -> None:
        self.status = status
        self.code = code
        super().__init__(code)


def _b64(value: str) -> bytes:
    try:
        return base64.b64decode(value, validate=True)
    except (binascii.Error, ValueError):
        raise CodecError("BASE64_INVALID") from None


def _day_dict(day) -> dict[str, int]:
    return {"day": day.day, **day.counts(), "last_book": day.last_book, "last_game": day.last_game}


def memory_projection(memory: Memory) -> dict[str, Any]:
    """Structured ids and counts only; preference source event ids are dropped."""
    return {
        "today": _day_dict(memory.today),
        "yesterday": _day_dict(memory.yesterday),
        "growth": {"books_finished": memory.growth.books_finished,
                   "games_completed": memory.growth.games_completed,
                   "preferences_chosen": memory.growth.preferences_chosen,
                   "active_days": memory.growth.active_days,
                   "milestones": memory.growth.milestones},
        "likes": [{"slot": p.slot, "option": p.option} for p in memory.preferences],
        "recent_books": list(memory.recent_books),
    }


def _table_exists(db: sqlite3.Connection, name: str) -> bool:
    return db.execute("SELECT 1 FROM sqlite_master WHERE type='table' AND name=?",
                      (name,)).fetchone() is not None


class CatalogNames:
    """Reviewed display names (§7.3). Ids without a reviewed name are omitted.

    Reads S4's like_topics / like_options and book titles, from approved
    packages only (as ContentCatalog.like_names), when those tables exist.
    """

    def like(self, db: sqlite3.Connection, slot: int, option: int) -> tuple[str, str] | None:
        if not (_table_exists(db, "like_topics") and _table_exists(db, "like_options")):
            return None
        row = db.execute(
            "SELECT t.label, o.label FROM like_options o "
            "JOIN like_topics t ON t.package_id=o.package_id AND t.slot=o.slot "
            "JOIN content_packages k ON k.package_id=o.package_id "
            "WHERE o.slot=? AND o.option=? AND k.status='approved' "
            "ORDER BY k.package_id DESC LIMIT 1", (slot, option)).fetchone()
        return (row[0], row[1]) if row else None

    def book(self, db: sqlite3.Connection, content_id: int) -> str | None:
        if not _table_exists(db, "content_packages"):
            return None
        row = db.execute("SELECT title FROM content_packages WHERE kind='book' AND content_id=? "
                         "AND status='approved' ORDER BY revision DESC, package_id DESC LIMIT 1",
                         (content_id,)).fetchone()
        return row[0] if row else None


def civil_day(epoch_seconds: float, tz_name: str) -> int:
    """Device rtc_day convention: local calendar days since 1970-01-01."""
    try:
        from zoneinfo import ZoneInfo
        tz = ZoneInfo(tz_name)
    except Exception:  # no tzdata on this host: China has no DST
        tz = timezone(timedelta(hours=8))
    local = datetime.fromtimestamp(epoch_seconds, tz).date()
    return (local - datetime(1970, 1, 1).date()).days


class StateSync:
    def __init__(self, filename, *, clock: Callable[[], float] = time.time,
                 names: CatalogNames | None = None, tz_name: str = "Asia/Shanghai") -> None:
        self.filename = filename
        self.clock = clock
        self.names = names or CatalogNames()
        self.tz_name = tz_name
        # S3 conservative-merge hook: fn(db, device_id, ParentSettings, rev, now), run inside
        # the snapshot transaction after the device row is stored.
        self.hooks: list[Callable[..., None]] = []
        migrate(filename)

    # -- device ---------------------------------------------------------

    def ingest(self, device_id: str, body: bytes) -> dict[str, Any]:
        now = int(self.clock())
        try:
            raw = json.loads(body)
        except (ValueError, UnicodeDecodeError, RecursionError):
            raise SyncError(422, "INVALID_STATE") from None
        if not isinstance(raw, dict) or raw.get("schema_version") != SCHEMA_VERSION:
            raise SyncError(422, "UNSUPPORTED_SCHEMA_VERSION")
        try:
            snap = StateSnapshot.model_validate(raw)
        except ValidationError:
            raise SyncError(422, "INVALID_STATE") from None
        if snap.ledger_schema not in LEDGER_SCHEMAS:
            raise SyncError(422, "UNSUPPORTED_LEDGER_SCHEMA")
        try:
            memory = decode_memory(_b64(snap.memory_b64))
            settings = decode_parent_settings(_b64(snap.parent_settings_b64))
        except CodecError as error:
            with connect(self.filename) as db:
                db.execute("BEGIN IMMEDIATE")
                commands.audit(db, now, "device", "state.reject", device_id, error.code,
                               collapse=AUDIT_COLLAPSE_SECONDS)
                db.commit()
            raise SyncError(422, error.code) from None
        # The idempotency hash covers only ledger-committed content. Battery,
        # inventory, clock, awaiting_confirm and receipts change between
        # uploads without a ledger commit, so they refresh the stored row
        # under the same generation instead of counting as a conflict.
        ledger = {"ledger_schema": snap.ledger_schema, "pet": snap.pet.model_dump(),
                  "memory_b64": snap.memory_b64, "parent_settings_b64": snap.parent_settings_b64,
                  "parent_settings_rev": snap.parent_settings_rev}
        canonical = json.dumps(ledger, sort_keys=True, separators=(",", ":"))
        state_sha = hashlib.sha256(canonical.encode("utf-8")).hexdigest()
        full = json.dumps(snap.model_dump(), sort_keys=True, separators=(",", ":"))
        upload_sha = hashlib.sha256(full.encode("utf-8")).hexdigest()

        quick = self._read_only(device_id, snap, upload_sha, now)
        if quick is not None:
            return quick
        with connect(self.filename) as db:
            db.execute("BEGIN IMMEDIATE")
            self._expire(db, now, device_id)
            row = db.execute("SELECT * FROM device_state WHERE device_id=?", (device_id,)).fetchone()
            acked: list[int] = []
            if row is None or snap.generation > row["generation"]:
                if row is not None and now - row["received_at"] < MIN_INTERVAL:
                    db.commit()
                    raise SyncError(429, "STATE_RATE_LIMITED")
                result = "accepted"
                acked = self._accept(db, device_id, snap, memory, settings, state_sha, now,
                                     upload_sha=upload_sha)
            elif snap.generation == row["generation"]:
                if hmac.compare_digest(row["state_sha"], state_sha):
                    result = "duplicate"
                    acked = self._accept(db, device_id, snap, memory, settings, state_sha, now,
                                         project=False, upload_sha=upload_sha)
                else:
                    result = "generation_conflict"
                    db.execute("UPDATE device_state SET conflict_at=? WHERE device_id=?", (now, device_id))
                    commands.audit(db, now, "device", "state.conflict", device_id, "rejected",
                                   collapse=AUDIT_COLLAPSE_SECONDS)
            else:
                result = "stale"
                streak = row["stale_streak"] + 1
                db.execute("UPDATE device_state SET stale_streak=?,stale_at=? WHERE device_id=?",
                           (streak, now, device_id))
                if streak == REGRESSION_WARNING:
                    commands.audit(db, now, "device", "state.regressed", device_id, "warning")
            stored = db.execute("SELECT generation FROM device_state WHERE device_id=?",
                                (device_id,)).fetchone()[0]
            response = {"result": result, "generation": stored,
                        "server_rev": commands.server_rev(db, device_id),
                        "acked_commands": acked,
                        "commands": commands.deliverable(db, device_id, now)}
            self._maybe_retain(db, now)
            db.commit()
        return response

    def _read_only(self, device_id: str, snap: StateSnapshot, upload_sha: str,
                   now: int) -> dict[str, Any] | None:
        """Answer retries without a write transaction: an identical re-upload
        with nothing new to deliver, a repeated stale upload, or a new
        generation inside the rate limit. None = take the write path."""
        with connect(self.filename) as db:
            row = db.execute("SELECT generation, upload_sha, received_at, stale_at FROM device_state "
                             "WHERE device_id=?", (device_id,)).fetchone()
            if row is None:
                return None
            if snap.generation > row["generation"]:
                if now - row["received_at"] < MIN_INTERVAL:
                    raise SyncError(429, "STATE_RATE_LIMITED")
                return None
            if snap.generation == row["generation"]:
                if not hmac.compare_digest(row["upload_sha"], upload_sha) or db.execute(
                        "SELECT 1 FROM commands WHERE device_id=? AND state='pending' "
                        "AND (delivered_at IS NULL OR expires_at<=?)", (device_id, now)).fetchone():
                    return None
                result, acked = "duplicate", [r.command_id for r in snap.applied_commands]
            elif now - (row["stale_at"] or 0) < MIN_INTERVAL:
                result, acked = "stale", []
            else:
                return None
            return {"result": result, "generation": row["generation"],
                    "server_rev": commands.server_rev(db, device_id), "acked_commands": acked,
                    "commands": commands.deliverable(db, device_id, now, mark=False)}

    def _accept(self, db, device_id, snap: StateSnapshot, memory: Memory, settings,
                state_sha: str, now: int, *, upload_sha: str,
                project: bool = True) -> list[int]:
        receipts = [(r.command_id, r.result) for r in snap.applied_commands]
        self._clear_results(db, commands.apply_receipts(db, device_id, receipts, now), snap.generation)
        db.execute(
            "INSERT INTO device_state(device_id,generation,state_sha,received_at,ledger_schema,fw_version,"
            "rtc_day,pet_json,battery_pct,charging,inventory_json,sd_free_kib,parent_settings_json,"
            "parent_settings_rev,awaiting_json,temperament,upload_sha,stale_streak) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,0) ON CONFLICT(device_id) DO UPDATE SET "
            "generation=excluded.generation,state_sha=excluded.state_sha,received_at=excluded.received_at,"
            "upload_sha=excluded.upload_sha,"
            "ledger_schema=excluded.ledger_schema,fw_version=excluded.fw_version,rtc_day=excluded.rtc_day,"
            "pet_json=excluded.pet_json,battery_pct=excluded.battery_pct,charging=excluded.charging,"
            "inventory_json=excluded.inventory_json,sd_free_kib=excluded.sd_free_kib,"
            "parent_settings_json=excluded.parent_settings_json,"
            "parent_settings_rev=excluded.parent_settings_rev,awaiting_json=excluded.awaiting_json,"
            "temperament=excluded.temperament,stale_streak=0",
            (device_id, snap.generation, state_sha, now, snap.ledger_schema, snap.fw_version,
             snap.rtc_day, json.dumps(snap.pet.model_dump(), sort_keys=True), snap.battery.pct,
             int(snap.battery.charging), json.dumps([i.model_dump() for i in snap.inventory]),
             snap.sd_free_kib, json.dumps(settings.as_dict(), sort_keys=True),
             snap.parent_settings_rev, json.dumps(snap.awaiting_confirm), memory.trait, upload_sha))
        for hook in self.hooks:
            hook(db, device_id, settings, snap.parent_settings_rev, now)
        if project and not self._tombstoned(db, device_id, snap.generation):
            self._project(db, device_id, snap.generation, memory)
        return [r.command_id for r in snap.applied_commands]

    def _tombstoned(self, db, device_id: str, generation: int | None = None) -> bool:
        """§5.4: memory is dropped while a clear is unconfirmed or refused, and
        for the snapshot carrying the applied receipt; accepted again after."""
        row = db.execute("SELECT clear_state, applied_generation FROM memory_clears WHERE device_id=?",
                         (device_id,)).fetchone()
        if row is None:
            return False
        if row["clear_state"] in TOMBSTONED:
            return True
        return (row["clear_state"] == "applied" and generation is not None
                and generation <= row["applied_generation"])

    def _project(self, db, device_id: str, generation: int, memory: Memory) -> None:
        previous = db.execute("SELECT memory_json FROM memory_current WHERE device_id=?",
                              (device_id,)).fetchone()
        # Daily archive: the previous "today" once the device has moved on,
        # then the device's own "yesterday" (more complete for that day).
        days = []
        if previous is not None:
            old_today = json.loads(previous[0])["today"]
            if old_today["day"] and old_today["day"] < memory.today.day:
                days.append(old_today)
        if memory.yesterday.day:
            days.append(_day_dict(memory.yesterday))
        for d in days:
            if any(d[k] for k in ("feeds", "greetings", "books", "games", "choices")):
                db.execute("INSERT OR REPLACE INTO daily_summaries VALUES(?,?,?,?,?,?,?,?,?)",
                           (device_id, d["day"], d["feeds"], d["greetings"], d["books"], d["games"],
                            d["choices"], d["last_book"], d["last_game"]))
        epoch = db.execute("SELECT clear_epoch FROM memory_clears WHERE device_id=?",
                           (device_id,)).fetchone()
        db.execute("INSERT OR REPLACE INTO memory_current VALUES(?,?,?,?)",
                   (device_id, generation, json.dumps(memory_projection(memory), sort_keys=True),
                    epoch[0] if epoch else 0))

    def _clear_results(self, db, changed: list[dict], generation: int | None) -> None:
        for command in changed:
            if command["kind"] != "MEMORY_CLEAR":
                continue
            if command["state"] == "applied":
                # The device memory is now empty. Drop anything a resumed sync
                # wrote back in the meantime (late confirmation after expiry).
                db.execute("DELETE FROM memory_current WHERE device_id=?", (command["device_id"],))
                db.execute("DELETE FROM daily_summaries WHERE device_id=?", (command["device_id"],))
            # After "resume sync" (clear_state 'none') only an applied clear
            # changes anything: a late refusal must not freeze sync again.
            db.execute("UPDATE memory_clears SET clear_state=?, applied_generation=? "
                       "WHERE device_id=? AND command_id=? AND (clear_state!='none' OR ?='applied')",
                       (command["state"], generation if command["state"] == "applied" else None,
                        command["device_id"], command["command_id"], command["state"]))

    def _expire(self, db, now: int, device_id: str | None = None) -> None:
        # Server timeouts carry no snapshot generation; a later device
        # "applied" receipt still lifts the tombstone via apply_receipts.
        self._clear_results(db, commands.expire_overdue(db, now, device_id), None)

    def device_commands(self, device_id: str) -> dict[str, Any]:
        now = int(self.clock())
        with connect(self.filename) as db:
            db.execute("BEGIN IMMEDIATE")
            self._expire(db, now, device_id)
            result = {"server_rev": commands.server_rev(db, device_id),
                      "commands": commands.deliverable(db, device_id, now)}
            db.commit()
        return result

    # -- parent ---------------------------------------------------------

    def _require_device(self, db, device_id: str) -> None:
        if not db.execute("SELECT 1 FROM devices WHERE device_id=?", (device_id,)).fetchone():
            raise SyncError(404, "DEVICE_NOT_FOUND")

    def _clear_view(self, db, device_id: str, awaiting: list[int]) -> dict[str, Any]:
        row = db.execute("SELECT * FROM memory_clears WHERE device_id=?", (device_id,)).fetchone()
        if row is None:
            return {"state": "none", "clear_epoch": 0, "command": None}
        command = commands.get(db, row["command_id"]) if row["command_id"] else None
        return {"state": row["clear_state"], "clear_epoch": row["clear_epoch"],
                "requested_at": row["requested_at"],
                "device_memory_not_cleared": row["clear_state"] in {"rejected", "expired"},
                "command": commands.status(command, awaiting)}

    def memories(self, device_id: str, days: int = DEFAULT_DAYS) -> dict[str, Any]:
        now = int(self.clock())
        with connect(self.filename) as db:
            db.execute("BEGIN IMMEDIATE")
            self._require_device(db, device_id)
            self._expire(db, now, device_id)
            state = db.execute("SELECT * FROM device_state WHERE device_id=?", (device_id,)).fetchone()
            awaiting = json.loads(state["awaiting_json"]) if state else []
            current = db.execute("SELECT * FROM memory_current WHERE device_id=?", (device_id,)).fetchone()
            memory = None
            if current is not None:
                m = json.loads(current["memory_json"])
                likes = []
                for like in m["likes"]:
                    names = self.names.like(db, like["slot"], like["option"])
                    likes.append({**like, "topic": names[0] if names else None,
                                  "label": names[1] if names else None})
                memory = {
                    "generation": current["generation"],
                    "likes": likes,
                    "growth": {**m["growth"], "milestones": [
                        name for bit, name in enumerate(MILESTONES) if m["growth"]["milestones"] >> bit & 1]},
                    "today": m["today"],
                    "recent_books": [{"content_id": b, "title": self.names.book(db, b)}
                                     for b in m["recent_books"]],
                }
            history = [dict(r) for r in db.execute(
                "SELECT day,feeds,greetings,books,games,choices,last_book,last_game FROM daily_summaries "
                "WHERE device_id=? ORDER BY day DESC LIMIT ?", (device_id, days))]
            warnings = []
            if state is not None and state["stale_streak"] >= REGRESSION_WARNING:
                warnings.append("GENERATION_REGRESSED")
            if state is not None and (state["conflict_at"] or 0) > now - CONFLICT_WARNING_SECONDS:
                warnings.append("GENERATION_CONFLICT")
            result = {
                "device_id": device_id,
                "trait": state["temperament"] if state else None,
                "last_snapshot_at": state["received_at"] if state else None,
                "warnings": warnings,
                "memory_clear": self._clear_view(db, device_id, awaiting),
                "memory": memory,
                "days": history,
            }
            db.commit()
        return result

    def clear(self, device_id: str, ip: str | None) -> dict[str, Any]:
        """Delete the server copy now (memory + daily summaries) and queue a
        MEMORY_CLEAR request the parent must confirm on the device (D5)."""
        now = int(self.clock())
        with connect(self.filename) as db:
            db.execute("BEGIN IMMEDIATE")
            self._require_device(db, device_id)
            self._expire(db, now, device_id)
            db.execute("DELETE FROM memory_current WHERE device_id=?", (device_id,))
            db.execute("DELETE FROM daily_summaries WHERE device_id=?", (device_id,))
            row = db.execute("SELECT * FROM memory_clears WHERE device_id=?", (device_id,)).fetchone()
            if row is not None and row["clear_state"] == "pending":
                commands.audit(db, now, "parent", "memory.clear", device_id, "already_pending", ip)
            else:
                epoch = (row["clear_epoch"] if row else 0) + 1
                command = commands.enqueue(db, device_id, "MEMORY_CLEAR", {"clear_epoch": epoch}, now, ip=ip)
                db.execute("INSERT OR REPLACE INTO memory_clears VALUES(?,?,?,'pending',?,NULL)",
                           (device_id, epoch, command["command_id"], now))
                commands.audit(db, now, "parent", "memory.clear", device_id, "requested", ip)
            state = db.execute("SELECT awaiting_json FROM device_state WHERE device_id=?",
                               (device_id,)).fetchone()
            view = self._clear_view(db, device_id, json.loads(state[0]) if state else [])
            db.commit()
        return {"status": "deleted", "memory_clear": view}

    def resume(self, device_id: str, ip: str | None) -> dict[str, Any]:
        """After the device refused or never confirmed a clear: lift the
        tombstone so the next snapshot writes the device's memory again."""
        now = int(self.clock())
        with connect(self.filename) as db:
            db.execute("BEGIN IMMEDIATE")
            self._require_device(db, device_id)
            self._expire(db, now, device_id)
            row = db.execute("SELECT clear_state FROM memory_clears WHERE device_id=?", (device_id,)).fetchone()
            if row is None or row[0] not in {"rejected", "expired"}:
                db.commit()
                raise SyncError(409, "CLEAR_PENDING" if row and row[0] == "pending" else "NOTHING_TO_RESUME")
            db.execute("UPDATE memory_clears SET clear_state='none' WHERE device_id=?", (device_id,))
            commands.audit(db, now, "parent", "memory.resume", device_id, "success", ip)
            db.commit()
        return {"status": "resumed"}

    # -- voice ----------------------------------------------------------

    def summary(self, device_id: str) -> dict[str, Any]:
        """§7.3 minimal summary: reviewed names only; no ids, dates, notes or text."""
        current_day = civil_day(self.clock(), self.tz_name)
        with connect(self.filename) as db:
            self._require_device(db, device_id)
            result: dict[str, Any] = {}
            if _table_exists(db, "profiles"):  # S3 owns profiles
                row = db.execute("SELECT pet_name FROM profiles WHERE device_id=? ORDER BY rev DESC LIMIT 1",
                                 (device_id,)).fetchone()
                if row and row[0]:
                    result["name"] = row[0]
            state = db.execute("SELECT temperament FROM device_state WHERE device_id=?", (device_id,)).fetchone()
            result["trait"] = state[0] if state else "gentle"
            current = db.execute("SELECT memory_json FROM memory_current WHERE device_id=?",
                                 (device_id,)).fetchone()
            if current is None or self._tombstoned(db, device_id):
                return result
            m = json.loads(current[0])
            likes = []
            for like in m["likes"]:
                names = self.names.like(db, like["slot"], like["option"])
                if names:
                    likes.append({"topic": names[0], "option": names[1]})
            today = m["today"]
            stale = current_day and today["day"] and today["day"] != current_day
            result["likes"] = likes
            result["today"] = {"feed": 0 if stale else today["feeds"],
                               "books": 0 if stale else today["books"],
                               "games": 0 if stale else today["games"]}
            title = self.names.book(db, m["recent_books"][0]) if m["recent_books"] else None
            if title:
                result["last_book"] = title
        return result

    # -- retention (D6) -------------------------------------------------

    def run_retention(self, now: int | None = None) -> dict[str, int]:
        """Operational data only. Never touches memory_current,
        daily_summaries, memory_clears or device_state (kept until the parent
        clears memory or deletes the device)."""
        now = int(self.clock()) if now is None else now
        with connect(self.filename) as db:
            db.execute("BEGIN IMMEDIATE")
            counts = self._retain(db, now)
            db.commit()
        return counts

    def _retain(self, db, now: int) -> dict[str, int]:
        self._expire(db, now)
        counts = {"commands": commands.purge_completed(db, now),
                  "audit_log": db.execute("DELETE FROM audit_log WHERE at<=?",
                                          (now - AUDIT_RETENTION,)).rowcount}
        db.execute("INSERT OR REPLACE INTO maintenance_runs VALUES(?,?)", (RETENTION_JOB, now))
        return counts

    def _maybe_retain(self, db, now: int) -> None:
        """Daily schedule piggybacked on device traffic (S7 may add a timer)."""
        row = db.execute("SELECT last_run_at FROM maintenance_runs WHERE job=?", (RETENTION_JOB,)).fetchone()
        if row is None or now - row[0] >= commands.DAY_SECONDS:
            self._retain(db, now)


async def _read_limited(request: Request) -> bytes:
    declared = request.headers.get("content-length")
    if declared is not None and (not declared.isdigit() or int(declared) > MAX_STATE_BYTES):
        raise HTTPException(413, "STATE_TOO_LARGE")
    body = b""
    async for chunk in request.stream():
        body += chunk
        if len(body) > MAX_STATE_BYTES:
            raise HTTPException(413, "STATE_TOO_LARGE")
    return body


def create_router(app, store) -> APIRouter:
    settings = app.state.settings
    sync = StateSync(store.path, tz_name=getattr(settings, "timezone", "Asia/Shanghai"))
    app.state.state_sync = sync
    router = APIRouter()

    def require_parent(request: Request, *, write: bool) -> None:
        check_origin(request)
        try:
            request.app.state.parent_auth.session(
                request.cookies.get(SESSION_COOKIE),
                # "" forces the CSRF check on writes even without the header.
                csrf=(request.headers.get("x-csrf-token") or "") if write else None,
                csrf_cookie=request.cookies.get(CSRF_COOKIE))
        except AuthError as error:
            raise HTTPException(error.status, error.code, headers=NO_STORE) from None

    def call(fn, *args):
        try:
            return fn(*args)
        except SyncError as error:
            raise HTTPException(error.status, error.code, headers=NO_STORE) from None

    # Device routes share S1's dependency: the token authenticates exactly the
    # path's device id; revoked and pending devices are refused. It is a sync
    # def, so FastAPI runs it in the threadpool.
    @router.put("/v1/devices/{device_id}/state")
    async def put_state(request: Request, device_id: str = Depends(device_auth)) -> dict:
        body = await _read_limited(request)
        try:
            return await run_in_threadpool(sync.ingest, device_id, body)
        except SyncError as error:
            headers = {"Retry-After": str(MIN_INTERVAL)} if error.status == 429 else None
            raise HTTPException(error.status, error.code, headers=headers) from None

    @router.get("/v1/devices/{device_id}/commands")
    def get_commands(device_id: str = Depends(device_auth)) -> dict:
        return sync.device_commands(device_id)

    @router.get("/v1/parent/devices/{device_id}/memories")
    def get_memories(device_id: str, request: Request,
                     days: int = Query(default=DEFAULT_DAYS, ge=1, le=366)):
        require_parent(request, write=False)
        return JSONResponse(call(sync.memories, device_id, days), headers=NO_STORE)

    @router.delete("/v1/parent/devices/{device_id}/memories")
    def delete_memories(device_id: str, request: Request):
        require_parent(request, write=True)
        return JSONResponse(call(sync.clear, device_id, client_ip(request)), headers=NO_STORE)

    @router.post("/v1/parent/devices/{device_id}/memories/resume")
    def resume_memories(device_id: str, request: Request):
        require_parent(request, write=True)
        return JSONResponse(call(sync.resume, device_id, client_ip(request)), headers=NO_STORE)

    @router.get("/v1/voice/{device_id}/summary")
    def voice_summary(device_id: str, request: Request,
                      authorization: str | None = Header(default=None)) -> dict:
        # Same checks, in the same order, as /v1/voice/*/turn: the voice token, then
        # the internal-only rule (§3.3 N6, §9) — the peer must be in
        # KID_PET_VOICE_ALLOWED_PEERS, never the public proxy, with no forwarding header.
        scheme, _, token = (authorization or "").partition(" ")
        if scheme != "Bearer" or not request.app.state.voice_turns.authorized(token):
            raise HTTPException(401, "VOICE_AUTH_REQUIRED")
        settings = request.app.state.settings
        try:
            address = ipaddress.ip_address(request.client.host if request.client else "")
        except ValueError:
            address = None
        if (address is None or str(address) == settings.trusted_proxy
                or not any(address in net for net in settings.voice_peer_networks())
                or any(name in request.headers for name in
                       ("x-forwarded-for", "forwarded", "x-real-ip"))):
            raise HTTPException(403, "VOICE_INTERNAL_ONLY")
        return call(sync.summary, device_id)

    return router
