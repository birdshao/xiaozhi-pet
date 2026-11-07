"""Content distribution (plan S4, §7.4, D10): assignment, manifest, Range download, reports.

Devices poll the manifest on their snapshot cadence; ``ContentCatalog.on_sync_needed``
is the hook S2 uses to push a ``SYNC_CONTENT`` command instead.
"""
from __future__ import annotations

import hashlib
import json
import re
import threading
import time
from typing import Literal

from fastapi import APIRouter, Depends, Header, HTTPException, Request, Response
from fastapi.concurrency import run_in_threadpool
from fastapi.responses import JSONResponse, StreamingResponse
from pydantic import Field

from .auth import AuthError, CSRF_COOKIE, SESSION_COOKIE
from .content import (MAX_UPLOAD_BYTES, ContentCatalog, ContentError, audit, package_view)
from .db import connect
from .models import StrictModel
from .pairing import device_auth
from .routers.parent import check_origin, client_ip

SHA_RE = re.compile(r"^[0-9a-f]{64}$")
SPACE_RESERVE_KIB = 1024  # keep 1 MiB free on the card for the device's own files
CHUNK = 64 * 1024
LEASE_STALE_SECONDS = 15  # short: a client that vanishes before streaming frees its slot
AUDIT_WINDOW_SECONDS = 300
NO_STORE = {"Cache-Control": "no-store"}


class AssignItem(StrictModel):
    package_id: int = Field(ge=1)
    wanted: Literal["install", "remove"]


class AssignBody(StrictModel):
    items: list[AssignItem] = Field(min_length=1, max_length=64)


class ApproveBody(StrictModel):
    sha256: str = Field(pattern=r"^[0-9a-f]{64}$")
    checklist: dict[str, bool] = Field(min_length=1, max_length=32)


class ReportBody(StrictModel):
    sha256: str = Field(pattern=r"^[0-9a-f]{64}$")
    state: Literal["downloading", "installed", "failed", "removed"]
    reason: str | None = Field(default=None, pattern=r"^[A-Z0-9_]{1,40}$")
    attempts: int = Field(default=0, ge=0, le=100)


class InventoryItem(StrictModel):
    kind: Literal["book", "soundpack", "game"]
    content_id: int | None = Field(default=None, ge=0, le=65535)
    revision: int = Field(ge=0, le=4294967295)
    header_crc: int = Field(ge=0, le=4294967295)
    bytes: int | None = Field(default=None, ge=0, le=MAX_UPLOAD_BYTES)


class InventoryBody(StrictModel):
    items: list[InventoryItem] = Field(max_length=256)
    sd_free_kib: int | None = Field(default=None, ge=0, le=2 ** 31)


class RangeNotSatisfiable(Exception):
    pass


def parse_range(header: str | None, size: int) -> tuple[int, int] | None:
    """Single ``bytes=`` range -> (first, last); ``None`` = serve the whole file.

    Syntactically invalid or multi-range headers are ignored (RFC 9110 §14.2);
    a valid but unsatisfiable range raises RangeNotSatisfiable (416).
    """
    if not header:
        return None
    unit, _, spec = header.partition("=")
    if unit.strip().lower() != "bytes":
        return None
    match = re.fullmatch(r"\s*(\d{0,18})-(\d{0,18})\s*", spec)
    if not match or (not match[1] and not match[2]):
        return None
    if not match[1]:
        suffix = int(match[2])
        if suffix == 0:
            raise RangeNotSatisfiable
        return max(0, size - suffix), size - 1
    first = int(match[1])
    if match[2] and int(match[2]) < first:
        return None
    if first >= size:
        raise RangeNotSatisfiable
    return first, min(int(match[2]), size - 1) if match[2] else size - 1


