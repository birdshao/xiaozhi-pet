"""Voice-first, parent-approved story playback cursor.

No audio persistence, external LLM calls, arbitrary file reads or content search.
Each device can resume a validated page across a Python service restart.
"""
from __future__ import annotations

from contextlib import contextmanager
from pathlib import Path
import re
import sqlite3
from typing import Any

START = frozenset(("讲故事", "给我讲故事", "开始故事", "读故事", "我想听故事"))
NEXT = frozenset(("下一页", "翻页", "继续故事"))
REPEAT = frozenset(("重读这一页", "再读一遍", "读这一页", "这一页讲了什么"))
STOP = frozenset(("结束故事", "退出故事", "不听了"))

def _phrase(text: str) -> str:
    return re.sub(r"[\s，。？！!?,.、]+", "", text.strip()).lower()


def _reply(text: str, *, state: str, kind: str = "approved_content",
           page: int | None = None) -> dict[str, Any]:
    result: dict[str, Any] = {
        "kind": kind, "action": None, "tts_text": text, "story_state": state
    }
    if page is not None:
        result["page"] = page
    return result


class StoryReader:
    def __init__(self, filename: str | Path) -> None:
        self.filename = str(filename)
        if self.filename != ":memory:":
            Path(self.filename).parent.mkdir(parents=True, exist_ok=True)
        with self._db() as db:
            db.execute("""
                CREATE TABLE IF NOT EXISTS story_reader_sessions(
                    device_id TEXT PRIMARY KEY,
                    current_book TEXT,
                    current_page INTEGER NOT NULL DEFAULT 1,
                    next_cursor INTEGER NOT NULL DEFAULT 0
                )
            """)

    @contextmanager
    def _db(self):
        db = sqlite3.connect(self.filename, timeout=5, isolation_level=None)
        try:
            db.row_factory = sqlite3.Row
            db.execute("PRAGMA busy_timeout=5000")
            with db:
                yield db
        finally:
            db.close()

    @staticmethod
    def _page(db, book_id: str | None, page: int):
        if not book_id or page < 1 or page > 1000:
            return None
        return db.execute(
            "SELECT p.text,b.title,p.page,b.book_id FROM pages p "
            "JOIN books b ON b.book_id=p.book_id "
            "WHERE p.book_id=? AND p.page=? AND p.approved=1",
            (book_id,page),
        ).fetchone()

    @staticmethod
    def _speak(page, *, prefix: str = ""):
        text = prefix + page["text"]
        # Upstream voice guard accepts no more than 320 spoken characters.
        # Long approved pages remain verbatim without a title prefix.
        if len(text) > 320:
            text = page["text"]
        return _reply(text, state="reading", page=page["page"])

    @staticmethod
    def _session(db, device_id: str):
        db.execute("INSERT OR IGNORE INTO story_reader_sessions(device_id) VALUES(?)",
                   (device_id,))
        return db.execute("SELECT * FROM story_reader_sessions WHERE device_id=?",
                          (device_id,)).fetchone()

    def start(self, device_id: str) -> dict[str, Any]:
        with self._db() as db:
            db.execute("BEGIN IMMEDIATE")
            session = self._session(db, device_id)
            page = self._page(db, session["current_book"], session["current_page"])
            if page:
                return self._speak(page)
            eligible = db.execute(
                "SELECT b.book_id FROM books b "
                "WHERE EXISTS (SELECT 1 FROM pages p WHERE p.book_id=b.book_id "
                "AND p.page=1 AND p.approved=1) ORDER BY b.book_id"
            ).fetchall()
            if not eligible:
                return _reply("现在还没有准备好的绘本，可以请家人帮忙选一本。",
                              state="idle", kind="fallback")
            book_id = eligible[session["next_cursor"] % len(eligible)]["book_id"]
            page = self._page(db, book_id, 1)
            if not page:
                return _reply("这本绘本还没有准备好，我们下次再读。",
                              state="idle", kind="fallback")
            db.execute(
                "UPDATE story_reader_sessions SET current_book=?,current_page=1,"
                "next_cursor=next_cursor+1 WHERE device_id=?",
                (book_id,device_id),
            )
            db.commit()
            return self._speak(page, prefix="故事开始啦：" + page["title"] + "。")

    def next(self, device_id: str) -> dict[str, Any]:
        with self._db() as db:
            db.execute("BEGIN IMMEDIATE")
            session = self._session(db, device_id)
            if not self._page(db,session["current_book"],session["current_page"]):
                return _reply("先说讲故事，我们再一起阅读吧。",
                              state="idle",kind="fallback")
            page = self._page(db,session["current_book"],session["current_page"]+1)
            if page:
                db.execute(
                    "UPDATE story_reader_sessions SET current_page=? WHERE device_id=?",
                    (page["page"],device_id),
                )
                db.commit()
                return self._speak(page)
            db.execute(
                "UPDATE story_reader_sessions SET current_book=NULL,current_page=1 "
                "WHERE device_id=?", (device_id,),
            )
            db.commit()
            return _reply("故事讲完啦，想再听可以说讲故事。",
                          state="finished", kind="fallback")

    def repeat(self, device_id: str) -> dict[str, Any]:
        with self._db() as db:
            db.execute("BEGIN IMMEDIATE")
            session = self._session(db,device_id)
            page = self._page(db,session["current_book"],session["current_page"])
            if not page:
                return _reply("先说讲故事，我们就开始阅读。",
                              state="idle",kind="fallback")
            return self._speak(page)

    def stop(self, device_id: str) -> dict[str, Any]:
        with self._db() as db:
            db.execute("BEGIN IMMEDIATE")
            self._session(db, device_id)
            db.execute(
                "UPDATE story_reader_sessions SET current_book=NULL,current_page=1 "
                "WHERE device_id=?", (device_id,),
            )
            db.commit()
        return _reply("故事先读到这里，下次我们再一起看书。",
                      state="finished",kind="fallback")

    def status(self, device_id: str) -> dict[str, Any]:
        with self._db() as db:
            row = db.execute(
                "SELECT current_book,current_page FROM story_reader_sessions "
                "WHERE device_id=?", (device_id,),
            ).fetchone()
            if row and self._page(db,row["current_book"],row["current_page"]):
                return {"active":True,"page":int(row["current_page"])}
        return {"active":False,"page":None}

    def intent(self, device_id: str, text: str, *, quiet: bool) -> dict | None:
        if not isinstance(text,str):
            return None
        phrase = _phrase(text)
        if phrase not in START | NEXT | REPEAT | STOP:
            return None
        if quiet:
            return _reply("现在是安静时间，我们下次再读吧。",
                          state="idle",kind="fallback")
        if phrase in START:
            return self.start(device_id)
        if phrase in NEXT:
            return self.next(device_id)
        if phrase in REPEAT:
            return self.repeat(device_id)
        return self.stop(device_id)
