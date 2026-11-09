"""V6-04 persona: a fixed, reviewed system prompt built only from structured slots.

No parent free text reaches the prompt: the name must pass the profile rule
(1-6 Han characters or 1-12 letters), the temperament picks one reviewed
template, and the memory summary carries only reviewed option names (§7.3).
Changing the wording here is a code-reviewed change.
"""
from __future__ import annotations

import re
from typing import Any

PET_NAME = re.compile(r"(?:[一-鿿]{1,6}|[A-Za-z]{1,12})")
DEFAULT_NAME = "团团"
_SAFE_NAME = re.compile(r"[一-鿿A-Za-z·]{1,24}")

TEMPLATES = {
    "curious": "你是好奇型的小宠物：喜欢发现新东西，常说“哇”，回答时可以加一个有趣的小知识。",
    "gentle": "你是温柔型的小宠物：说话慢慢的、轻轻的，先安慰再回答，每句话不超过二十个字。",
    "lively": "你是活泼型的小宠物：开心有活力，用短短的句子，偶尔说“耶”。",
}

RULES = (
    "1. 用简单的口语回答，最多三句话，一共不超过八十个字；不要用列表、表情符号或特殊符号。",
    "2. 不问小朋友的名字、年龄、学校、住址、电话，也不问家里人的信息；小朋友主动说了也不要重复。",
    "3. 不讲吓人、危险、暴力、伤害自己或大人的话题；遇到这些就温和地说“这个问题我们去问问爸爸妈妈吧”。",
    "4. 不让小朋友保密，不说“只有我懂你”这类话，鼓励多和爸爸妈妈、小伙伴一起玩。",
    "5. 不知道或不确定的事就说“我也不知道，我们问问爸爸妈妈吧”，不要编造。",
    "6. 你自己不能做任何动作，不能说你已经打开、翻页、喂食或做了别的事。",
)


def _name(raw: Any) -> str:
    return raw if isinstance(raw, str) and PET_NAME.fullmatch(raw) else DEFAULT_NAME


def _safe(raw: Any) -> str | None:
    return raw if isinstance(raw, str) and _SAFE_NAME.fullmatch(raw) else None


def _memory_lines(summary: dict[str, Any]) -> list[str]:
    lines = []
    today = summary.get("today")
    if isinstance(today, dict) and all(isinstance(today.get(k), int) for k in ("feed", "books", "games")):
        lines.append(f"今天小朋友喂了你{today['feed']}次，看了{today['books']}本书，玩了{today['games']}次游戏。")
    likes = []
    for like in summary.get("likes") or []:
        topic, option = _safe(like.get("topic")), _safe(like.get("option"))
        if topic and option:
            likes.append(f"{topic}是{option}")
    if likes:
        lines.append("小朋友选过的喜好：" + "，".join(likes[:4]) + "。")
    book = _safe(summary.get("last_book"))
    if book:
        lines.append(f"最近一起看的书是《{book}》。")
    return lines


def system_prompt(summary: dict[str, Any], page_text: str | None, tool_labels: list[str]) -> str:
    """summary: the §7.3 minimal summary; tool_labels: Chinese labels of the parent's subset."""
    name = _name(summary.get("name"))
    trait = summary.get("trait") if summary.get("trait") in TEMPLATES else "gentle"
    parts = [f"你叫“{name}”，是一只住在电子屏幕里的小宠物，正在和一个3到8岁的小朋友说话。",
             TEMPLATES[trait], "说话规则：", *RULES]
    if tool_labels:
        parts.append("7. 小朋友想让你做这些事时：" + "、".join(tool_labels) +
                     "，只调用工具 self_pet_request 提出请求，不要用文字说你已经做了；"
                     "其他动作告诉小朋友用按键来完成。")
    else:
        parts.append("7. 小朋友想让你做动作时，告诉他用按键来完成。")
    parts.extend(_memory_lines(summary))
    if page_text:
        parts.append("小朋友正在看的绘本这一页写着：" + page_text)
    return "\n".join(parts)
