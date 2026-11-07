"""Parent web panel routes (/parent/*): server-rendered HTML over the existing services.

Business logic stays in the S1-S3 services (pairing, sync, profile, notes). This module only
adds the browser edge: form login (password + TOTP), the same cookie session the JSON API uses,
form CSRF, and HTML responses. Only firmware-supported actions are exposed: no content packs,
voice packs, parent-settings push or open chat.
"""
from __future__ import annotations

import hashlib
import hmac
import json
import re
import secrets
import time
from pathlib import Path
from urllib.parse import parse_qs

from fastapi import APIRouter, HTTPException, Request
from fastapi.concurrency import run_in_threadpool
from fastapi.responses import HTMLResponse, RedirectResponse, Response
from pydantic import ValidationError

from .. import commands
from ..auth import AuthError, CSRF_COOKIE, SESSION_COOKIE, SESSION_SECONDS
from ..db import connect
from ..pairing import DEVICE_RE, PAIRING_ID_RE, CODE_RE, PairingError
# Notes are reached only through profile.py (the isolation test pins who imports notes.py).
from ..profile import InitBody, NotesBody, NotesError, ProfileBody, ProfileError, Reauth, validate_notes
from ..routers.parent import check_origin, client_ip
from ..sync import SyncError, TOMBSTONED  # noqa: F401  (TOMBSTONED documents the tombstone states shown)
from . import render
from .render import e

STATIC = Path(__file__).resolve().parent / "static"
LOGIN_CSRF_COOKIE = "__Host-pet_login_csrf"
LOGIN_CSRF_SECONDS = 1800
MAX_FORM_BYTES = 8192
PAGE_SIZE = 50
CSP = ("default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self' data:; "
       "connect-src 'none'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'; form-action 'self'")

# Flash messages are chosen by key from this table, so a query string never reaches the page text.
FLASH = {
    "pairing_approved": ("配对码正确，设备将在 2 分钟内自动完成配对。", "ok"),
    "revoked": ("设备已撤销。", "ok"),
    "profile_init": ("档案已初始化。性格需要在设备上确认后才会生效。", "ok"),
    "profile_saved": ("已保存。若修改了性格，请在设备上确认。", "ok"),
    "temperament_resent": ("已重新向设备请求确认性格。", "ok"),
    "clear_requested": ("服务器上的记忆已删除，已请求设备清空，请在设备上确认。", "ok"),
    "resumed": ("已恢复同步。", "ok"),
    "notes_saved": ("备注已保存。", "ok"),
    "notes_deleted": ("备注已删除。", "ok"),
    "logged_out": ("已退出登录。", "ok"),
}
ERRORS = {
    "PAIRING_THROTTLED": "操作过于频繁，请稍后再试。",
    "PAIRING_INVALID": "配对已失效，请重新开始配对。",
    "PAIRING_CODE_INVALID": "配对码不正确，请重新输入。",
    "PAIRING_VOIDED": "输错次数过多，本次配对已作废，请重新开始。",
    "DEVICE_NOT_FOUND": "找不到这台设备。",
    "DEVICE_NOT_ACTIVE": "设备已撤销，不能再操作。",
    "PET_NAME_INVALID": "名字只能是 1–6 个汉字或 1–12 个英文字母。",
    "TEMPERAMENT_INVALID": "请选择有效的性格。",
    "ALREADY_INITIALIZED": "档案已经初始化。",
    "NOT_INITIALIZED": "请先初始化档案。",
    "PROFILE_REV_CONFLICT": "档案刚刚被修改过，请刷新页面后重试。",
    "NOTHING_TO_UPDATE": "没有需要保存的修改。",
    "CLEAR_PENDING": "上一次清空请求还在等待设备确认。",
    "NOTHING_TO_RESUME": "当前没有需要恢复的同步。",
    "NOTES_INVALID": "备注内容不合法（昵称最多 20 字，生日为 月-日，备注最多 500 字，不能含控制字符）。",
    "NOTES_KEY_MISSING": "未配置备注加密密钥，备注不可用。",
    "NOTES_UNREADABLE": "备注无法解密，请联系管理员。",
    "PARENT_REAUTH_FAILED": "密码或验证码不正确（验证码不能重复使用，请等待刷新）。",
    "PARENT_LOGIN_THROTTLED": "尝试次数过多，请 15 分钟后再试。",
    "PARENT_AUTH_REQUIRED": "用户名、密码或验证码不正确。",
}


