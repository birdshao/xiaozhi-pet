from typing import Literal

from fastapi import APIRouter, Header, HTTPException, Request
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
        # Internal-only: the public proxy must never carry this route (§3.3 N6).
        peer = request.client.host if request.client else ""
        if (peer == request.app.state.settings.trusted_proxy
                or "x-forwarded-for" in request.headers or "forwarded" in request.headers):
            raise HTTPException(status_code=403, detail="VOICE_INTERNAL_ONLY")
        return turns.turn(device_id, await request.body())

    return router