class Distribution:
    def __init__(self, catalog: ContentCatalog, *, clock=time.time) -> None:
        self.catalog = catalog
        self.database = catalog.database
        self.clock = clock
        self._leases: dict[str, tuple[object, float]] = {}
        self._lease_lock = threading.Lock()
        self._audit_seen: dict[tuple, float] = {}

    # -- assignment ---------------------------------------------------------
    def assign(self, device_id: str, items: list[tuple[int, str]], *, ip=None) -> None:
        now = int(self.clock())
        with connect(self.database) as db:
            db.execute("BEGIN IMMEDIATE")
            if not db.execute("SELECT 1 FROM devices WHERE device_id=?", (device_id,)).fetchone():
                raise ContentError(404, "DEVICE_NOT_FOUND")
            for package_id, wanted in items:
                pkg = db.execute("SELECT * FROM content_packages WHERE package_id=?", (package_id,)).fetchone()
                if pkg is None:
                    raise ContentError(404, "PACKAGE_NOT_FOUND")
                row = db.execute("SELECT * FROM device_content WHERE device_id=? AND package_id=?",
                                 (device_id, package_id)).fetchone()
                if wanted == "install":
                    if pkg["status"] != "approved":
                        raise ContentError(409, "PACKAGE_NOT_APPROVED")
                    # One file per card path: a new assignment replaces the old one.
                    db.execute("DELETE FROM device_content WHERE device_id=? AND package_id IN "
                               "(SELECT package_id FROM content_packages WHERE path=? AND kind=? "
                               "AND package_id!=?) AND wanted='install'",
                               (device_id, pkg["path"], pkg["kind"], package_id))
                    if row is None or row["wanted"] != "install":
                        db.execute("INSERT OR REPLACE INTO device_content(device_id,package_id,wanted,"
                                   "state,updated_at) VALUES(?,?,'install','pending',?)",
                                   (device_id, package_id, now))
                else:
                    if row is None:
                        # Hand-copied files are never touched; only tracked packs can be removed.
                        raise ContentError(409, "NOT_ASSIGNED")
                    if row["state"] in ("pending", "failed"):
                        db.execute("DELETE FROM device_content WHERE device_id=? AND package_id=?",
                                   (device_id, package_id))
                    else:
                        db.execute("UPDATE device_content SET wanted='remove',updated_at=? "
                                   "WHERE device_id=? AND package_id=?", (now, device_id, package_id))
            audit(db, now, "parent", "content.assign", device_id, "success", ip)
        self.catalog.notify([device_id])

    # -- manifest -----------------------------------------------------------
    def _plan(self, db, device_id: str) -> dict:
        inv = db.execute("SELECT sd_free_kib FROM device_content_inventory WHERE device_id=?",
                         (device_id,)).fetchone()
        free_kib = inv["sd_free_kib"] if inv else None
        rows = db.execute(
            "SELECT d.wanted,d.state,d.reason,d.attempts,k.* FROM device_content d JOIN content_packages k "
            "USING(package_id) WHERE d.device_id=? ORDER BY k.package_id", (device_id,)).fetchall()
        budget = None if free_kib is None else max(0, free_kib - SPACE_RESERVE_KIB) * 1024
        install, withheld, remove = [], [], []
        paths = {r["path"] for r in rows if r["wanted"] == "install" and r["status"] == "approved"}
        for r in rows:
            if r["wanted"] == "remove":
                if r["path"] not in paths:  # a replacement install overwrites the file anyway
                    remove.append(r)
                continue
            if r["status"] != "approved":
                continue
            if r["state"] != "installed" and budget is not None:
                if r["bytes"] > budget:
                    withheld.append(r)
                    continue
                budget -= r["bytes"]
            install.append(r)
        return {"free_kib": free_kib, "install": install, "withheld": withheld, "remove": remove}

    @staticmethod
    def _entry(r) -> dict:
        return {"kind": r["kind"], "content_id": r["content_id"], "revision": r["revision"],
                "sha256": r["sha256"], "header_crc": r["header_crc"], "bytes": r["bytes"],
                "path": r["path"], "url": f"/v1/content/packages/{r['sha256']}"}

    def manifest(self, device_id: str) -> dict:
        with connect(self.database) as db:
            plan = self._plan(db, device_id)
        return {"generated_at": int(self.clock()),
                "install": [self._entry(r) for r in plan["install"]],
                "remove": [{k: v for k, v in self._entry(r).items() if k != "url"} for r in plan["remove"]],
                "withheld": [{"sha256": r["sha256"], "reason": "NO_SPACE", "bytes": r["bytes"]}
                             for r in plan["withheld"]]}

    # -- parent view --------------------------------------------------------
    def parent_view(self, device_id: str) -> dict:
        with connect(self.database) as db:
            if not db.execute("SELECT 1 FROM devices WHERE device_id=?", (device_id,)).fetchone():
                raise ContentError(404, "DEVICE_NOT_FOUND")
            plan = self._plan(db, device_id)
            withheld = {r["sha256"] for r in plan["withheld"]}
            rows = db.execute(
                "SELECT d.wanted,d.state,d.reason,d.attempts,d.updated_at,k.* FROM device_content d "
                "JOIN content_packages k USING(package_id) WHERE d.device_id=? ORDER BY k.package_id",
                (device_id,)).fetchall()
            inv = db.execute("SELECT * FROM device_content_inventory WHERE device_id=?",
                             (device_id,)).fetchone()
        return {
            "assignments": [{**package_view(r), "wanted": r["wanted"], "state": r["state"],
                             "reason": "NO_SPACE" if r["sha256"] in withheld else r["reason"],
                             "attempts": r["attempts"], "updated_at": r["updated_at"]} for r in rows],
            "card": None if inv is None else {"sd_free_kib": inv["sd_free_kib"],
                                              "reported_at": inv["reported_at"],
                                              "items": json.loads(inv["inventory_json"])}}

    # -- device reports -----------------------------------------------------
    def report(self, device_id: str, body: ReportBody, *, ip=None) -> None:
        now = int(self.clock())
        with connect(self.database) as db:
            db.execute("BEGIN IMMEDIATE")
            row = db.execute(
                "SELECT d.*,k.path FROM device_content d JOIN content_packages k USING(package_id) "
                "WHERE d.device_id=? AND k.sha256=?", (device_id, body.sha256)).fetchone()
            if row is None:
                raise ContentError(404, "NOT_ASSIGNED")
            if (body.state == "removed") != (row["wanted"] == "remove"):
                raise ContentError(409, "ASSIGNMENT_NOT_ACTIVE")
            if body.state == "failed" and not body.reason:
                raise ContentError(422, "REASON_REQUIRED")
            if body.state == "removed":
                db.execute("DELETE FROM device_content WHERE device_id=? AND package_id=?",
                           (device_id, row["package_id"]))
            else:
                db.execute("UPDATE device_content SET state=?,reason=?,attempts=?,updated_at=? "
                           "WHERE device_id=? AND package_id=?",
                           (body.state, body.reason if body.state == "failed" else None,
                            body.attempts, now, device_id, row["package_id"]))
                if body.state == "installed":  # the new file replaced any older one at this path
                    db.execute("DELETE FROM device_content WHERE device_id=? AND package_id!=? AND package_id IN "
                               "(SELECT package_id FROM content_packages WHERE path=?)",
                               (device_id, row["package_id"], row["path"]))
            if body.state != "downloading" or row["state"] != "downloading":
                audit(db, now, "device", f"content.report.{body.state}", device_id, body.reason or "ok", ip)

    def reconcile(self, device_id: str, items: list[dict], sd_free_kib: int | None) -> list[dict]:
        """Compare the card inventory with the catalog and the assignments.

        Also the entry point for S2: call it with the snapshot's inventory.
        Returns the items with ``review``: approved | outdated | unreviewed.
        """
        now = int(self.clock())
        checked = []
        with connect(self.database) as db:
            db.execute("BEGIN IMMEDIATE")
            if not db.execute("SELECT 1 FROM devices WHERE device_id=?", (device_id,)).fetchone():
                raise ContentError(404, "DEVICE_NOT_FOUND")
            present: set[int] = set()
            for item in items:
                sql = ("SELECT package_id,status FROM content_packages WHERE kind=? AND revision=? "
                       "AND header_crc=?")
                args = [item["kind"], item["revision"], item["header_crc"]]
                if item["kind"] != "soundpack":
                    sql += " AND content_id=?"
                    args.append(item.get("content_id"))
                best = None
                for match in db.execute(sql, args).fetchall():
                    present.add(match["package_id"])
                    if best is None or match["status"] == "approved":
                        best = match["status"]
                review = {"approved": "approved", "retired": "outdated"}.get(best, "unreviewed")
                checked.append({**item, "review": review})
            for r in db.execute("SELECT * FROM device_content WHERE device_id=?", (device_id,)).fetchall():
                here = r["package_id"] in present
                if r["wanted"] == "remove" and not here:
                    db.execute("DELETE FROM device_content WHERE device_id=? AND package_id=?",
                               (device_id, r["package_id"]))
                elif r["wanted"] == "install" and here and r["state"] != "installed":
                    db.execute("UPDATE device_content SET state='installed',reason=NULL,updated_at=? "
                               "WHERE device_id=? AND package_id=?", (now, device_id, r["package_id"]))
                elif r["wanted"] == "install" and not here and r["state"] == "installed":
                    db.execute("UPDATE device_content SET state='pending',updated_at=? "
                               "WHERE device_id=? AND package_id=?", (now, device_id, r["package_id"]))
            db.execute("INSERT INTO device_content_inventory(device_id,sd_free_kib,inventory_json,reported_at) "
                       "VALUES(?,?,?,?) ON CONFLICT(device_id) DO UPDATE SET sd_free_kib=excluded.sd_free_kib,"
                       "inventory_json=excluded.inventory_json,reported_at=excluded.reported_at",
                       (device_id, sd_free_kib, json.dumps(checked, sort_keys=True), now))
        return checked

    def audit_once(self, key: tuple) -> bool:
        """True at most once per window for a key, so retry loops can't flood audit_log."""
        now = time.monotonic()
        with self._lease_lock:
            if len(self._audit_seen) > 1000:
                self._audit_seen = {k: t for k, t in self._audit_seen.items()
                                    if now - t < AUDIT_WINDOW_SECONDS}
            if now - self._audit_seen.get(key, -AUDIT_WINDOW_SECONDS) < AUDIT_WINDOW_SECONDS:
                return False
            self._audit_seen[key] = now
            return True

    # -- download lease: one concurrent download per device (plan §9) -------
    # In-process state: correct for the single uvicorn worker S7 deploys; with several
    # workers the limit would be per worker.
    def acquire(self, device_id: str) -> object | None:
        with self._lease_lock:
            held = self._leases.get(device_id)
            if held and time.monotonic() - held[1] < LEASE_STALE_SECONDS:
                return None
            token = object()
            self._leases[device_id] = (token, time.monotonic())
            return token

    def touch(self, device_id: str, token: object) -> None:
        with self._lease_lock:
            if self._leases.get(device_id, (None,))[0] is token:
                self._leases[device_id] = (token, time.monotonic())

    def release(self, device_id: str, token: object) -> None:
        with self._lease_lock:
            if self._leases.get(device_id, (None,))[0] is token:
                del self._leases[device_id]

    def downloadable(self, device_id: str, sha256: str) -> dict | None:
        """The package only if approved *and* assigned (install) to this device."""
        with connect(self.database) as db:
            return db.execute(
                "SELECT k.* FROM content_packages k JOIN device_content d USING(package_id) "
                "WHERE k.sha256=? AND k.status='approved' AND d.device_id=? AND d.wanted='install'",
                (sha256, device_id)).fetchone()