def _headers(response: Response) -> Response:
    response.headers["Cache-Control"] = "no-store"
    response.headers["Pragma"] = "no-cache"
    response.headers["X-Content-Type-Options"] = "nosniff"
    # Not "no-referrer": browsers then send "Origin: null" on form POSTs, which check_origin must refuse.
    response.headers["Referrer-Policy"] = "same-origin"
    response.headers["X-Frame-Options"] = "DENY"
    response.headers["Permissions-Policy"] = "camera=(), microphone=(), geolocation=()"
    response.headers["Content-Security-Policy"] = CSP
    return response


def page(body: str, status: int = 200) -> HTMLResponse:
    return _headers(HTMLResponse(str(body), status_code=status))


def redirect(url: str, *, flash: str | None = None) -> Response:
    if flash:
        url += ("&" if "?" in url else "?") + "msg=" + flash
    return _headers(RedirectResponse(url, status_code=303))


def error_page(status: int, text: str, back: str = "/parent/devices") -> HTMLResponse:
    return page(render.message_page("出错了", text, back=back), status)


async def read_form(request: Request) -> dict[str, str]:
    body = b""
    async for chunk in request.stream():
        body += chunk
        if len(body) > MAX_FORM_BYTES:
            raise HTTPException(413, "FORM_TOO_LARGE")
    if not body:  # no fields at all: the CSRF check then refuses it
        return {}
    if (request.headers.get("content-type") or "").split(";")[0].strip().lower() != "application/x-www-form-urlencoded":
        raise HTTPException(415, "UNSUPPORTED_MEDIA_TYPE")
    try:
        parsed = parse_qs(body.decode("utf-8"), keep_blank_values=True, max_num_fields=20)
    except (UnicodeDecodeError, ValueError):
        raise HTTPException(400, "BAD_FORM") from None
    return {k: v[-1] for k, v in parsed.items()}


def _login_csrf_field(request: Request, cookie: str) -> str:
    key = hashlib.sha256(b"panel-login-csrf-v1" + request.app.state.settings.totp_key.encode()).digest()
    return hmac.new(key, cookie.encode(), hashlib.sha256).hexdigest()


class Panel:
    def __init__(self, app, database) -> None:
        self.app = app
        self.database = database
        self.clock = time.time

    # -- session ---------------------------------------------------------

    def session(self, request: Request, fields: dict[str, str] | None = None):
        """Account id, or a redirect/error response. Writes (fields given) also need the CSRF field."""
        try:
            check_origin(request)
        except HTTPException:
            return error_page(403, "请求来源不被允许。")
        try:
            return request.app.state.parent_auth.session(
                request.cookies.get(SESSION_COOKIE),
                csrf=None if fields is None else fields.get("csrf", ""),
                csrf_cookie=request.cookies.get(CSRF_COOKIE), ip=client_ip(request))
        except AuthError as error:
            if error.status == 403:
                return error_page(403, "页面已过期或请求被拒绝，请刷新后重试。")
            return redirect("/parent/login")

    def csrf(self, request: Request) -> str:
        return request.cookies.get(CSRF_COOKIE, "")

    def tz(self) -> str:
        return self.app.state.settings.timezone

    def notice(self, request: Request):
        return FLASH.get(request.query_params.get("msg", ""))

    # -- data ------------------------------------------------------------

    def device(self, device_id: str) -> dict | None:
        if not DEVICE_RE.fullmatch(device_id):
            return None
        return next((d for d in self.app.state.pairing.list_devices() if d["device_id"] == device_id), None)

    def recent_commands(self, db, device_id: str, awaiting: list[int]) -> list[dict]:
        rows = db.execute("SELECT * FROM commands WHERE device_id=? ORDER BY seq DESC LIMIT 15",
                          (device_id,)).fetchall()
        return [commands.status(dict(r), awaiting) for r in rows]

    def state_row(self, db, device_id: str) -> dict | None:
        row = db.execute("SELECT * FROM device_state WHERE device_id=?", (device_id,)).fetchone()
        if row is None:
            return None
        state = dict(row)
        state["pet"] = json.loads(state["pet_json"])
        state["awaiting"] = json.loads(state["awaiting_json"])
        return state

    def overview_rows(self) -> list[dict]:
        now = int(self.clock())
        out = []
        with connect(self.database) as db:
            db.execute("BEGIN IMMEDIATE")
            self.app.state.state_sync._expire(db, now)
            for device in self.app.state.pairing.list_devices():
                state = self.state_row(db, device["device_id"])
                profile = db.execute("SELECT pet_name FROM profiles WHERE device_id=?",
                                     (device["device_id"],)).fetchone()
                out.append({**device, "state": state, "pet_name": profile[0] if profile else None,
                            "awaiting": len(state["awaiting"]) if state else 0})
            db.commit()
        return out

    def detail(self, device: dict) -> dict:
        now = int(self.clock())
        with connect(self.database) as db:
            db.execute("BEGIN IMMEDIATE")
            self.app.state.state_sync._expire(db, now, device["device_id"])
            state = self.state_row(db, device["device_id"])
            items = self.recent_commands(db, device["device_id"], state["awaiting"] if state else [])
            db.commit()
        return {"device": device, "state": state, "commands": items}

    def audit_rows(self, page_no: int) -> tuple[list[dict], bool]:
        with connect(self.database) as db:
            rows = db.execute("SELECT at,actor,action,target,result,ip FROM audit_log ORDER BY id DESC LIMIT ? OFFSET ?",
                              (PAGE_SIZE + 1, (page_no - 1) * PAGE_SIZE)).fetchall()
        return [dict(r) for r in rows[:PAGE_SIZE]], len(rows) > PAGE_SIZE

    def pairing_state(self, account_id: int, pairing_id: str) -> str | None:
        with connect(self.database) as db:
            row = db.execute("SELECT state, expires_at FROM pairing_codes WHERE pairing_id=? AND account_id=?",
                             (pairing_id, account_id)).fetchone()
        if row is None:
            return None
        if row["expires_at"] <= int(self.clock()) and row["state"] in {"open", "code_issued", "approved", "claimed"}:
            return "expired"
        return row["state"]


