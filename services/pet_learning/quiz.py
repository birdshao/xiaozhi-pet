"""Approved two-choice learning game for ages 3–5.

Server-owned learning progress is deliberately separate from ESP32 Pet rewards.
Only parent-reviewed content is spoken; no raw speech or child PII is stored.
"""
from __future__ import annotations

from contextlib import contextmanager
from pathlib import Path
import re
import sqlite3
from typing import Any

CARD_RE = re.compile(r"^[a-z][a-z0-9_-]{2,63}$")
START_PHRASES = frozenset(("猜动物", "玩猜动物", "我们猜动物", "来猜动物", "再来一题"))
FIRST_PHRASES = frozenset(("第一个", "选第一个", "我选第一个", "选一", "我选一", "一"))
SECOND_PHRASES = frozenset(("第二个", "选第二个", "我选第二个", "选二", "我选二", "二"))

def _speech(text: str) -> str:
    return re.sub(r"[\s，。！？!?,.、]+", "", text).lower()


def _reply(text: str, *, kind: str = "approved_content",
           state: str = "idle") -> dict[str, Any]:
    return {"kind": kind, "action": None, "tts_text": text, "learning_state": state}


class QuizEngine:
    """Transactional, deterministic learning game. No LLM scoring or incentives."""

    def __init__(self, filename: str | Path) -> None:
        self.path = str(filename)
        if self.path != ":memory:":
            Path(self.path).parent.mkdir(parents=True, exist_ok=True)
        with self._db() as db:
            db.executescript("""
                CREATE TABLE IF NOT EXISTS quiz_cards (
                    card_id TEXT PRIMARY KEY, prompt TEXT NOT NULL,
                    first_text TEXT NOT NULL, second_text TEXT NOT NULL,
                    correct INTEGER NOT NULL CHECK(correct IN (1,2)),
                    success_text TEXT NOT NULL, retry_text TEXT NOT NULL,
                    approved INTEGER NOT NULL DEFAULT 0
                );
                CREATE TABLE IF NOT EXISTS quiz_sessions (
                    device_id TEXT PRIMARY KEY,
                    active_card TEXT,
                    attempts INTEGER NOT NULL DEFAULT 0,
                    next_cursor INTEGER NOT NULL DEFAULT 0,
                    completed INTEGER NOT NULL DEFAULT 0
                );
            """)

    @contextmanager
    def _db(self):
        db = sqlite3.connect(self.path, timeout=5, isolation_level=None)
        try:
            db.row_factory = sqlite3.Row
            db.execute("PRAGMA busy_timeout=5000")
            with db:
                yield db
        finally:
            db.close()

    def import_card(self, card_id: str, prompt: str, first: str, second: str,
                    correct: int, success: str, retry: str,
                    *, approved: bool = False) -> None:
        """Trusted parent/editor action; deliberately no HTTP publication API."""
        if not CARD_RE.fullmatch(card_id) or correct not in (1, 2):
            raise ValueError("Invalid card identifier or correct choice")
        for value, maximum in ((prompt, 90), (first, 35), (second, 35),
                               (success, 100), (retry, 100)):
            if not value or not value.strip() or len(value) > maximum or (
                    "\n" in value or "\r" in value):
                raise ValueError("Invalid reviewed text length")
        with self._db() as db:
            db.execute("BEGIN IMMEDIATE")
            db.execute(
                "INSERT INTO quiz_cards VALUES(?,?,?,?,?,?,?,?) "
                "ON CONFLICT(card_id) DO UPDATE SET "
                "prompt=excluded.prompt,first_text=excluded.first_text,"
                "second_text=excluded.second_text,correct=excluded.correct,"
                "success_text=excluded.success_text,retry_text=excluded.retry_text,"
                "approved=excluded.approved",
                (card_id, prompt.strip(), first.strip(), second.strip(), correct,
                 success.strip(), retry.strip(), int(approved)),
            )
            db.commit()

    def _session(self, db: sqlite3.Connection, device_id: str) -> sqlite3.Row:
        db.execute("INSERT OR IGNORE INTO quiz_sessions(device_id) VALUES(?)",
                   (device_id,))
        return db.execute("SELECT * FROM quiz_sessions WHERE device_id=?",
                          (device_id,)).fetchone()

    @staticmethod
    def _question(card: sqlite3.Row) -> dict[str, Any]:
        return _reply("猜一猜：" + card["prompt"] +
                      " 选一：" + card["first_text"] +
                      "。选二：" + card["second_text"] + "。",
                      state="question")

    def _active(self, db: sqlite3.Connection, session: sqlite3.Row):
        if session["active_card"] is None:
            return None
        return db.execute("SELECT * FROM quiz_cards WHERE card_id=? AND approved=1",
                          (session["active_card"],)).fetchone()

    def start(self, device_id: str) -> dict[str, Any]:
        with self._db() as db:
            db.execute("BEGIN IMMEDIATE")
            session = self._session(db, device_id)
            card = self._active(db, session)
            if card is None:
                cards = db.execute("SELECT * FROM quiz_cards WHERE approved=1 "
                                   "ORDER BY card_id").fetchall()
                if not cards:
                    return _reply("还没有准备好的小游戏，请家长帮忙选题吧。",
                                  kind="fallback")
                index = session["next_cursor"] % len(cards)
                card = cards[index]
                db.execute(
                    "UPDATE quiz_sessions SET active_card=?,attempts=0,"
                    "next_cursor=next_cursor+1 WHERE device_id=?",
                    (card["card_id"], device_id),
                )
                db.commit()
            return self._question(card)

    def answer(self, device_id: str, choice: int) -> dict[str, Any]:
        if choice not in (1, 2):
            return _reply("请说选一或选二。", kind="fallback")
        with self._db() as db:
            db.execute("BEGIN IMMEDIATE")
            session = self._session(db, device_id)
            card = self._active(db, session)
            if card is None:
                if session["active_card"]:
                    db.execute(
                        "UPDATE quiz_sessions SET active_card=NULL,attempts=0 "
                        "WHERE device_id=?", (device_id,))
                    db.commit()
                return _reply("先说猜动物，我们就开始吧。", kind="fallback")
            if choice == card["correct"]:
                db.execute(
                    "UPDATE quiz_sessions SET active_card=NULL,attempts=0,"
                    "completed=completed+1 WHERE device_id=?", (device_id,))
                db.commit()
                return _reply(card["success_text"] + " 想继续可以说再来一题。",
                              state="finished")
            if session["attempts"] < 1:
                db.execute(
                    "UPDATE quiz_sessions SET attempts=attempts+1 "
                    "WHERE device_id=?", (device_id,))
                db.commit()
                return _reply(card["retry_text"], state="retry")
            db.execute(
                "UPDATE quiz_sessions SET active_card=NULL,attempts=0 "
                "WHERE device_id=?", (device_id,))
            db.commit()
            return _reply("这次我们先玩到这里，下一次再一起试试吧。",
                          kind="fallback", state="finished")

    def status(self, device_id: str) -> dict[str, Any]:
        with self._db() as db:
            session = db.execute(
                "SELECT active_card,completed FROM quiz_sessions WHERE device_id=?",
                (device_id,)).fetchone()
        return {
            "active": bool(session and session["active_card"]),
            "completed": int(session["completed"]) if session else 0,
        }

    def intent(self, device_id: str, text: str, *, quiet: bool) -> dict | None:
        if not isinstance(text, str):
            return None
        phrase = _speech(text)
        if phrase not in START_PHRASES | FIRST_PHRASES | SECOND_PHRASES:
            return None
        if quiet:
            return _reply("现在是安静时间，我们下次再一起玩吧。",
                          kind="fallback")
        if phrase in START_PHRASES:
            return self.start(device_id)
        return self.answer(device_id, 1 if phrase in FIRST_PHRASES else 2)