def _fail(error: ContentError) -> HTTPException:
    return HTTPException(error.status, error.code, headers=NO_STORE)


def create_router(app, database) -> APIRouter:
    catalog = ContentCatalog(database)
    dist = Distribution(catalog)
    app.state.content_catalog = catalog
    app.state.content_distribution = dist
    store = app.state.pet_store
    router = APIRouter()

    def parent(request: Request, *, write: bool = False) -> int:
        if write:
            check_origin(request)
        try:
            return request.app.state.parent_auth.session(
                request.cookies.get(SESSION_COOKIE),
                csrf=(request.headers.get("x-csrf-token") or "") if write else None,
                csrf_cookie=request.cookies.get(CSRF_COOKIE), ip=client_ip(request))
        except AuthError as error:
            raise HTTPException(error.status, error.code, headers=NO_STORE) from None

    def device(device_id: str, authorization: str | None) -> None:
        if not authorization or not authorization.startswith("Bearer ") \
                or not store.authenticated(device_id, authorization[7:]):
            raise HTTPException(401, "DEVICE_AUTH_REQUIRED", headers=NO_STORE)

    # ---- parent: catalog ----------------------------------------------------
    @router.post("/v1/parent/content", status_code=201)
    async def upload(request: Request, pack_id: str | None = None):
        parent(request, write=True)
        if request.headers.get("content-type", "").split(";")[0].strip() != "application/octet-stream":
            raise HTTPException(415, "UNSUPPORTED_MEDIA_TYPE", headers=NO_STORE)
        declared = request.headers.get("content-length", "")
        if declared.isdigit() and int(declared) > MAX_UPLOAD_BYTES:
            raise HTTPException(413, "PACK_TOO_LARGE", headers=NO_STORE)
        spool, digest, size = catalog.new_spool(), hashlib.sha256(), 0
        try:
            with open(spool, "wb") as out:
                async for chunk in request.stream():
                    size += len(chunk)
                    if size > MAX_UPLOAD_BYTES:
                        raise HTTPException(413, "PACK_TOO_LARGE", headers=NO_STORE)
                    digest.update(chunk)
                    await run_in_threadpool(out.write, chunk)
            package, created = await run_in_threadpool(
                catalog.import_file, spool, digest.hexdigest(), size,
                pack_id=pack_id, ip=client_ip(request))
        except ContentError as error:
            raise _fail(error) from None
        finally:
            spool.unlink(missing_ok=True)  # no-op once import_file moved it into place
        return JSONResponse({"package": package, "created": created},
                            status_code=201 if created else 200, headers=NO_STORE)

    @router.get("/v1/parent/content")
    def catalog_list(request: Request):
        parent(request)
        return JSONResponse({"packages": catalog.list()}, headers=NO_STORE)

    @router.post("/v1/parent/content/{package_id}/approve")
    def approve(package_id: int, body: ApproveBody, request: Request):
        account = parent(request, write=True)
        with connect(database) as db:
            row = db.execute("SELECT username FROM parent_accounts WHERE id=?", (account,)).fetchone()
        try:
            package = catalog.approve(package_id, body.sha256, body.checklist,
                                      row["username"] if row else "parent", ip=client_ip(request))
        except ContentError as error:
            raise _fail(error) from None
        return JSONResponse({"package": package}, headers=NO_STORE)

    @router.post("/v1/parent/content/{package_id}/retire")
    def retire(package_id: int, request: Request):
        parent(request, write=True)
        try:
            package = catalog.retire(package_id, ip=client_ip(request))
        except ContentError as error:
            raise _fail(error) from None
        return JSONResponse({"package": package}, headers=NO_STORE)

    # ---- parent: per-device assignment -------------------------------------
    @router.get("/v1/parent/devices/{device_id}/content")
    def device_content(device_id: str, request: Request):
        parent(request)
        try:
            return JSONResponse(dist.parent_view(device_id), headers=NO_STORE)
        except ContentError as error:
            raise _fail(error) from None

    @router.put("/v1/parent/devices/{device_id}/content")
    def assign(device_id: str, body: AssignBody, request: Request):
        parent(request, write=True)
        try:
            dist.assign(device_id, [(i.package_id, i.wanted) for i in body.items], ip=client_ip(request))
            return JSONResponse(dist.parent_view(device_id), headers=NO_STORE)
        except ContentError as error:
            raise _fail(error) from None

    # ---- device -------------------------------------------------------------
    @router.get("/v1/devices/{device_id}/content/manifest")
    def manifest(device_id: str, authorization: str | None = Header(default=None)):
        device(device_id, authorization)
        return JSONResponse(dist.manifest(device_id), headers=NO_STORE)

    @router.post("/v1/devices/{device_id}/content/report")
    def report(device_id: str, body: ReportBody, request: Request,
               authorization: str | None = Header(default=None)):
        device(device_id, authorization)
        try:
            dist.report(device_id, body, ip=client_ip(request))
        except ContentError as error:
            raise _fail(error) from None
        return JSONResponse({"status": "ok"}, headers=NO_STORE)

    @router.put("/v1/devices/{device_id}/content/inventory")
    def inventory(device_id: str, body: InventoryBody,
                  authorization: str | None = Header(default=None)):
        device(device_id, authorization)
        try:
            checked = dist.reconcile(device_id, [i.model_dump() for i in body.items], body.sd_free_kib)
        except ContentError as error:
            raise _fail(error) from None
        return JSONResponse({"items": checked}, headers=NO_STORE)

    @router.api_route("/v1/content/packages/{sha256}", methods=["GET", "HEAD"])
    def download(sha256: str, request: Request,
                 x_device_id: str | None = Header(default=None),
                 authorization: str | None = Header(default=None),
                 range_header: str | None = Header(default=None, alias="Range"),
                 if_range: str | None = Header(default=None)):
        if not x_device_id:
            raise HTTPException(401, "DEVICE_AUTH_REQUIRED", headers=NO_STORE)
        device(x_device_id, authorization)
        # Unapproved, unassigned (or another device's) and unknown all look the same.
        package = dist.downloadable(x_device_id, sha256) if SHA_RE.fullmatch(sha256) else None
        if package is None:
            if dist.audit_once(("denied", x_device_id, sha256[:16])):
                with connect(database) as db:
                    audit(db, int(time.time()), "device", "content.download", x_device_id, "denied",
                          client_ip(request))
            raise HTTPException(404, "PACKAGE_NOT_FOUND", headers=NO_STORE)
        size = package["bytes"]
        etag = f'"{sha256}"'
        headers = {"ETag": etag, "Accept-Ranges": "bytes", "Cache-Control": "no-store",
                   "Content-Type": "application/octet-stream",
                   "Content-Disposition": f'attachment; filename="{sha256}.bin"'}
        try:
            span = None if (if_range and if_range.strip() != etag) else parse_range(range_header, size)
        except RangeNotSatisfiable:
            return Response(status_code=416, headers={**headers, "Content-Range": f"bytes */{size}",
                                                      "Content-Length": "0"})
        first, last = span or (0, size - 1)
        length = last - first + 1
        headers["Content-Length"] = str(length)
        if span:
            headers["Content-Range"] = f"bytes {first}-{last}/{size}"
        status = 206 if span else 200
        if request.method == "HEAD":
            return Response(status_code=status, headers=headers)
        try:
            handle = open(catalog.package_path(sha256), "rb")
        except OSError:
            raise HTTPException(503, "PACKAGE_FILE_MISSING", headers=NO_STORE) from None
        if size != catalog.package_path(sha256).stat().st_size:
            handle.close()
            raise HTTPException(503, "PACKAGE_FILE_MISSING", headers=NO_STORE)
        lease = dist.acquire(x_device_id)
        if lease is None:
            handle.close()
            raise HTTPException(429, "DOWNLOAD_BUSY", headers={**NO_STORE, "Retry-After": "5"})
        if first == 0:
            with connect(database) as db:
                audit(db, int(time.time()), "device", "content.download", x_device_id, sha256[:16],
                      client_ip(request))

        def stream():
            try:
                handle.seek(first)
                remaining = length
                while remaining > 0:
                    chunk = handle.read(min(CHUNK, remaining))
                    if not chunk:
                        break
                    remaining -= len(chunk)
                    dist.touch(x_device_id, lease)
                    yield chunk
            finally:
                handle.close()
                dist.release(x_device_id, lease)

        return StreamingResponse(stream(), status_code=status, headers=headers)

    return router
