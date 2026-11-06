"""Parent identity is cookie-only and never accepts device bearer tokens."""
import ipaddress

from fastapi import APIRouter, HTTPException, Request, Response
from pydantic import Field

from ..auth import AuthError, CSRF_COOKIE, SESSION_COOKIE, SESSION_SECONDS
from ..models import StrictModel

router = APIRouter(prefix="/v1/parent", tags=["parent"])


class Login(StrictModel):
    username: str = Field(min_length=3, max_length=64, pattern=r"^[A-Za-z0-9][A-Za-z0-9_.-]+$")
    password: str = Field(min_length=1, max_length=256, repr=False)
    otp: str = Field(min_length=6, max_length=32, pattern=r"^(?:[0-9]{6}|[0-9a-f]{32})$", repr=False)


def client_ip(request):
    peer = request.client.host if request.client else "unknown"
    if peer == request.app.state.settings.trusted_proxy:
        forwarded = request.headers.get("x-forwarded-for", "")
        # The trusted edge must overwrite XFF with a single client address.
        try:
            return str(ipaddress.ip_address(forwarded))
        except ValueError:
            pass
    return peer


def check_origin(request):
    origin = request.headers.get("origin")
    expected = request.app.state.settings.public_origin.rstrip("/") or str(request.base_url).rstrip("/")
    if origin is not None and origin != expected:
        raise HTTPException(403, "PARENT_ORIGIN_REJECTED")


@router.post("/login")
def login(body: Login, request: Request, response: Response):
    check_origin(request)
    try:
        token, csrf = request.app.state.parent_auth.login(
            body.username, body.password, body.otp, client_ip(request))
    except AuthError as error:
        raise HTTPException(error.status, error.code, headers={"Cache-Control": "no-store"}) from None
    response.set_cookie(SESSION_COOKIE, token, max_age=SESSION_SECONDS,
                        secure=True, httponly=True, samesite="strict", path="/")
    response.set_cookie(CSRF_COOKIE, csrf, max_age=SESSION_SECONDS,
                        secure=True, httponly=False, samesite="strict", path="/")
    response.headers["Cache-Control"] = "no-store"
    return {"status": "ok", "csrf_token": csrf}


@router.post("/logout")
def logout(request: Request, response: Response):
    check_origin(request)
    try:
        request.app.state.parent_auth.session(
            request.cookies.get(SESSION_COOKIE), csrf=request.headers.get("x-csrf-token"),
            csrf_cookie=request.cookies.get(CSRF_COOKIE), logout=True, ip=client_ip(request))
    except AuthError as error:
        raise HTTPException(error.status, error.code, headers={"Cache-Control": "no-store"}) from None
    for name in (SESSION_COOKIE, CSRF_COOKIE):
        response.delete_cookie(name, secure=True, httponly=name == SESSION_COOKIE, samesite="strict", path="/")
    response.headers["Cache-Control"] = "no-store"
    return {"status": "ok"}
