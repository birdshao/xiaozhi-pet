"""FastAPI PetLearning MVP for a self-hosted XiaoZhi voice bridge.

Run only on loopback / behind an authenticated TLS reverse proxy.
Authentication is device-scoped, provisioned offline, and never managed by LLM.
"""
from __future__ import annotations

import os
from pathlib import Path

from fastapi import FastAPI, HTTPException
from fastapi.exception_handlers import request_validation_exception_handler
from fastapi.exceptions import RequestValidationError
from fastapi.responses import FileResponse, JSONResponse

from . import chat_provider, distribution
from .open_chat import OpenChat
from .config import Settings
from .auth import ParentAuth
from . import pairing
from .routers import device, voice, parent
from .quiz import QuizEngine
from .story_reader import StoryReader
from .store import PetStore
from . import panel, profile, sync
from .voice import VoiceTurns


def create_app(filename: str | Path | None = None, *, settings: Settings | None = None) -> FastAPI:
    settings = settings or Settings.load()
    settings.validate()
    database = filename or settings.database
    store = PetStore(database)
    quiz = QuizEngine(database)
    reader = StoryReader(database)
    app = FastAPI(
        title="xiaozhi-pet PetLearning MVP",
        version="0.2.0",
        docs_url=None if settings.production else "/docs",
        redoc_url=None if settings.production else "/redoc",
        openapi_url=None if settings.production else "/openapi.json",
        description="Authenticated pet replay receipts and reviewed local learning content.",
    )
    app.state.settings = settings
    app.state.parent_auth = ParentAuth(database, settings)
    app.state.pet_store = store
    app.state.quiz_engine = quiz
    app.state.story_reader = reader
    app.state.voice_turns = VoiceTurns(store, database, settings)
    # S6: swappable provider (tests put a FakeChatProvider here); read on every turn.
    app.state.chat_provider = chat_provider.from_settings(settings)

    @app.exception_handler(RequestValidationError)
    async def validation_error(request, exc):
        if request.url.path.startswith("/v1/parent/"):
            # FastAPI's default errors echo invalid input, including credentials.
            return JSONResponse(status_code=422, content={"detail": "INVALID_PARENT_REQUEST"},
                                headers={"Cache-Control": "no-store"})
        return await request_validation_exception_handler(request, exc)

    # Developer-only interactive preview. Disabled by default and static:
    # no token, content approval, storage or parental admin endpoints.
    # Bind uvicorn to loopback and use a separate test device ID.
    def playground_file(name: str, media_type: str):
        if settings.production or os.environ.get("KID_PET_ENABLE_PLAYGROUND", str(int(settings.playground))) != "1":
            raise HTTPException(status_code=404, detail="NOT_FOUND")
        file = Path(__file__).resolve().parent / "playground" / name
        response = FileResponse(file, media_type=media_type)
        response.headers["Cache-Control"] = "no-store"
        response.headers["X-Content-Type-Options"] = "nosniff"
        response.headers["Referrer-Policy"] = "no-referrer"
        response.headers["Permissions-Policy"] = "camera=(), microphone=(), geolocation=()"
        response.headers["Content-Security-Policy"] = (
            "default-src 'none'; script-src 'self'; style-src 'self'; "
            "connect-src 'self'; img-src 'self' data:; object-src 'none'; "
            "base-uri 'none'; frame-ancestors 'none'; form-action 'self'"
        )
        return response

    @app.get("/play", include_in_schema=False)
    def playground():
        return playground_file("index.html", "text/html; charset=utf-8")

    @app.get("/play/{asset}", include_in_schema=False)
    def playground_asset(asset: str):
        if asset not in {"play.js", "play.css"}:
            raise HTTPException(status_code=404, detail="NOT_FOUND")
        return playground_file(asset, "text/javascript" if asset == "play.js" else "text/css")

    @app.get("/healthz")
    def healthz() -> dict[str, str]:
        return {"status": "ok"}

    app.include_router(device.create_router(store, quiz, reader))
    app.include_router(voice.create_router(store, quiz, reader))
    app.include_router(parent.router)
    pairing.mount(app, database)
    app.include_router(sync.create_router(app, store))
    app.include_router(distribution.create_router(app, database))
    app.include_router(profile.create_router(app, database))
    app.include_router(panel.create_router(app, database))
    app.state.voice_turns.open_chat = OpenChat(database, settings, lambda: app.state.chat_provider,
                                               summary=app.state.state_sync.summary)

    return app


# Run locally via python -m uvicorn services.pet_learning.api:app
# Device enrollment / content publication remain trusted local CLI actions.
app = create_app()
