"""Deterministic speech intents, intentionally *not* a general LLM safety filter.

Every speech response here is fixed and reviewed, or an approved book page.
No arbitrary model output is accepted or sent to TTS by this adapter.
"""
from __future__ import annotations
import re
import unicodedata
from dataclasses import dataclass
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


# --- S5 fixed-command matcher (V5_V6_SERVER_PLAN §6.2) -----------------------
#
# The ONLY producer of device actions. Page Q&A and (later) open chat never
# call it for an action, and nothing here accepts model output.

ACTIONS = frozenset({
    "SLEEP_REQUEST", "PAGE_NEXT", "PAGE_PREV", "OPEN_READER", "OPEN_GAME",
    "PET_FEED", "PET_GROOM", "GO_HOME",
    # V8-03: proposals only; the device decides, voice never rewards.
    "ASK_NEEDS", "PLAY_SONG", "GOOD_NIGHT", "TELL_JOKE", "START_BRUSH", "START_WASH",
})
# Spoken as a question on purpose ("团团饿了吗？"): the question mark is allowed.
QUESTION_OK = frozenset({"ASK_NEEDS"})


@dataclass(frozen=True)
class FixedMatch:
    """`action` is set only for local_command rows; `topic` for server-answered rows."""
    kind: str                  # "local_command" | "time" | "battery"
    reason: str
    action: str | None = None
    tts_text: str = ""


def normalize_speech(text: str) -> str:
    """NFKC, casefold, then drop whitespace, punctuation and control characters."""
    folded = unicodedata.normalize("NFKC", text).casefold()
    return "".join(c for c in folded if unicodedata.category(c)[0] not in "PZC")


def _commands() -> dict[str, FixedMatch]:
    rows = {
        ("SLEEP_REQUEST", "好的，我们准备睡觉啦。"): ("团团睡觉", "团团睡觉吧", "我们睡觉吧", "我们睡觉了"),
        ("PAGE_NEXT", "我们翻到下一页。"): ("下一页", "下一页吧", "翻页", "翻到下一页"),
        ("PAGE_PREV", "我们翻到上一页。"): ("上一页", "上一页吧", "翻到上一页", "返回上一页"),
        ("OPEN_READER", "好呀，我们来讲故事。"): ("讲故事", "讲故事吧", "讲个故事", "读绘本", "读绘本吧"),
        ("OPEN_GAME", "好呀，我们来玩游戏。"): ("玩游戏", "玩游戏吧", "玩个游戏", "我要玩游戏"),
        ("PET_FEED", "好呀，我们给团团喂一口。"): ("喂宠物", "喂团团", "喂食", "喂宠物吧", "喂团团吧"),
        ("PET_GROOM", "好呀，我们给团团梳梳毛。"): ("给团团梳毛", "团团梳毛", "梳毛", "给团团梳梳毛"),
        ("GO_HOME", "好的，我们回首页。"): ("回家", "回首页", "回到首页", "回家吧"),
        ("ASK_NEEDS", "我来看看团团的小肚子。"): ("团团饿了吗", "团团饿不饿", "团团饿了没有", "团团肚子饿吗"),
        ("PLAY_SONG", "好呀，我们来听歌。"): ("唱首歌", "唱首歌吧", "唱个歌", "唱歌", "团团唱首歌", "团团唱歌"),
        ("GOOD_NIGHT", "晚安，做个好梦。"): ("晚安", "团团晚安", "晚安团团", "说晚安"),
        ("TELL_JOKE", ""): ("讲个笑话", "讲笑话", "讲个笑话吧", "说个笑话", "团团讲个笑话"),
        ("START_BRUSH", "好呀，我们一起刷牙。"): ("刷牙", "刷牙吧", "团团刷牙", "我们刷牙", "该刷牙了"),
        ("START_WASH", "好呀，我们一起洗手。"): ("洗手", "洗手吧", "团团洗手", "我们洗手", "去洗手"),
    }
    table = {}
    for (action, reply), phrases in rows.items():
        assert action in ACTIONS
        for phrase in phrases:
            table[normalize_speech(phrase)] = FixedMatch("local_command", "fixed_command", action, reply)
    for phrase in ("几点了", "现在几点", "现在几点了", "现在几点钟", "几点钟了"):
        table[normalize_speech(phrase)] = FixedMatch("time", "tell_time")
    for phrase in ("还有多少电", "还剩多少电", "电量还有多少", "团团还有多少电"):
        table[normalize_speech(phrase)] = FixedMatch("battery", "tell_battery")
    return table


_FIXED = _commands()
PAGE_QA_PHRASES = frozenset(normalize_speech(p) for p in (
    "这一页讲了什么", "这页讲了什么", "当前页是什么", "读这一页", "这一页是什么"))


def _is_command_shaped(text: str) -> bool:
    """Reject utterances whose punctuation changes the meaning once stripped.

    A question ("团团睡觉？") is not a command, and "喂，团团！" is a call for
    attention, not "feed Tuantuan".
    """
    folded = unicodedata.normalize("NFKC", text)
    if "?" in folded:
        return False
    for index, char in enumerate(folded[:-1]):
        if char == "喂" and unicodedata.category(folded[index + 1])[0] in "PZ":
            return False
    return True


def match_fixed(text: str) -> FixedMatch | None:
    """Exact match of the normalised utterance against the reviewed whitelist."""
    match = _FIXED.get(normalize_speech(text))
    # Questions about time/battery are natural ("几点了？"); only actions need the check.
    if (match is not None and match.kind == "local_command" and match.action not in QUESTION_OK
            and not _is_command_shaped(text)):
        return None
    return match


def is_page_question(text: str) -> bool:
    return normalize_speech(text) in PAGE_QA_PHRASES