def service_error(error, back: str):
    status = getattr(error, "status", 400)
    return error_page(status if status in {400, 401, 403, 404, 409, 410, 422, 429, 503} else 400,
                      ERRORS.get(error.code, "操作失败。"), back)


def create_router(app, database) -> APIRouter:
    panel = Panel(app, database)
    app.state.parent_panel = panel
    router = APIRouter(prefix="/parent", include_in_schema=False)

    # -- static ----------------------------------------------------------

    @router.get("/static/{name}")
    def static(name: str):
        types = {"panel.css": "text/css; charset=utf-8", "panel.js": "text/javascript; charset=utf-8"}
        if name not in types:
            return _headers(Response("Not found", status_code=404))
        return _headers(Response((STATIC / name).read_bytes(), media_type=types[name]))

    # -- login -----------------------------------------------------------

    def login_response(request: Request, error: str | None = None, status: int = 200) -> Response:
        token = secrets.token_urlsafe(24)
        response = page(render.login_page(_login_csrf_field(request, token), error), status)
        response.set_cookie(LOGIN_CSRF_COOKIE, token, max_age=LOGIN_CSRF_SECONDS, secure=True, httponly=True,
                            samesite="strict", path="/")
        return response

    @router.get("/login")
    def login_form(request: Request):
        return login_response(request, "已退出登录。" if request.query_params.get("msg") == "logged_out" else None)

    @router.post("/login")
    async def login(request: Request):
        try:
            check_origin(request)
        except HTTPException:
            return error_page(403, "请求来源不被允许。", "/parent/login")
        fields = await read_form(request)
        cookie = request.cookies.get(LOGIN_CSRF_COOKIE, "")
        if not cookie or len(cookie) > 128 or not hmac.compare_digest(
                fields.get("csrf", "").encode(), _login_csrf_field(request, cookie).encode()):
            return login_response(request, "页面已过期，请重新输入。", 403)
        username, password, otp = fields.get("username", ""), fields.get("password", ""), fields.get("otp", "").strip()
        if not (3 <= len(username) <= 64 and re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]+", username)
                and 1 <= len(password) <= 256 and re.fullmatch(r"[0-9]{6}|[0-9a-f]{32}", otp)):
            return login_response(request, ERRORS["PARENT_AUTH_REQUIRED"], 401)
        try:
            token, csrf = await run_in_threadpool(request.app.state.parent_auth.login, username, password, otp,
                                                  client_ip(request))
        except AuthError as error:
            return login_response(request, ERRORS.get(error.code, "登录失败。"), error.status)
        response = redirect("/parent/devices")
        response.set_cookie(SESSION_COOKIE, token, max_age=SESSION_SECONDS, secure=True, httponly=True,
                            samesite="strict", path="/")
        response.set_cookie(CSRF_COOKIE, csrf, max_age=SESSION_SECONDS, secure=True, httponly=False,
                            samesite="strict", path="/")
        response.delete_cookie(LOGIN_CSRF_COOKIE, secure=True, httponly=True, samesite="strict", path="/")
        return response

    @router.post("/logout")
    async def logout(request: Request):
        fields = await read_form(request)
        try:
            check_origin(request)
            request.app.state.parent_auth.session(
                request.cookies.get(SESSION_COOKIE), csrf=fields.get("csrf", ""),
                csrf_cookie=request.cookies.get(CSRF_COOKIE), logout=True, ip=client_ip(request))
        except HTTPException:
            return error_page(403, "请求来源不被允许。", "/parent/login")
        except AuthError as error:
            if error.status == 403:
                return error_page(403, "页面已过期，请刷新后重试。")
            return redirect("/parent/login")
        response = redirect("/parent/login", flash="logged_out")
        for name in (SESSION_COOKIE, CSRF_COOKIE):
            response.delete_cookie(name, secure=True, httponly=name == SESSION_COOKIE, samesite="strict", path="/")
        return response

    @router.get("")
    @router.get("/")
    def index(request: Request):
        return redirect("/parent/devices")

    # -- helpers ---------------------------------------------------------

    def get_page(request: Request):
        account = panel.session(request)
        return account if isinstance(account, Response) else None

    async def write(request: Request):
        """(account, fields) or a response."""
        try:
            fields = await read_form(request)
        except HTTPException as error:
            return error_page(error.status_code, "表单无效。")
        account = panel.session(request, fields)
        return account if isinstance(account, Response) else (account, fields)

    def find(device_id: str):
        return panel.device(device_id)

    def not_found():
        return error_page(404, ERRORS["DEVICE_NOT_FOUND"])

    # -- devices ---------------------------------------------------------

    @router.get("/devices")
    def devices(request: Request):
        if (denied := get_page(request)) is not None:
            return denied
        rows = panel.overview_rows()
        body = render.device_list(rows, int(panel.clock()), panel.tz())
        return page(render.layout("设备", body, active="devices", csrf=panel.csrf(request),
                                  notice=panel.notice(request)))

    @router.get("/devices/{device_id}")
    def device_detail(device_id: str, request: Request):
        if (denied := get_page(request)) is not None:
            return denied
        device = find(device_id)
        if device is None:
            return not_found()
        csrf = panel.csrf(request)
        body = render.device_overview(panel.detail(device), int(panel.clock()), panel.tz(), csrf)
        return page(render.layout(f"设备 {device_id}", body, active="devices", csrf=csrf, notice=panel.notice(request)))

    @router.post("/devices/{device_id}/revoke")
    async def revoke(device_id: str, request: Request):
        got = await write(request)
        if isinstance(got, Response):
            return got
        if find(device_id) is None:
            return not_found()
        try:
            request.app.state.pairing.revoke(got[0], device_id, client_ip(request))
        except PairingError as error:
            return service_error(error, f"/parent/devices/{device_id}")
        return redirect(f"/parent/devices/{device_id}", flash="revoked")

    # -- pairing ---------------------------------------------------------

    @router.get("/pairing")
    def pairing(request: Request):
        if (denied := get_page(request)) is not None:
            return denied
        return page(render.layout("添加设备", render.pairing_start(panel.csrf(request)), active="pairing",
                                  csrf=panel.csrf(request)))

    @router.post("/pairings")
    async def pairing_create(request: Request):
        got = await write(request)
        if isinstance(got, Response):
            return got
        try:
            result = request.app.state.pairing.create_pairing(got[0], client_ip(request))
        except PairingError as error:
            return service_error(error, "/parent/pairing")
        return redirect(f"/parent/pairings/{result['pairing_id']}")

    @router.get("/pairings/{pairing_id}")
    def pairing_status(pairing_id: str, request: Request):
        account = panel.session(request)
        if isinstance(account, Response):
            return account
        state = panel.pairing_state(account, pairing_id) if PAIRING_ID_RE.fullmatch(pairing_id) else None
        if state is None:
            return error_page(404, ERRORS["PAIRING_INVALID"], "/parent/pairing")
        csrf = panel.csrf(request)
        return page(render.layout("添加设备", render.pairing_wait(pairing_id, state, csrf), active="pairing",
                                  csrf=csrf, notice=panel.notice(request)))

    @router.post("/pairings/{pairing_id}/redeem")
    async def pairing_redeem(pairing_id: str, request: Request):
        got = await write(request)
        if isinstance(got, Response):
            return got
        account, fields = got
        code = fields.get("code", "").strip()
        back = f"/parent/pairings/{pairing_id}"
        if not PAIRING_ID_RE.fullmatch(pairing_id):
            return error_page(404, ERRORS["PAIRING_INVALID"], "/parent/pairing")
        if not CODE_RE.fullmatch(code):
            return error_page(422, ERRORS["PAIRING_CODE_INVALID"], back)
        try:
            request.app.state.pairing.redeem(account, pairing_id, code, client_ip(request))
        except PairingError as error:
            return service_error(error, back)
        return redirect(back, flash="pairing_approved")

    # -- profile ---------------------------------------------------------

    @router.get("/devices/{device_id}/profile")
    def profile(device_id: str, request: Request):
        if (denied := get_page(request)) is not None:
            return denied
        device = find(device_id)
        if device is None:
            return not_found()
        try:
            view = request.app.state.profile_service.get_profile(device_id)
        except ProfileError as error:
            return service_error(error, "/parent/devices")
        csrf = panel.csrf(request)
        return page(render.layout("宠物档案", render.profile_page(device, view, panel.tz(), csrf), active="devices",
                                  csrf=csrf, notice=panel.notice(request)))

    @router.post("/devices/{device_id}/profile/init")
    async def profile_init(device_id: str, request: Request):
        got = await write(request)
        if isinstance(got, Response):
            return got
        account, fields = got
        back = f"/parent/devices/{device_id}/profile"
        if find(device_id) is None:
            return not_found()
        try:
            body = InitBody(pet_name=fields.get("pet_name", ""), temperament=fields.get("temperament", ""))
            request.app.state.profile_service.init(device_id, body, client_ip(request), f"parent:{account}")
        except ValidationError:
            return error_page(422, ERRORS["PET_NAME_INVALID"], back)
        except ProfileError as error:
            return service_error(error, back)
        return redirect(back, flash="profile_init")

    @router.post("/devices/{device_id}/profile")
    async def profile_update(device_id: str, request: Request):
        got = await write(request)
        if isinstance(got, Response):
            return got
        account, fields = got
        back = f"/parent/devices/{device_id}/profile"
        if find(device_id) is None:
            return not_found()
        service = request.app.state.profile_service
        resend = fields.get("resend") == "1"
        try:
            current = service.get_profile(device_id)
            base_rev = int(fields.get("base_rev", ""))
            name, trait = fields.get("pet_name", ""), fields.get("temperament", "")
            if resend:  # explicit re-request of the wanted temperament, nothing else changes
                body = ProfileBody(base_rev=base_rev, temperament=current["temperament"])
            else:
                body = ProfileBody(
                    base_rev=base_rev,
                    pet_name=name if name != current["pet_name"] else None,
                    temperament=trait if trait != current["temperament"] else None)
            service.update_profile(device_id, body, client_ip(request), f"parent:{account}")
        except (ValueError, ValidationError):
            return error_page(422, ERRORS["PET_NAME_INVALID"], back)
        except ProfileError as error:
            return service_error(error, back)
        return redirect(back, flash="temperament_resent" if resend else "profile_saved")

    # -- memory ----------------------------------------------------------

    @router.get("/devices/{device_id}/memory")
    def memory(device_id: str, request: Request):
        if (denied := get_page(request)) is not None:
            return denied
        device = find(device_id)
        if device is None:
            return not_found()
        view = request.app.state.state_sync.memories(device_id)
        csrf = panel.csrf(request)
        return page(render.layout("记忆", render.memory_page(device, view, panel.tz(), csrf), active="devices",
                                  csrf=csrf, notice=panel.notice(request)))

    async def memory_action(device_id: str, request: Request, action: str, flash: str):
        got = await write(request)
        if isinstance(got, Response):
            return got
        back = f"/parent/devices/{device_id}/memory"
        device = find(device_id)
        if device is None:
            return not_found()
        if device["status"] != "active":
            return error_page(409, ERRORS["DEVICE_NOT_ACTIVE"], back)
        try:
            getattr(request.app.state.state_sync, action)(device_id, client_ip(request))
        except SyncError as error:
            return service_error(error, back)
        return redirect(back, flash=flash)

    @router.post("/devices/{device_id}/memory/clear")
    async def memory_clear(device_id: str, request: Request):
        return await memory_action(device_id, request, "clear", "clear_requested")

    @router.post("/devices/{device_id}/memory/resume")
    async def memory_resume(device_id: str, request: Request):
        return await memory_action(device_id, request, "resume", "resumed")

    # -- notes -----------------------------------------------------------

    def load_notes(request: Request, device_id: str):
        try:
            return request.app.state.profile_service.get_notes(device_id)
        except NotesError as error:
            if error.code == "NOTES_KEY_MISSING":
                return None
            raise

    @router.get("/devices/{device_id}/notes")
    def notes(device_id: str, request: Request):
        if (denied := get_page(request)) is not None:
            return denied
        device = find(device_id)
        if device is None:
            return not_found()
        try:
            data = load_notes(request, device_id)
        except NotesError as error:
            return service_error(error, f"/parent/devices/{device_id}")
        csrf = panel.csrf(request)
        return page(render.layout("家长备注", render.notes_page(device, data, panel.tz(), csrf), active="devices",
                                  csrf=csrf, notice=panel.notice(request)))

    def proof_of(fields: dict[str, str]) -> Reauth:
        return Reauth(password=fields.get("reauth_password", ""), otp=fields.get("reauth_otp", "").strip())

    def step_up(request: Request, account: int, proof: Reauth) -> None:
        request.app.state.parent_auth.reauth(account, proof.password, proof.otp, client_ip(request))

    @router.post("/devices/{device_id}/notes")
    async def notes_save(device_id: str, request: Request):
        got = await write(request)
        if isinstance(got, Response):
            return got
        account, fields = got
        back = f"/parent/devices/{device_id}/notes"
        device = find(device_id)
        if device is None:
            return not_found()
        service = request.app.state.profile_service
        nickname = fields.get("nickname", "").strip() or None
        birthday = fields.get("birthday", "").strip() or None
        note = fields.get("note", "").replace("\r\n", "\n").strip() or None
        if nickname is None and birthday is None and note is None:
            return error_page(422, ERRORS["NOTES_INVALID"], back)

        def rerender(code: str, status: int):
            data = load_notes(request, device_id)
            values = {"nickname": nickname or "", "birthday": birthday or "", "note": note or ""}
            csrf = panel.csrf(request)
            return page(render.layout("家长备注", render.notes_page(device, data, panel.tz(), csrf, values),
                                      active="devices", csrf=csrf, notice=(ERRORS.get(code, "操作失败。"), "bad")),
                        status)
        try:
            validate_notes(nickname, birthday, note)  # a malformed form never spends a TOTP step
            service.require_device(device_id)
            proof = proof_of(fields)
            body = NotesBody(nickname=nickname, birthday=birthday, note=note, reauth=proof)
            await run_in_threadpool(step_up, request, account, proof)
            service.put_notes(device_id, body, client_ip(request), f"parent:{account}")
        except (NotesError, ProfileError, AuthError) as error:
            return rerender(error.code, error.status)
        except ValidationError:
            return rerender("PARENT_REAUTH_FAILED", 401)
        return redirect(back, flash="notes_saved")

    @router.post("/devices/{device_id}/notes/delete")
    async def notes_delete(device_id: str, request: Request):
        got = await write(request)
        if isinstance(got, Response):
            return got
        account, fields = got
        back = f"/parent/devices/{device_id}/notes"
        device = find(device_id)
        if device is None:
            return not_found()
        service = request.app.state.profile_service
        try:
            service.require_device(device_id, write=False)
            await run_in_threadpool(step_up, request, account, proof_of(fields))
            service.delete_notes(device_id, client_ip(request), f"parent:{account}")
        except (NotesError, ProfileError, AuthError) as error:
            return service_error(error, back)
        except ValidationError:
            return service_error(AuthError(401, "PARENT_REAUTH_FAILED"), back)
        return redirect(back, flash="notes_deleted")

    # -- audit -----------------------------------------------------------

    @router.get("/audit")
    def audit(request: Request):
        if (denied := get_page(request)) is not None:
            return denied
        try:
            page_no = min(max(int(request.query_params.get("page", "1")), 1), 10_000)
        except ValueError:
            page_no = 1
        rows, more = panel.audit_rows(page_no)
        return page(render.layout("审计日志", render.audit_page(rows, panel.tz(), page_no, more), active="audit",
                                  csrf=panel.csrf(request)))

    return router

