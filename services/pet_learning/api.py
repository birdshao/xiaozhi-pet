"""FastAPI PetLearning MVP for a self-hosted XiaoZhi voice bridge.

Run only on loopback / behind an authenticated TLS reverse proxy.
Authentication is device-scoped, provisioned offline, and never managed by LLM.
"""
from __future__ import annotations

import os
from pathlib import Path
from typing import Literal

from fastapi import FastAPI, Header, HTTPException
from pydantic import BaseModel, ConfigDict, Field

from .intent import resolve
from .quiz import QuizEngine
from .store import PetStore


class StrictModel(BaseModel):
    model_config = ConfigDict(extra="forbid")


class EventPayload(StrictModel):
    event: str = Field(min_length=1, max_length=64)
    book_id: str | None = Field(default=None, max_length=64)


class EventEnvelope(StrictModel):
    schema_version: Literal["1.0"]
    message_id: str = Field(min_length=3, max_length=128, pattern=r"^[A-Za-z0-9:_-]+$")
    device_id: str = Field(min_length=3, max_length=64)
    seq: int = Field(ge=1, le=9223372036854775807)
    type: Literal["pet.event"]
    payload: EventPayload
    session_id: str | None = Field(default=None, max_length=80)
    sent_at_ms: int | None = Field(default=None, ge=0)


class EventBatch(StrictModel):
    events: list[EventEnvelope] = Field(min_length=1, max_length=32)


class VoiceQuery(StrictModel):
    text: str = Field(min_length=1, max_length=120)


class QuizAnswer(StrictModel):
    choice: Literal[1, 2]


def create_app(filename: str | Path | None = None) -> FastAPI:
    database = filename or os.environ.get("KID_PET_DB", "data/pet_learning.sqlite3")
    store = PetStore(database)
    quiz = QuizEngine(database)
    app = FastAPI(
        title="xiaozhi-pet PetLearning MVP",
        version="0.1.0",
        description="Authenticated pet replay receipts and reviewed local learning content.",
    )
    app.state.pet_store = store
    app.state.quiz_engine = quiz

    def require_device(device_id: str, authorization: str | None) -> None:
        # No user identity or bearer token is returned in error messages.
        if not authorization or not authorization.startswith("Bearer "):
            raise HTTPException(status_code=401, detail="DEVICE_AUTH_REQUIRED")
        token = authorization[7:]
        if not store.authenticated(device_id, token):
            raise HTTPException(status_code=401, detail="DEVICE_AUTH_REQUIRED")

    @app.get("/healthz")
    def healthz() -> dict[str, str]:
        return {"status": "ok"}

    @app.post("/v1/devices/{device_id}/events")
    def ingest(device_id: str, batch: EventBatch,
               authorization: str | None = Header(default=None)) -> dict:
        require_device(device_id, authorization)
        if any(x.device_id != device_id for x in batch.events):
            raise HTTPException(status_code=400, detail="DEVICE_ID_MISMATCH")
        # Each accepted event commits in SQLite before its ACK is returned.
        # A rejected event does not consume the next sequence number.
        return {"acks": [store.apply_event(device_id, item.model_dump(exclude_none=True))
                         for item in batch.events]}

    @app.get("/v1/devices/{device_id}/snapshot")
    def snapshot(device_id: str,
                 authorization: str | None = Header(default=None)) -> dict:
        require_device(device_id, authorization)
        return store.snapshot(device_id)

    @app.get("/v1/books/{book_id}/pages/{page}")
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

    @app.post("/v1/devices/{device_id}/quiz/start")
    def start_quiz(device_id: str, authorization: str | None = Header(default=None)) -> dict:
        require_device(device_id, authorization)
        if store.snapshot(device_id)["quiet"]:
            return {"kind": "fallback", "action": None,
                    "tts_text": "现在是安静时间，我们下次再一起玩吧。",
                    "learning_state": "idle"}
        return quiz.start(device_id)

    @app.post("/v1/devices/{device_id}/quiz/answer")
    def answer_quiz(device_id: str, answer: QuizAnswer,
                    authorization: str | None = Header(default=None)) -> dict:
        require_device(device_id, authorization)
        if store.snapshot(device_id)["quiet"]:
            return {"kind": "fallback", "action": None,
                    "tts_text": "现在是安静时间，我们下次再一起玩吧。",
                    "learning_state": "idle"}
        return quiz.answer(device_id, answer.choice)

    @app.get("/v1/devices/{device_id}/quiz/status")
    def quiz_status(device_id: str,
                    authorization: str | None = Header(default=None)) -> dict:
        require_device(device_id, authorization)
        return quiz.status(device_id)

    @app.post("/v1/devices/{device_id}/intent")
    def classify_intent(device_id: str, query: VoiceQuery,
                        authorization: str | None = Header(default=None)) -> dict:
        require_device(device_id, authorization)
        context = store.snapshot(device_id)
        learning_reply = quiz.intent(device_id, query.text, quiet=context["quiet"])
        if learning_reply is not None:
            return learning_reply
        return resolve(query.text, context, store.page)

    return app


# Run locally via python -m uvicorn services.pet_learning.api:app
# Device enrollment / content publication remain trusted local CLI actions.
app = create_app()
