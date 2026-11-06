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

    @router.post("/v1/devices/{device_id}/events")
    def ingest(device_id: str, batch: EventBatch,
               authorization: str | None = Header(default=None)) -> dict:
        require_device(device_id, authorization)
        if any(x.device_id != device_id for x in batch.events):
            raise HTTPException(status_code=400, detail="DEVICE_ID_MISMATCH")
        # Each accepted event commits in SQLite before its ACK is returned.
        # A rejected event does not consume the next sequence number.
        return {"acks": [store.apply_event(device_id, item.model_dump(exclude_none=True))
                         for item in batch.events]}

    @router.get("/v1/devices/{device_id}/snapshot")
    def snapshot(device_id: str,
                 authorization: str | None = Header(default=None)) -> dict:
        require_device(device_id, authorization)
        return store.snapshot(device_id)

    @router.get("/v1/books/{book_id}/pages/{page}")
    def approved_page(book_id: str, page: int,
                      x_device_id: str | None = Header(default=None),
                      authorization: str | None = Header(default=None)) -> dict:
        if x_device_id is None:
            raise HTTPException(status_code=401, detail="DEVICE_AUTH_REQUIRED")
        require_device(x_device_id, authorization)
        item = store.page(book_id, page)
        if item is None:
            # Treat unapproved and nonexistent content identically.
            raise HTTPException(status_code=404, detail="APPROVED_PAGE_NOT_FOUND")
        return item

    @router.post("/v1/devices/{device_id}/quiz/start")
    def start_quiz(device_id: str, authorization: str | None = Header(default=None)) -> dict:
        require_device(device_id, authorization)
        if store.snapshot(device_id)["quiet"]:
            return {"kind": "fallback", "action": None,
                    "tts_text": "现在是安静时间，我们下次再一起玩吧。",
                    "learning_state": "idle"}
        return quiz.start(device_id)

    @router.post("/v1/devices/{device_id}/quiz/answer")
    def answer_quiz(device_id: str, answer: QuizAnswer,
                    authorization: str | None = Header(default=None)) -> dict:
        require_device(device_id, authorization)
        if store.snapshot(device_id)["quiet"]:
            return {"kind": "fallback", "action": None,
                    "tts_text": "现在是安静时间，我们下次再一起玩吧。",
                    "learning_state": "idle"}
        return quiz.answer(device_id, answer.choice)

    @router.get("/v1/devices/{device_id}/quiz/status")
    def quiz_status(device_id: str,
                    authorization: str | None = Header(default=None)) -> dict:
        require_device(device_id, authorization)
        return quiz.status(device_id)

    @router.post("/v1/devices/{device_id}/story/start")
    def start_story(device_id: str, authorization: str | None = Header(default=None)) -> dict:
        require_device(device_id, authorization)
        if store.snapshot(device_id)["quiet"]:
            return {"kind":"fallback","action":None,"tts_text":"现在是安静时间，我们下次再读吧。","story_state":"idle"}
        return reader.start(device_id)

    @router.post("/v1/devices/{device_id}/story/next")
    def next_story(device_id: str, authorization: str | None = Header(default=None)) -> dict:
        require_device(device_id, authorization)
        if store.snapshot(device_id)["quiet"]:
            return {"kind":"fallback","action":None,"tts_text":"现在是安静时间，我们下次再读吧。","story_state":"idle"}
        return reader.next(device_id)

    @router.post("/v1/devices/{device_id}/story/repeat")
    def repeat_story(device_id: str, authorization: str | None = Header(default=None)) -> dict:
        require_device(device_id, authorization)
        if store.snapshot(device_id)["quiet"]:
            return {"kind":"fallback","action":None,"tts_text":"现在是安静时间，我们下次再读吧。","story_state":"idle"}
        return reader.repeat(device_id)

    @router.post("/v1/devices/{device_id}/story/stop")
    def stop_story(device_id: str, authorization: str | None = Header(default=None)) -> dict:
        require_device(device_id, authorization)
        return reader.stop(device_id)

    @router.get("/v1/devices/{device_id}/story/status")
    def story_status(device_id: str, authorization: str | None = Header(default=None)) -> dict:
        require_device(device_id, authorization)
        return reader.status(device_id)

    return router
