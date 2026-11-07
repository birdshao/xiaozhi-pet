import ipaddress
import json
from typing import Literal

from fastapi import APIRouter, Header, HTTPException, Request, Response
from pydantic import Field

from ..models import EventBatch, QuizAnswer, StrictModel, VoiceQuery
from ..voice import PageContext
from ..intent import resolve


class ReaderContext(StrictModel):
    scene: Literal["HOME", "READER", "GAME"]
    page: int = Field(ge=1, le=1000)
    content_id: int | None = Field(default=None, ge=0, le=65535)
    revision: int | None = Field(default=None, ge=0, le=65535)
    header_crc: str | None = Field(default=None, pattern=r"^[0-9a-f]{8}$")
    # Pre-S4 bridge to the legacy `pages` table; S4 resolves content_id/revision instead.
    book_id: str | None = Field(default=None, pattern=r"^[a-z0-9][a-z0-9_-]{2,63}$")


def create_router(store, quiz, reader):
    router = APIRouter()
    def require_device(device_id: str, authorization: str | None) -> None:
        # No user identity or bearer token is returned in error messages.
        if not authorization or not authorization.startswith("Bearer "):
            raise HTTPException(status_code=401, detail="DEVICE_AUTH_REQUIRED")
        token = authorization[7:]
        if not store.authenticated(device_id, token):
            raise HTTPException(status_code=401, detail="DEVICE_AUTH_REQUIRED")

    @router.post("/v1/devices/{device_id}/intent")
    def classify_intent(device_id: str, query: VoiceQuery,
                        authorization: str | None = Header(default=None)) -> dict:
        require_device(device_id, authorization)
        context = store.snapshot(device_id)
        learning_reply = quiz.intent(device_id, query.text, quiet=context["quiet"])
        if learning_reply is not None:
            return learning_reply
        # Preserve legacy STORY_OPEN/PAGE_NEXT PetStore behavior when an
        # existing device has already opened a book with Pet events.
        if context["mode"] == "STORY":
            legacy_reply = resolve(query.text, context, store.page)
            if legacy_reply["kind"] != "fallback":
                return legacy_reply
        story_reply = reader.intent(device_id, query.text, quiet=context["quiet"])
        if story_reply is not None:
            return story_reply
        return resolve(query.text, context, store.page)

    @router.put("/v1/devices/{device_id}/context")
    def put_context(device_id: str, body: ReaderContext, request: Request,
                    authorization: str | None = Header(default=None)) -> dict:
        require_device(device_id, authorization)
        request.app.state.voice_turns.contexts.put(device_id, PageContext(**body.model_dump()))
        return {"status": "ok", "expires_in": 120}

    @router.post("/v1/voice/{device_id}/turn")
    async def voice_turn(device_id: str, request: Request,
                         authorization: str | None = Header(default=None)) -> dict:
        turns = request.app.state.voice_turns
        scheme, _, token = (authorization or "").partition(" ")
        if scheme != "Bearer" or not turns.authorized(token):
            raise HTTPException(status_code=401, detail="VOICE_AUTH_REQUIRED")
        # Internal-only allowlist: the public proxy must never carry this route (§3.3 N6).
        settings = request.app.state.settings
        peer = request.client.host if request.client else ""
        try:
            address = ipaddress.ip_address(peer)
        except ValueError:
            address = None
        if (address is None or str(address) == settings.trusted_proxy
                or not any(address in net for net in settings.voice_peer_networks())
                or any(name in request.headers for name in
                       ("x-forwarded-for", "forwarded", "x-real-ip"))):
            raise HTTPException(status_code=403, detail="VOICE_INTERNAL_ONLY")
        body = await request.body()
        content = json.dumps(turns.turn(device_id, body), ensure_ascii=False,
                             separators=(",", ":")).encode("utf-8")
        # The guard verifies this before speaking: a rogue host cannot make the pet talk.
        return Response(content, media_type="application/json", headers={
            "X-Voice-Signature": turns.sign(turns.turn_id_of(body), content),
            "Cache-Control": "no-store"})

    return router
