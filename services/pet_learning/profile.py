"""Server S3 (V5-06): parent initialisation, profile, policy and notes routes.

V5_V6 plan §7.1/§7.2/§7.5. Every route here is parent-only (session cookie;
writes also need the CSRF token and an Origin check) and every mutation is
audited by operation type. Personality changes go through S2's command
channel as SET_TEMPERAMENT, whose on-device parent confirmation (D5) is fixed
by kind in commands.py; voice pack and settings use SELECT_VOICE_PACK and
SET_PARENT_SETTINGS (no confirmation, D13).

The parent notes module is imported only here and only to serve /notes.
"""
from __future__ import annotations

import json
import re
import sqlite3
import time
from typing import Any, Callable

from fastapi import APIRouter, Depends, HTTPException, Request
from fastapi.responses import JSONResponse
from pydantic import Field, StrictBool, StrictInt

from . import commands, policy
from .auth import AuthError, CSRF_COOKIE, SESSION_COOKIE
from .db import connect, migrate
from .memory_codec import TEMPERAMENTS, ParentSettings
from .models import StrictModel
from .notes import MAX_NICKNAME_CHARS, MAX_NOTE_CHARS, NotesError, ParentNotes, validate as validate_notes
from .routers.parent import check_origin, client_ip

NO_STORE = {"Cache-Control": "no-store"}
PET_NAME = re.compile(r"(?:[一-鿿]{1,6}|[A-Za-z]{1,12})")  # §6.4: no digits, symbols or links
DEVICE_ID = re.compile(r"[A-Za-z0-9:_.-]{3,64}")


class ProfileError(Exception):
    def __init__(self, status: int, code: str) -> None:
        self.status = status
        self.code = code
        super().__init__(code)


class Reauth(StrictModel):
    password: str = Field(min_length=1, max_length=256, repr=False)
    otp: str = Field(min_length=6, max_length=6, pattern=r"^[0-9]{6}$", repr=False)


class InitBody(StrictModel):
    pet_name: str = Field(max_length=12)
    temperament: str = Field(max_length=16)
    voice_pack_id: StrictInt | None = Field(default=None, ge=0, le=0xFFFF)


class ProfileBody(StrictModel):
    base_rev: StrictInt = Field(ge=0)
    pet_name: str | None = Field(default=None, max_length=12)
    temperament: str | None = Field(default=None, max_length=16)
    voice_pack_id: StrictInt | None = Field(default=None, ge=0, le=0xFFFF)


class SettingsChanges(StrictModel):
    wake_min: StrictInt | None = Field(default=None, ge=0, le=1439)
    evening_min: StrictInt | None = Field(default=None, ge=0, le=1439)
    quiet_start_min: StrictInt | None = Field(default=None, ge=0, le=1439)
    quiet_end_min: StrictInt | None = Field(default=None, ge=0, le=1439)
    nap_enabled: StrictBool | None = None
    nap_start_min: StrictInt | None = Field(default=None, ge=0, le=1439)
    nap_end_min: StrictInt | None = Field(default=None, ge=0, le=1439)
    volume_cap_pct: StrictInt | None = Field(default=None, ge=0, le=100)
    quiet_volume_pct: StrictInt | None = Field(default=None, ge=0, le=100)
    motion_enabled: StrictBool | None = None


class PolicyBody(StrictModel):
    base_rev: StrictInt = Field(ge=0)
    settings: SettingsChanges | None = None
    open_chat: StrictBool | None = None
    reauth: Reauth | None = None


class NotesBody(StrictModel):
    nickname: str | None = Field(default=None, max_length=MAX_NICKNAME_CHARS, repr=False)
    birthday: str | None = Field(default=None, max_length=5, repr=False)
    note: str | None = Field(default=None, max_length=MAX_NOTE_CHARS, repr=False)
    reauth: Reauth


class NotesDeleteBody(StrictModel):
    reauth: Reauth


def _status(db: sqlite3.Connection, device_id: str, kind: str, awaiting: list[int]) -> dict | None:
    row = db.execute("SELECT * FROM commands WHERE device_id=? AND kind=? ORDER BY seq DESC LIMIT 1",
                     (device_id, kind)).fetchone()
    return commands.status(dict(row), awaiting) if row else None


