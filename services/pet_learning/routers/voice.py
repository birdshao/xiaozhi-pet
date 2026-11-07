from fastapi import APIRouter, Depends, Header, HTTPException
from ..pairing import device_auth
from ..models import EventBatch, QuizAnswer, VoiceQuery
from ..intent import resolve


def create_router(store, quiz, reader):
    router = APIRouter(dependencies=[Depends(device_auth)])
    @router.post("/v1/devices/{device_id}/intent")
    def classify_intent(device_id: str, query: VoiceQuery,
                        authorization: str | None = Header(default=None)) -> dict:
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
