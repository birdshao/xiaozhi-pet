from fastapi import APIRouter, Header, HTTPException
from ..models import EventBatch, QuizAnswer, VoiceQuery
from ..intent import resolve


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

    return router