def _inventory_packs(db: sqlite3.Connection, device_id: str) -> list[dict[str, Any]]:
    """Voice packs the device holds that the server catalogue has approved
    (same revision and header CRC, so a swapped file never qualifies, §7.2)."""
    state = db.execute("SELECT inventory_json FROM device_state WHERE device_id=?", (device_id,)).fetchone()
    packs = []
    for item in (json.loads(state[0]) if state else []):
        if item["kind"] != "soundpack":
            continue
        row = db.execute("SELECT title FROM content_packages WHERE kind='soundpack' AND content_id=? "
                         "AND revision=? AND header_crc=? AND status='approved' ORDER BY package_id DESC LIMIT 1",
                         (item["content_id"], item["revision"], int(item["header_crc"], 16))).fetchone()
        if row:
            packs.append({"content_id": item["content_id"], "revision": item["revision"], "title": row[0]})
    return packs


class ProfileService:
    def __init__(self, filename, settings, sync, *, clock: Callable[[], float] = time.time) -> None:
        self.filename = filename
        self.sync = sync
        self.clock = clock
        self.notes = ParentNotes(settings)
        migrate(filename)

    def _device(self, db, device_id: str, *, write: bool = False) -> None:
        row = db.execute("SELECT status FROM devices WHERE device_id=?", (device_id,)).fetchone()
        if row is None:
            raise ProfileError(404, "DEVICE_NOT_FOUND")
        if write and row[0] != "active":
            raise ProfileError(409, "DEVICE_NOT_ACTIVE")

    def _expire(self, db, now: int, device_id: str) -> None:
        # Same housekeeping as S2: statuses shown to the parent must not lag the clock.
        self.sync._expire(db, now, device_id)

    # -- profile --------------------------------------------------------

    def _profile_view(self, db, device_id: str) -> dict[str, Any]:
        row = db.execute("SELECT * FROM profiles WHERE device_id=?", (device_id,)).fetchone()
        state = db.execute("SELECT awaiting_json, temperament FROM device_state WHERE device_id=?",
                           (device_id,)).fetchone()
        awaiting = json.loads(state[0]) if state else []
        return {"initialized": row is not None, "rev": row["rev"] if row else 0,
                "pet_name": row["pet_name"] if row else None,
                "temperament": row["temperament"] if row else None,  # requested by the parent
                "temperament_confirmed": row["temperament_confirmed"] if row else None,  # confirmed on the device
                "voice_pack_id": row["voice_pack_id"] if row else None,
                "device_temperament": state[1] if state else None,
                "temperaments": list(TEMPERAMENTS),
                "available_voice_packs": _inventory_packs(db, device_id),
                "commands": {"temperament": _status(db, device_id, "SET_TEMPERAMENT", awaiting),
                             "voice_pack": _status(db, device_id, "SELECT_VOICE_PACK", awaiting)}}

    def get_profile(self, device_id: str) -> dict[str, Any]:
        now = int(self.clock())
        with connect(self.filename) as db:
            db.execute("BEGIN IMMEDIATE")
            self._device(db, device_id)
            self._expire(db, now, device_id)
            view = self._profile_view(db, device_id)
            db.commit()
        return view

    @staticmethod
    def _check_name(name: str) -> None:
        if not PET_NAME.fullmatch(name):
            raise ProfileError(422, "PET_NAME_INVALID")

    @staticmethod
    def _check_temperament(value: str) -> None:
        if value not in TEMPERAMENTS:
            raise ProfileError(422, "TEMPERAMENT_INVALID")

    @staticmethod
    def _check_voice_pack(db, device_id: str, content_id: int) -> None:
        if content_id not in {p["content_id"] for p in _inventory_packs(db, device_id)}:
            raise ProfileError(409, "VOICE_PACK_UNAVAILABLE")

    def init(self, device_id: str, body: InitBody, ip: str | None, actor: str = "parent") -> dict[str, Any]:
        """§7.2: profiles(rev=1), then SET_TEMPERAMENT (confirmed on the device) and
        SELECT_VOICE_PACK. The device has no personality yet, so the temperament
        command is always sent, even if it equals the firmware default."""
        self._check_name(body.pet_name)
        self._check_temperament(body.temperament)
        now = int(self.clock())
        with connect(self.filename) as db:
            db.execute("BEGIN IMMEDIATE")
            self._device(db, device_id, write=True)
            if db.execute("SELECT 1 FROM profiles WHERE device_id=?", (device_id,)).fetchone():
                raise ProfileError(409, "ALREADY_INITIALIZED")
            if body.voice_pack_id is not None:
                self._check_voice_pack(db, device_id, body.voice_pack_id)
            db.execute("INSERT INTO profiles(device_id,rev,pet_name,temperament,temperament_seq,voice_pack_id,"
                       "created_at,updated_at) VALUES(?,?,?,?,0,?,?,?)",
                       (device_id, 1, body.pet_name, body.temperament, body.voice_pack_id, now, now))
            self._issue_temperament(db, device_id, body.temperament, now, ip, actor)
            if body.voice_pack_id is not None:
                commands.enqueue(db, device_id, "SELECT_VOICE_PACK", {"content_id": body.voice_pack_id},
                                 now, actor=actor, ip=ip)
            commands.audit(db, now, actor, "profile.init", device_id, "success", ip)
            view = self._profile_view(db, device_id)
            db.commit()
        return view

    @staticmethod
    def _issue_temperament(db, device_id: str, trait: str, now: int, ip: str | None, actor: str) -> None:
        """One SET_TEMPERAMENT in flight; `seq` grows with every issue so a device can refuse
        a superseded command it still holds (see SERVER_S3.md)."""
        seq = db.execute("SELECT temperament_seq FROM profiles WHERE device_id=?", (device_id,)).fetchone()[0] + 1
        db.execute("UPDATE profiles SET temperament_seq=? WHERE device_id=?", (seq, device_id))
        commands.supersede(db, device_id, "SET_TEMPERAMENT", now)
        commands.enqueue(db, device_id, "SET_TEMPERAMENT", {"temperament": trait, "seq": seq}, now,
                         actor=actor, ip=ip)

    @staticmethod
    def _pending_temperament(db, device_id: str, now: int) -> str | None:
        row = db.execute("SELECT payload_json FROM commands WHERE device_id=? AND kind='SET_TEMPERAMENT' "
                         "AND state='pending' AND expires_at>? ORDER BY seq DESC LIMIT 1",
                         (device_id, now)).fetchone()
        return json.loads(row[0])["temperament"] if row else None

    def on_commands(self, db, device_id: str, changed: list[dict], now: int) -> None:
        """Hook for S2 receipts. An applied SET_TEMPERAMENT is what the device now holds,
        even a superseded one it still had awaiting confirmation: record it as confirmed and,
        if it is not what the parent wants, issue the wanted one again (its seq is higher)."""
        applied = [c for c in changed if c["kind"] == "SET_TEMPERAMENT" and c["state"] == "applied"]
        row = db.execute("SELECT temperament FROM profiles WHERE device_id=?", (device_id,)).fetchone()
        if not applied or row is None:
            return
        confirmed = json.loads(applied[-1]["payload_json"])["temperament"]
        db.execute("UPDATE profiles SET temperament_confirmed=? WHERE device_id=?", (confirmed, device_id))
        if confirmed != row["temperament"] and self._pending_temperament(db, device_id, now) != row["temperament"]:
            self._issue_temperament(db, device_id, row["temperament"], now, None, "server")

    def update_profile(self, device_id: str, body: ProfileBody, ip: str | None,
                       actor: str = "parent") -> dict[str, Any]:
        if body.pet_name is None and body.temperament is None and body.voice_pack_id is None:
            raise ProfileError(422, "NOTHING_TO_UPDATE")
        if body.pet_name is not None:
            self._check_name(body.pet_name)
        if body.temperament is not None:
            self._check_temperament(body.temperament)
        now = int(self.clock())
        with connect(self.filename) as db:
            db.execute("BEGIN IMMEDIATE")
            self._device(db, device_id, write=True)
            row = db.execute("SELECT * FROM profiles WHERE device_id=?", (device_id,)).fetchone()
            if row is None:
                raise ProfileError(409, "NOT_INITIALIZED")
            if body.base_rev != row["rev"]:
                raise ProfileError(409, "PROFILE_REV_CONFLICT")
            name = body.pet_name if body.pet_name is not None else row["pet_name"]
            trait = body.temperament if body.temperament is not None else row["temperament"]
            pack = body.voice_pack_id if body.voice_pack_id is not None else row["voice_pack_id"]
            pack_changed = pack != row["voice_pack_id"]
            if pack_changed:
                self._check_voice_pack(db, device_id, pack)
            if (name, trait, pack) != (row["pet_name"], row["temperament"], row["voice_pack_id"]):
                db.execute("UPDATE profiles SET rev=rev+1,pet_name=?,temperament=?,voice_pack_id=?,"
                           "updated_at=? WHERE device_id=?", (name, trait, pack, now, device_id))
            if body.temperament is not None:
                # Send unless the device already confirmed it or the same request is still pending:
                # a refused or expired request is re-issued when the parent submits it again.
                if trait != row["temperament_confirmed"] and self._pending_temperament(db, device_id, now) != trait:
                    self._issue_temperament(db, device_id, trait, now, ip, actor)
                elif trait == row["temperament_confirmed"]:
                    commands.supersede(db, device_id, "SET_TEMPERAMENT", now)
            if pack_changed:
                commands.supersede(db, device_id, "SELECT_VOICE_PACK", now)
                commands.enqueue(db, device_id, "SELECT_VOICE_PACK", {"content_id": pack}, now,
                                 actor=actor, ip=ip)
            commands.audit(db, now, actor, "profile.update", device_id, "success", ip)
            view = self._profile_view(db, device_id)
            db.commit()
        return view

    # -- policy ---------------------------------------------------------

    def get_policy(self, device_id: str) -> dict[str, Any]:
        now = int(self.clock())
        with connect(self.filename) as db:
            db.execute("BEGIN IMMEDIATE")
            self._device(db, device_id)
            self._expire(db, now, device_id)
            view = policy.view(db, device_id, now)
            db.commit()
        return view

    def update_policy(self, device_id: str, body: PolicyBody, ip: str | None,
                      reauth: Callable[[], None], actor: str = "parent") -> dict[str, Any]:
        changes = body.settings.model_dump(exclude_none=True) if body.settings else {}
        if not changes and body.open_chat is None:
            raise ProfileError(422, "NOTHING_TO_UPDATE")
        now = int(self.clock())
        with connect(self.filename) as db:
            db.execute("BEGIN IMMEDIATE")
            self._device(db, device_id, write=True)
            if body.open_chat:
                # Turning chat on is a sensitive action (§6.5/§9): step-up first, then the code-level gate.
                db.commit()
                reauth()
                db.execute("BEGIN IMMEDIATE")
                if not policy.OPEN_CHAT_SELECTABLE:
                    commands.audit(db, now, actor, "policy.update", device_id, "open_chat_unavailable", ip)
                    db.commit()
                    raise ProfileError(409, "OPEN_CHAT_UNAVAILABLE")
            try:
                view = policy.update(db, device_id, base_rev=body.base_rev, changes=changes,
                                     open_chat=body.open_chat, now=now, ip=ip, actor=actor)
            except policy.PolicyError as error:
                db.commit()
                raise ProfileError(error.status, error.code) from None
            db.commit()
        return view

    def on_snapshot(self, db, device_id: str, settings: ParentSettings, device_rev: int, now: int) -> None:
        """Hook for S2's snapshot transaction (conservative merge, §5.4)."""
        policy.on_snapshot(db, device_id, settings, device_rev, now)

    # -- notes ----------------------------------------------------------

    def get_notes(self, device_id: str) -> dict[str, Any]:
        with connect(self.filename) as db:
            self._device(db, device_id)
            return self.notes.get(db, device_id)

    def put_notes(self, device_id: str, body: NotesBody, ip: str | None, actor: str = "parent") -> dict[str, Any]:
        now = int(self.clock())
        with connect(self.filename) as db:
            db.execute("BEGIN IMMEDIATE")
            self._device(db, device_id, write=True)
            result = self.notes.put(db, device_id, body.nickname, body.birthday, body.note, now, ip, actor)
            db.commit()
        return result

    def delete_notes(self, device_id: str, ip: str | None, actor: str = "parent") -> dict[str, Any]:
        now = int(self.clock())
        with connect(self.filename) as db:
            db.execute("BEGIN IMMEDIATE")
            self._device(db, device_id)  # a revoked device's notes can still be deleted
            self.notes.delete(db, device_id, now, ip, actor)
            db.commit()
        return {"status": "deleted"}


