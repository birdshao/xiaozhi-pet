"""Screen-code pairing and device credentials (plan 5.1, 9).

Only SHA-256 digests of tokens, claim secrets and pairing codes are stored, and
none of them is ever written to the audit log. Devices enrolled by the CLI before
S1 keep working: the migration marks them ``active`` with their existing token.
"""
from __future__ import annotations

import contextvars
import functools
import hashlib
import hmac
import json
import re
import secrets
import time

from fastapi import APIRouter, HTTPException, Request
from fastapi.responses import JSONResponse
from starlette.requests import Request as StarletteRequest

from .auth import AuthError, CSRF_COOKIE, SESSION_COOKIE
from .db import connect, migrate
from .routers.parent import check_origin, client_ip

DEVICE_RE = re.compile(r"^[a-zA-Z0-9][a-zA-Z0-9_-]{2,63}$")
CODE_RE = re.compile(r"^[0-9]{6}$")
PAIRING_ID_RE = re.compile(r"^[A-Za-z0-9_-]{16,64}$")

PAIRING_TTL = 600           # parent click -> code must be entered
CLAIM_TTL = 120             # after approval, device must claim
CONFIRM_TTL = 300           # after claim, device must confirm or the row is purged
CODE_ATTEMPTS = 5           # wrong codes before the pairing is void
STARTS_PER_PAIRING = 3
ROTATION_OVERLAP = 24 * 3600
TOUCH_INTERVAL = 60         # throttle last-used writes

# (limit, window seconds) per scope; every scope counts events within the window.
LIMITS = {
    "create_acct": (10, 600),
    "start_ip": (10, 600),
    "start_dev": (5, 600),
    "claim_ip": (120, 600),
    "redeem_ip": (10, 600),
    "redeem_acct": (10, 900),   # wrong codes only; lockout until the window drains
    "rotate_dev": (10, 3600),
    "devauth_ip": (20, 600),    # bad tokens on rotate/confirm endpoints
}

request_ip: contextvars.ContextVar[str | None] = contextvars.ContextVar("request_ip", default=None)


def now() -> int:
    return int(time.time())


