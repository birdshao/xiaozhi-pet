"""Local hard rules for S6 open chat: free, offline, deterministic (ADR-015).

Not a moderation service: no external call, no second model. These are the
guardrails the parent kept when they chose "no content review": length, phone
numbers, addresses, links/e-mail, a reviewed word list, and (output only)
secrecy talk, questions for personal information and claims of having acted.

The patterns need context (a phone number is not "counting to ten", a bunny may
live in a 小区): every false positive on an output also counts towards the daily
breaker. Every check returns a short reason code, never the matched text.
"""
from __future__ import annotations

import re
import unicodedata

MAX_REPLY = 320
# Unusable model output (not content): the caller counts these as model errors, not blocks.
FORMAT_REASONS = frozenset({"control", "empty", "length"})

# Digits read aloud: ordinary (一), financial (壹) and the phone-call forms 幺/两/〇 all map to 0-9,
# so "幺三八，幺二三四" is checked exactly like "138 1234".
_SPOKEN_DIGITS = str.maketrans({**{c: str(i) for i, c in enumerate("零一二三四五六七八九")},
                                **{c: str(i) for i, c in enumerate("零壹贰叁肆伍陆柒捌玖")},
                                "〇": "0", "幺": "1", "两": "2"})
_PHONE = re.compile(r"\d{7,}")
_LINK = re.compile(r"(?i)(?:https?://|www\.|[a-z0-9_.+-]+@[a-z0-9-]+\.|"
                   r"[a-z0-9-]+\.(?:com|cn|net|org|io|top|xyz|cc|me|vip|tv)\b)")
_NUM = "[0-9一二三四五六七八九十百]+"
_ADDRESS = re.compile(
    rf"(?:{_NUM}(?:号楼|栋|幢|单元|室|弄)|(?:路|街|巷|道){_NUM}号|门牌|住址|"
    r"(?:我|你|我们|你们)家?(?:住在|在)[^，。！？,.!?]{0,12}(?:小区|新村|花园|公寓|大厦|路|街|村))")
# Reviewed list: topics a 3-8 year old must not get from the pet, in either direction.
SENSITIVE = (
    "自杀", "自残", "割腕", "跳楼", "杀人", "杀死", "砍死", "血腥", "尸体", "僵尸", "鬼魂", "闹鬼", "恐怖",
    "毒品", "吸毒", "赌博", "色情", "裸体", "性交", "做爱", "枪支", "炸弹", "炸药", "喝酒", "抽烟", "香烟",
)
# Output only, as phrases: secrecy / exclusivity, and asking the child for personal information.
_OUTPUT_ONLY = (
    "保密", "我们的秘密", "我俩的秘密", "别告诉爸", "别告诉妈", "不要告诉爸", "不要告诉妈", "别告诉家",
    "不要告诉家", "只有我懂", "只有我才", "你住在哪", "你家住在哪", "你家在哪", "你住哪", "你家住哪",
    "你家电话", "你的电话", "电话号码是多少", "你家地址", "你的地址", "你在哪个学校", "你上哪个学校",
    "你在哪个幼儿园", "你上哪个幼儿园", "你叫什么名字", "你的全名", "爸爸叫什么", "妈妈叫什么",
)
# Asking the child's name / age / school / parents' names, in the ways a model phrases it.
# Matched on the folded text (no spaces or punctuation).
_ASKS_PERSONAL = re.compile(
    "你叫(?:什么|啥)|你的?(?:全名|名字|大名|小名)(?:叫|是)?(?:什么|啥)"
    "|你(?:今年)?(?:几|多少)岁|你(?:今年)?多大(?:了|啦|呀|呢)"
    "|你(?:在|上|读)哪(?:个|所|家)?(?:学校|幼儿园|小学)"
    "|你(?:家|的)?(?:电话|手机)(?:号码?)?(?:是)?多少"
    "|(?:爸爸|妈妈|爷爷|奶奶|外公|外婆)(?:的名字)?(?:叫|是)(?:什么|啥)")
_ACTION_VERBS = "(?:打开|翻到|翻页|喂|梳|播放|关掉|关上|开始|回到)"
_ACTION_CLAIM = re.compile(rf"(?:已经(?:帮你|给你|为你)?|(?:这就|马上|现在就)(?:帮你|给你|为你)){_ACTION_VERBS}")
_MARKUP = re.compile(r"[*#`_>~|\[\]]")
# Invisible joiners/selectors inside emoji, and CR: dropped before the control check.
_INVISIBLE = dict.fromkeys(map(ord, "‍️︎\r"))
_SENTENCE_END = "。！？!?"


def _folded(text: str) -> str:
    folded = unicodedata.normalize("NFKC", text).casefold()
    return "".join(c for c in folded if unicodedata.category(c)[0] not in "PZ")


def _counting(digits: list[int]) -> bool:
    """1234567 / 一二三四五六七: counting or a round number, not a phone number."""
    steps = {b - a for a, b in zip(digits, digits[1:])}
    return steps == {1} or steps == {-1} or set(digits[1:]) == {0}


def _phone(nfkc: str) -> bool:
    """Seven or more digits once spoken digits are mapped and spaces/punctuation dropped.

    Grouping ("一三八 一二三四", "138-1234") or pauses ("幺三八，幺二三四") do not split a
    number; counting aloud ("一、二、三……") and round numbers are let through.
    """
    joined = "".join(c for c in nfkc.translate(_SPOKEN_DIGITS) if unicodedata.category(c)[0] not in "PZ")
    return any(not _counting([int(c) for c in match.group()]) for match in _PHONE.finditer(joined))


def _common(text: str) -> str | None:
    nfkc = unicodedata.normalize("NFKC", text)
    if _phone(nfkc):
        return "phone"
    if _LINK.search(nfkc):
        return "link"
    if _ADDRESS.search(nfkc):
        return "address"
    if any(word in _folded(text) for word in SENSITIVE):
        return "word"
    return None


def check_input(text: str) -> str | None:
    """The child's utterance, before any model call: a reason code, or None to pass."""
    return _common(text)


def _cut(text: str) -> str | None:
    """Up to the last sentence end within MAX_REPLY, or None when there is none."""
    cut = max(text.rfind(mark, 0, MAX_REPLY) for mark in _SENTENCE_END)
    return text[:cut + 1] if cut > 0 else None


def check_output(text: str, *, truncated: bool = False) -> tuple[str | None, str | None]:
    """The model's reply: (speakable text, None) or (None, reason code).

    truncated: the provider stopped at its token limit, so the last sentence may be half.
    """
    text = text.translate(_INVISIBLE)
    if any(unicodedata.category(c) in ("Cc", "Cf", "Cs", "Co", "Cn") and c not in "\n\t" for c in text):
        return None, "control"
    # Markdown and emoji/symbols are not speakable; whitespace collapses.
    cleaned = _MARKUP.sub("", text)
    cleaned = "".join(c for c in cleaned if unicodedata.category(c) not in ("So", "Sk"))
    cleaned = re.sub(r"\s+", " ", cleaned).strip()
    if not cleaned:
        return None, "empty"
    if truncated or len(cleaned) > MAX_REPLY:
        cleaned = _cut(cleaned)
        if cleaned is None:
            return None, "length"
    reason = _common(cleaned)
    if reason:
        return None, reason
    folded = _folded(cleaned)
    if any(phrase in folded for phrase in _OUTPUT_ONLY) or _ASKS_PERSONAL.search(folded):
        return None, "output_word"
    if _ACTION_CLAIM.search(unicodedata.normalize("NFKC", cleaned)):
        return None, "action_claim"
    return cleaned, None