def create_router(app, database) -> APIRouter:
    service = ProfileService(database, app.state.settings, app.state.state_sync)
    app.state.profile_service = service
    app.state.state_sync.hooks.append(service.on_snapshot)
    app.state.state_sync.command_hooks.append(service.on_commands)
    router = APIRouter(prefix="/v1/parent/devices/{device_id}")

    def parent_session(request: Request, device_id: str, *, write: bool) -> int:
        check_origin(request)
        if not DEVICE_ID.fullmatch(device_id):
            raise HTTPException(404, "DEVICE_NOT_FOUND", headers=NO_STORE)
        try:
            return request.app.state.parent_auth.session(
                request.cookies.get(SESSION_COOKIE),
                csrf=(request.headers.get("x-csrf-token") or "") if write else None,
                csrf_cookie=request.cookies.get(CSRF_COOKIE))
        except AuthError as error:
            raise HTTPException(error.status, error.code, headers=NO_STORE) from None

    # Dependencies run before body validation: an anonymous caller gets 401, never a schema error.
    def parent_read(request: Request, device_id: str) -> int:
        return parent_session(request, device_id, write=False)

    def parent_write(request: Request, device_id: str) -> int:
        return parent_session(request, device_id, write=True)

    def step_up(request: Request, account_id: int, proof: Reauth) -> None:
        try:
            request.app.state.parent_auth.reauth(account_id, proof.password, proof.otp, client_ip(request))
        except AuthError as error:
            raise HTTPException(error.status, error.code, headers=NO_STORE) from None

    def call(fn, *args):
        try:
            return JSONResponse(fn(*args), headers=NO_STORE)
        except (ProfileError, NotesError) as error:
            raise HTTPException(error.status, error.code, headers=NO_STORE) from None

    @router.get("/profile")
    def get_profile(device_id: str, request: Request, _: int = Depends(parent_read)):
        return call(service.get_profile, device_id)

    @router.post("/init")
    def init(device_id: str, body: InitBody, request: Request, account: int = Depends(parent_write)):
        return call(service.init, device_id, body, client_ip(request), f"parent:{account}")

    @router.put("/profile")
    def put_profile(device_id: str, body: ProfileBody, request: Request, account: int = Depends(parent_write)):
        return call(service.update_profile, device_id, body, client_ip(request), f"parent:{account}")

    @router.get("/policy")
    def get_policy(device_id: str, request: Request, _: int = Depends(parent_read)):
        return call(service.get_policy, device_id)

    @router.put("/policy")
    def put_policy(device_id: str, body: PolicyBody, request: Request, account: int = Depends(parent_write)):

        def reauth() -> None:
            if body.reauth is None:
                raise HTTPException(403, "PARENT_REAUTH_REQUIRED", headers=NO_STORE)
            step_up(request, account, body.reauth)
        return call(service.update_policy, device_id, body, client_ip(request), reauth, f"parent:{account}")

    @router.get("/notes")
    def get_notes(device_id: str, request: Request, _: int = Depends(parent_read)):
        return call(service.get_notes, device_id)

    @router.put("/notes")
    def put_notes(device_id: str, body: NotesBody, request: Request, account: int = Depends(parent_write)):
        try:  # validate first: a malformed request never spends a TOTP step or counts as a failed proof
            validate_notes(body.nickname, body.birthday, body.note)
        except NotesError as error:
            raise HTTPException(error.status, error.code, headers=NO_STORE) from None
        step_up(request, account, body.reauth)
        return call(service.put_notes, device_id, body, client_ip(request), f"parent:{account}")

    @router.delete("/notes")
    def delete_notes(device_id: str, body: NotesDeleteBody, request: Request,
                     account: int = Depends(parent_write)):
        step_up(request, account, body.reauth)
        return call(service.delete_notes, device_id, client_ip(request), f"parent:{account}")

    return router
