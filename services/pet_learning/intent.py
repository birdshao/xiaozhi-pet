"""Deterministic speech intents, intentionally *not* a general LLM safety filter.

Every speech response here is fixed and reviewed, or an approved book page.
No arbitrary model output is accepted or sent to TTS by this adapter.
"""
from __future__ import annotations
import re
from typing import Any, Callable


def _normalize(text: str) -> str:
    return re.sub(r"[\s，。？！、!?,.]+", "", text.strip()).lower()


def resolve(text: str, snapshot: dict[str, Any],
            approved_page: Callable[[str, int], dict[str, Any] | None]) -> dict[str, Any]:
    if not isinstance(text, str) or not 0 < len(text) <= 120:
        return {
            "kind": "fallback", "action": None,
            "tts_text": "我没听清，可以请大人帮我们再说一次吗？",
        }

    phrase = _normalize(text)
    # Fixed commands are proposals only: the device's durable PetController
    # must perform and ACK the action. No LLM is permitted to award points.
    commands = {
        "喂宠物": ("PET_FEED", "好呀，我们给小宠物喂一口。"),
        "喂食": ("PET_FEED", "好呀，我们给小宠物喂一口。"),
        "下一页": ("PAGE_NEXT", "我们翻到下一页。"),
        "翻页": ("PAGE_NEXT", "我们翻到下一页。"),
        "继续故事": ("PAGE_NEXT", "我们翻到下一页。"),
        "结束故事": ("STORY_STOP", "故事先读到这里。"),
        "退出故事": ("STORY_STOP", "故事先读到这里。"),
    }
    if phrase in commands:
        action, response = commands[phrase]
        if snapshot["quiet"]:
            return {"kind": "fallback", "action": None, "tts_text": "现在是安静时间，我们轻轻休息吧。"}
        if action in {"PAGE_NEXT", "STORY_STOP"} and snapshot["mode"] != "STORY":
            return {"kind": "fallback", "action": None, "tts_text": "我们先选一本绘本吧。"}
        if action == "PAGE_NEXT" and (
            not snapshot["active_book"] or
            approved_page(snapshot["active_book"], snapshot["active_page"] + 1) is None
        ):
            return {"kind": "fallback", "action": None, "tts_text": "这本故事已经到最后一页啦。"}
        return {"kind": "local_command", "action": action, "tts_text": response}

    if phrase in {"这一页讲了什么", "当前页是什么", "读这一页", "这一页是什么"}:
        if snapshot["mode"] == "STORY" and snapshot["active_book"]:
            page = approved_page(snapshot["active_book"], snapshot["active_page"])
            if page is not None:
                return {
                    "kind": "approved_content", "action": None,
                    "tts_text": page["text"], "book_id": page["book_id"],
                    "page": page["page"],
                }
        return {"kind": "fallback", "action": None, "tts_text": "我们先选一本家长准备好的绘本吧。"}

    # Safe, conservative MVP: never send unknown ASR to an LLM or TTS.
    # Parents can enable chat later, only after a separate end-to-end content
    # review layer is implemented and proven to gate *every* TTS fragment.
    return {
        "kind": "fallback", "action": None,
        "tts_text": "我们可以看看绘本，或者请家人一起聊聊这个问题。",
    }