def digest(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


class PairingError(Exception):
    def __init__(self, status: int, code: str):
        self.status = status
        self.code = code
        super().__init__(code)


def _audit(db, at, actor, action, target, result, ip):
    db.execute("INSERT INTO audit_log(at,actor,action,target,result,ip) VALUES(?,?,?,?,?,?)",
               (at, actor, action, target, result, ip))


def _count(db, scope, key, at):
    limit, window = LIMITS[scope]
    db.execute("DELETE FROM pairing_attempts WHERE scope=? AND at<=?", (scope, at - window))
    return db.execute("SELECT COUNT(*) FROM pairing_attempts WHERE scope=? AND key=?",
                      (scope, key)).fetchone()[0], limit


def _hit(db, scope, key, at):
    db.execute("INSERT INTO pairing_attempts VALUES(?,?,?)", (scope, key, at))


def _limited(db, scope, key, at, *, record):
    """True when the scope is already at its limit; otherwise optionally record a hit."""
    count, limit = _count(db, scope, key, at)
    if count >= limit:
        return True
    if record:
        _hit(db, scope, key, at)
    return False


def _matches(row, token):
    """Which stored credential matches: 'current', 'previous' or None (both compared)."""
    given = digest(token)
    current = hmac.compare_digest(given, row["token_sha"])
    previous = hmac.compare_digest(given, row["prev_token_sha"] or "0" * 64)
    if current:
        return "current"
    if previous and row["prev_token_expires_at"] > now():
        return "previous"
    return None


def _valid_token_shape(device_id, token):
    return bool(token) and len(token) <= 256 and bool(DEVICE_RE.fullmatch(device_id or ""))


def verify_token(filename, device_id, token, *, allow_pending=False):
    """Device bearer check used by every device/voice route (via PetStore.authenticated)."""
    if not _valid_token_shape(device_id, token):
        return False
    at = now()
    with connect(filename) as db:
        row = db.execute("SELECT * FROM devices WHERE device_id=?", (device_id,)).fetchone()
        if not row:
            # Burn comparable time so unknown ids are not cheaper than known ones.
            hmac.compare_digest(digest(token), "0" * 64)
            return False
        matched = _matches(row, token)
        if matched is None:
            return False
        if row["status"] == "pending":
            if not (allow_pending and matched == "current"):
                return False
        elif row["status"] != "active":
            return False
        ip = request_ip.get()
        if (row["last_seen_at"] is None or at - row["last_seen_at"] >= TOUCH_INTERVAL
                or (ip and ip != row["last_seen_ip"])):
            db.execute("UPDATE devices SET last_seen_at=?, last_seen_ip=COALESCE(?,last_seen_ip) "
                       "WHERE device_id=?", (at, ip, device_id))
    return True


class PairingService:
    def __init__(self, filename):
        self.filename = filename
        migrate(filename)

    def _run(self, work):
        """Run work(db, at) in a write transaction; errors surface after commit."""
        error = None
        with connect(self.filename) as db:
            db.execute("BEGIN IMMEDIATE")
            at = now()
            self._expire(db, at)
            result = work(db, at)
            if isinstance(result, PairingError):
                error, result = result, None
            db.commit()
        if error:
            raise error
        return result

    @staticmethod
    def _expire(db, at):
        stale = db.execute("SELECT pairing_id,device_id,state FROM pairing_codes WHERE expires_at<=? "
                           "AND state IN ('open','code_issued','approved','claimed')", (at,)).fetchall()
        for row in stale:
            if row["state"] == "claimed":
                # Token was handed out but never confirmed: discard the unusable device.
                db.execute("DELETE FROM devices WHERE device_id=? AND status='pending'", (row["device_id"],))
            db.execute("UPDATE pairing_codes SET state='expired' WHERE pairing_id=?", (row["pairing_id"],))

    # -- parent side ------------------------------------------------------
    def create_pairing(self, account_id, ip):
        def work(db, at):
            if _limited(db, "create_acct", str(account_id), at, record=True):
                _audit(db, at, "parent", "pairing.create", str(account_id), "rate_limited", ip)
                return PairingError(429, "PAIRING_THROTTLED")
            db.execute("UPDATE pairing_codes SET state='void' WHERE account_id=? "
                       "AND state IN ('open','code_issued','approved')", (account_id,))
            pairing_id = secrets.token_urlsafe(24)
            db.execute("INSERT INTO pairing_codes(pairing_id,account_id,created_at,expires_at,state) "
                       "VALUES(?,?,?,?, 'open')", (pairing_id, account_id, at, at + PAIRING_TTL))
            _audit(db, at, "parent", "pairing.create", pairing_id[:8], "success", ip)
            return {"pairing_id": pairing_id, "expires_in": PAIRING_TTL}
        return self._run(work)

    def redeem(self, account_id, pairing_id, code, ip):
        def work(db, at):
            if (_limited(db, "redeem_ip", ip, at, record=True)
                    or _count(db, "redeem_acct", str(account_id), at)[0] >= LIMITS["redeem_acct"][0]):
                _audit(db, at, "parent", "pairing.redeem", pairing_id[:8], "rate_limited", ip)
                return PairingError(429, "PAIRING_THROTTLED")
            row = db.execute("SELECT * FROM pairing_codes WHERE pairing_id=? AND account_id=?",
                             (pairing_id, account_id)).fetchone()
            if not row or row["state"] != "code_issued":
                _hit(db, "redeem_acct", str(account_id), at)
                _audit(db, at, "parent", "pairing.redeem", pairing_id[:8], "invalid", ip)
                return PairingError(404, "PAIRING_INVALID")
            if hmac.compare_digest(digest(f"{pairing_id}:{code}"), row["code_sha"]):
                db.execute("UPDATE pairing_codes SET state='approved', expires_at=? WHERE pairing_id=?",
                           (at + CLAIM_TTL, pairing_id))
                _audit(db, at, "parent", "pairing.redeem", row["device_id"], "success", ip)
                return {"device_id": row["device_id"], "status": "approved"}
            attempts = row["attempts"] + 1
            state = "void" if attempts >= CODE_ATTEMPTS else row["state"]
            db.execute("UPDATE pairing_codes SET attempts=?, state=? WHERE pairing_id=?",
                       (attempts, state, pairing_id))
            _hit(db, "redeem_acct", str(account_id), at)
            _audit(db, at, "parent", "pairing.redeem", row["device_id"],
                   "voided" if state == "void" else "failure", ip)
            return PairingError(410 if state == "void" else 400,
                                "PAIRING_VOIDED" if state == "void" else "PAIRING_CODE_INVALID")
        return self._run(work)

    def list_devices(self):
        with connect(self.filename) as db:
            rows = db.execute("SELECT * FROM devices WHERE status!='pending' ORDER BY device_id").fetchall()
        at = now()
        return [{"device_id": r["device_id"], "status": r["status"], "enrolled_at": r["enrolled_at"],
                 "token_rotated_at": r["token_rotated_at"], "last_seen_at": r["last_seen_at"],
                 "last_seen_ip": r["last_seen_ip"], "fw_version": r["fw_version"],
                 "rotation_overlap_until": r["prev_token_expires_at"]
                 if r["prev_token_sha"] and r["prev_token_expires_at"] > at else None}
                for r in rows]

    def revoke(self, account_id, device_id, ip):
        def work(db, at):
            row = db.execute("SELECT status FROM devices WHERE device_id=? AND status!='pending'",
                             (device_id,)).fetchone()
            if not row:
                return PairingError(404, "DEVICE_NOT_FOUND")
            db.execute("UPDATE devices SET status='revoked', prev_token_sha=NULL, "
                       "prev_token_expires_at=0 WHERE device_id=?", (device_id,))
            _audit(db, at, "parent", "device.revoke", device_id, "success", ip)
            return {"device_id": device_id, "status": "revoked"}
        return self._run(work)

    # -- device side ------------------------------------------------------
    def start(self, device_id, ip):
        def work(db, at):
            if (_limited(db, "start_ip", ip, at, record=True)
                    or _limited(db, "start_dev", device_id, at, record=True)):
                _audit(db, at, "device", "pairing.start", device_id, "rate_limited", ip)
                return PairingError(429, "PAIRING_THROTTLED")
            exists = db.execute("SELECT 1 FROM devices WHERE device_id=?", (device_id,)).fetchone()
            row = None if exists else db.execute(
                "SELECT * FROM pairing_codes WHERE state IN ('open','code_issued') "
                "ORDER BY created_at DESC LIMIT 1").fetchone()
            if (not row or (row["state"] == "code_issued" and row["device_id"] != device_id)
                    or row["starts"] >= STARTS_PER_PAIRING):
                # One generic answer: no open session, known id, or someone else got there first.
                _audit(db, at, "device", "pairing.start", device_id, "rejected", ip)
                return PairingError(404, "PAIRING_NOT_AVAILABLE")
            code = f"{secrets.randbelow(10 ** 6):06d}"
            claim = secrets.token_urlsafe(24)
            db.execute("UPDATE pairing_codes SET state='code_issued', device_id=?, code_sha=?, claim_sha=?, "
                       "starts=starts+1 WHERE pairing_id=?",
                       (device_id, digest(f"{row['pairing_id']}:{code}"), digest(claim), row["pairing_id"]))
            _audit(db, at, "device", "pairing.start", device_id, "success", ip)
            return {"code": code, "claim_secret": claim, "expires_in": row["expires_at"] - at}
        return self._run(work)

    def claim(self, device_id, claim_secret, ip):
        def work(db, at):
            if _limited(db, "claim_ip", ip, at, record=True):
                return PairingError(429, "PAIRING_THROTTLED")
            row = db.execute("SELECT * FROM pairing_codes WHERE device_id=? AND state IN "
                             "('code_issued','approved') ORDER BY created_at DESC LIMIT 1",
                             (device_id,)).fetchone()
            if not row or not hmac.compare_digest(digest(claim_secret), row["claim_sha"]):
                _audit(db, at, "device", "pairing.claim", device_id, "invalid", ip)
                return PairingError(401, "PAIRING_CLAIM_INVALID")
            if row["state"] == "code_issued":
                return {"status": "pending"}
            token = secrets.token_urlsafe(32)
            try:
                db.execute("INSERT INTO devices(device_id,token_sha,status,enrolled_at) "
                           "VALUES(?,?, 'pending', ?)", (device_id, digest(token), at))
            except Exception:
                return PairingError(409, "PAIRING_DEVICE_EXISTS")
            db.execute("UPDATE pairing_codes SET state='claimed', claim_sha=NULL, expires_at=? "
                       "WHERE pairing_id=?", (at + CONFIRM_TTL, row["pairing_id"]))
            _audit(db, at, "device", "pairing.claim", device_id, "success", ip)
            return {"status": "claimed", "device_id": device_id, "token": token}
        return self._run(work)

    def confirm(self, device_id, token, ip):
        def work(db, at):
            row = db.execute("SELECT * FROM devices WHERE device_id=? AND status='pending'",
                             (device_id,)).fetchone() if _valid_token_shape(device_id, token) else None
            if (not row or _matches(row, token) != "current"
                    or _limited(db, "devauth_ip", ip, at, record=False)):
                if _valid_token_shape(device_id, token):
                    _hit(db, "devauth_ip", ip, at)
                return PairingError(401, "DEVICE_AUTH_REQUIRED")
            db.execute("UPDATE devices SET status='active', last_seen_at=?, last_seen_ip=? WHERE device_id=?",
                       (at, ip, device_id))
            db.execute("UPDATE pairing_codes SET state='done' WHERE device_id=? AND state='claimed'",
                       (device_id,))
            _audit(db, at, "device", "pairing.confirm", device_id, "success", ip)
            return {"status": "active"}
        return self._run(work)

    def _device_auth(self, db, at, device_id, token, ip):
        """Authenticated active device row and matched credential, or None (counts failures)."""
        row = None
        if _valid_token_shape(device_id, token) and not _limited(db, "devauth_ip", ip, at, record=False):
            row = db.execute("SELECT * FROM devices WHERE device_id=? AND status='active'",
                             (device_id,)).fetchone()
        matched = _matches(row, token) if row else None
        if matched is None:
            if _valid_token_shape(device_id, token):
                _hit(db, "devauth_ip", ip, at)
            return None, None
        return row, matched

    def rotate(self, device_id, token, ip):
        def work(db, at):
            row, matched = self._device_auth(db, at, device_id, token, ip)
            if not row:
                return PairingError(401, "DEVICE_AUTH_REQUIRED")
            if _limited(db, "rotate_dev", device_id, at, record=True):
                _audit(db, at, "device", "token.rotate", device_id, "rate_limited", ip)
                return PairingError(429, "ROTATION_THROTTLED")
            new = secrets.token_urlsafe(32)
            if matched == "current":
                db.execute("UPDATE devices SET prev_token_sha=token_sha, prev_token_expires_at=?, "
                           "token_sha=?, token_rotated_at=? WHERE device_id=?",
                           (at + ROTATION_OVERLAP, digest(new), at, device_id))
            else:
                # Device never received the earlier replacement; keep the original deadline.
                db.execute("UPDATE devices SET token_sha=?, token_rotated_at=? WHERE device_id=?",
                           (digest(new), at, device_id))
            _audit(db, at, "device", "token.rotate", device_id, "success", ip)
            return {"token": new, "overlap_seconds": ROTATION_OVERLAP}
        return self._run(work)

    def confirm_rotation(self, device_id, token, ip):
        def work(db, at):
            row, matched = self._device_auth(db, at, device_id, token, ip)
            if not row or matched != "current":
                return PairingError(401, "DEVICE_AUTH_REQUIRED")
            db.execute("UPDATE devices SET prev_token_sha=NULL, prev_token_expires_at=0 WHERE device_id=?",
                       (device_id,))
            _audit(db, at, "device", "token.rotate_confirm", device_id, "success", ip)
            return {"status": "ok"}
        return self._run(work)


def _bearer(request):
    value = request.headers.get("authorization", "")
    return value[7:] if value.startswith("Bearer ") else ""


async def _body(request, **fields):
    """Parse a flat JSON object by hand so validation errors never echo secrets."""
    try:
        data = json.loads(await request.body() or b"{}")
    except ValueError:
        data = None
    if not isinstance(data, dict) or set(data) - set(fields):
        raise PairingError(422, "INVALID_PAIRING_REQUEST")
    out = {}
    for name, pattern in fields.items():
        value = data.get(name)
        if not isinstance(value, str) or not pattern.fullmatch(value):
            raise PairingError(422, "INVALID_PAIRING_REQUEST")
        out[name] = value
    return out


def _parent(request, *, write):
    check_origin(request)
    csrf = request.headers.get("x-csrf-token", "") if write else None
    return request.app.state.parent_auth.session(
        request.cookies.get(SESSION_COOKIE), csrf=csrf,
        csrf_cookie=request.cookies.get(CSRF_COOKIE), ip=client_ip(request))


def create_router() -> APIRouter:
    router = APIRouter(tags=["pairing"])

    def guarded(fn):
        @functools.wraps(fn)
        async def wrapper(request: Request, **path):
            try:
                result = await fn(request, **path)
            except (PairingError, AuthError) as error:
                raise HTTPException(error.status, error.code, headers={"Cache-Control": "no-store"}) from None
            return JSONResponse(result, headers={"Cache-Control": "no-store"})
        return wrapper

    def service(request):
        return request.app.state.pairing

    @router.post("/v1/parent/pairings")
    @guarded
    async def create(request: Request):
        return service(request).create_pairing(_parent(request, write=True), client_ip(request))

    @router.post("/v1/parent/pairings/{pairing_id}/redeem")
    @guarded
    async def redeem(request: Request, pairing_id: str):
        account = _parent(request, write=True)
        if not PAIRING_ID_RE.fullmatch(pairing_id):
            raise PairingError(404, "PAIRING_INVALID")
        body = await _body(request, code=CODE_RE)
        return service(request).redeem(account, pairing_id, body["code"], client_ip(request))

    @router.get("/v1/parent/devices")
    @guarded
    async def devices(request: Request):
        _parent(request, write=False)
        return {"devices": service(request).list_devices()}

    @router.post("/v1/parent/devices/{device_id}/revoke")
    @guarded
    async def revoke(request: Request, device_id: str):
        account = _parent(request, write=True)
        if not DEVICE_RE.fullmatch(device_id):
            raise PairingError(404, "DEVICE_NOT_FOUND")
        return service(request).revoke(account, device_id, client_ip(request))

    @router.post("/v1/pairing/start")
    @guarded
    async def start(request: Request):
        body = await _body(request, device_id=DEVICE_RE)
        return service(request).start(body["device_id"], client_ip(request))

    @router.post("/v1/pairing/claim")
    @guarded
    async def claim(request: Request):
        body = await _body(request, device_id=DEVICE_RE, claim_secret=re.compile(r"^[A-Za-z0-9_-]{16,64}$"))
        return service(request).claim(body["device_id"], body["claim_secret"], client_ip(request))

    @router.post("/v1/pairing/confirm")
    @guarded
    async def confirm(request: Request):
        body = await _body(request, device_id=DEVICE_RE)
        return service(request).confirm(body["device_id"], _bearer(request), client_ip(request))

    @router.post("/v1/devices/{device_id}/token/rotate")
    @guarded
    async def rotate(request: Request, device_id: str):
        return service(request).rotate(device_id, _bearer(request), client_ip(request))

    @router.post("/v1/devices/{device_id}/token/confirm")
    @guarded
    async def rotation_confirm(request: Request, device_id: str):
        return service(request).confirm_rotation(device_id, _bearer(request), client_ip(request))

    return router


class _ClientIpMiddleware:
    """Expose the client address to verify_token for last-used records."""

    def __init__(self, app):
        self.app = app

    async def __call__(self, scope, receive, send):
        if scope["type"] == "http":
            app = scope["app"]
            token = request_ip.set(client_ip(StarletteRequest(scope)) if hasattr(app.state, "settings") else None)
            try:
                await self.app(scope, receive, send)
            finally:
                request_ip.reset(token)
        else:
            await self.app(scope, receive, send)


def mount(app, filename):
    app.state.pairing = PairingService(filename)
    app.add_middleware(_ClientIpMiddleware)
    app.include_router(create_router())
