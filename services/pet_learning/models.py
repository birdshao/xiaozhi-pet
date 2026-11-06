from typing import Literal
from pydantic import BaseModel, ConfigDict, Field


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


