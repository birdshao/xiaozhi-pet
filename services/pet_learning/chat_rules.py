"""Local hard rules for S6 open chat: free, offline, deterministic (ADR-016).

Not a moderation service: no external call, no second model. These are the
guardrails the parent kept when they chose "no content review": length, phone
numbers, addresses, links/e-mail, a reviewed word list, and (output only)
secrecy talk, questions for personal information and claims of having acted.
Every check returns a short reason code, never the matched text.
"""
from __future__ import annotations

import re
import unicodedata

MAX_REPLY = 320

_PHONE = re.compile(r"(?:\d[\s\-－—]?){7,}")
_CN_PHONE = re.compile(r"[零〇一二三四五六七八九幺两]{7,}")
_LINK = re.compile(r"(?i)(?:https?://|www\.|[a-z0-9_.+-]+@[a-z0-9-]+\.|"
                   r"[a-z0-9-]+\.(?:com|cn|net|org|io|top|xyz|cc|me|vip|tv)\b)")
_ADDRESS = re.compile(r"(?:[0-9一二三四五六七八九十百]+(?:号楼|栋|幢|单元|室|弄)|"
                      r"(?:路|街|巷|道)[0-9一二三四五六七八九十百]+号|小区|门牌|住址|家住)")
# Reviewed list: topics a 3-8 year old must not get from the pet, in either direction.
SENSITIVE = (
    "自杀", "自残", "割腕", "跳楼", "杀人", "杀死", "砍死", "血腥", "尸体", "僵尸", "鬼魂", "闹鬼", "恐怖",
    "毒品", "吸毒", "赌博", "色情", "裸体", "性交", "做爱", "枪支", "炸弹", "炸药", "喝酒", "抽烟", "香烟",
)
# Output only: secrecy / exclusivity, asking for personal information, pretending to act.
_OUTPUT_ONLY = (
    "保密", "秘密", "别告诉", "不要告诉", "只有我懂", "只有我才", "住在哪", "家在哪", "住哪", "电话", "地址",
    "哪个学校", "哪个幼儿园", "叫什么名字", "全名", "爸爸叫什么", "妈妈叫什么",
)
_ACTION_CLAIM = re.compile(r"(?:已经|这就|马上|现在就|正在)(?:帮你|给你|为你)?"
                           r"(?:打开|翻到|翻页|喂|梳|播放|关掉|关上|开始|回到)")
_MARKUP = re.compile(r"[*#`_>~|\[\]]")
_SENTENCE_END = "。！？!?"


def _folded(text: str) -> str:
    folded = unicodedata.normalize("NFKC", text).casefold()
    return "".join(c for c in folded if unicodedata.category(c)[0] not in "PZ")


def _common(text: str) -> str | None:
    nfkc = unicodedata.normalize("NFKC", text)
    if _PHONE.search(nfkc) or _CN_PHONE.search(_folded(text)):
        return "phone"
    if _LINK.search(nfkc):
        return "link"
    if _ADDRESS.search(nfkc):
        return "address"
    folded = _folded(text)
    if any(word in folded for word in SENSITIVE):
        return "word"
    return None


def check_input(text: str) -> str | None:
    """The child's utterance, before any model call: a reason code, or None to pass."""
    return _common(text)


def check_output(text: str) -> tuple[str | None, str | None]:
    """The model's reply: (speakable text, None) or (None, reason code)."""
    if any(unicodedata.category(c) in ("Cc", "Cf", "Cs", "Co", "Cn") and c not in "\n\t" for c in text):
        return None, "control"
    # Markdown and emoji/symbols are not speakable; whitespace collapses.
    cleaned = _MARKUP.sub("", text)
    cleaned = "".join(c for c in cleaned if unicodedata.category(c) not in ("So", "Sk", "Cs"))
    cleaned = re.sub(r"\s+", " ", cleaned).strip()
    if not cleaned:
        return None, "empty"
    if len(cleaned) > MAX_REPLY:
        cut = max(cleaned.rfind(mark, 0, MAX_REPLY) for mark in _SENTENCE_END)
        if cut <= 0:
            return None, "length"
        cleaned = cleaned[:cut + 1]
    reason = _common(cleaned)
    if reason:
        return None, reason
    folded = _folded(cleaned)
    if any(word in folded for word in _OUTPUT_ONLY):
        return None, "output_word"
    if _ACTION_CLAIM.search(unicodedata.normalize("NFKC", cleaned)):
        return None, "action_claim"
    return cleaned, None
