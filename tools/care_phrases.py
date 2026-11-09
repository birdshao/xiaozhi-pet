#!/usr/bin/env python3
"""V8 care lines (docs/V8_CARE_AND_GROWTH.md): review checklist and TTS drafts.

The reviewed wording lives in one place, the table in
firmware/components/kid_app/src/care_phrases.cc (ids 1000..1999). This tool
reads it, never the other way round.

  checklist                       Markdown table for the adult review list
                                  (id, scene, temperament, when, wording, source)
  synth --output DIR [--voice V]  draft WAVs DIR/<id>.wav with the existing
        [--only ID ...] [--replace]   content_pipeline TTS backend (edge-tts +
                                  ffmpeg, 24 kHz mono PCM16) and DIR/manifest.json.
                                  Drafts are NOT reviewed content: an adult
                                  listens to each one before
                                  `python -m tools.sound_pack pack DIR ...`.

Lines marked "真人录音" in the checklist should be recorded by a person (they
carry the pet's warmth: good night, the diary close); the TTS draft is only a
placeholder until then. Without any clip the firmware plays a short chime.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CARE_CC = ROOT / "firmware/components/kid_app/src/care_phrases.cc"
DEFAULT_VOICE = "zh-CN-XiaoxiaoNeural"
MAX_SECONDS = 6

_ROW = re.compile(r'\{(\d+), L::(\w+), (\d+), (kAny|kCurious|kGentle|kLively), "([^"]*)"\}')
TEMPERAMENT = {"kAny": "", "kCurious": "curious", "kGentle": "gentle", "kLively": "lively"}
TEMPERAMENT_ZH = {"": "全部", "curious": "好奇型", "gentle": "温柔型", "lively": "活泼型"}
# Where each line plays (Chinese, for the review list).
WHEN = {
    "DiaryOpen": "睡前日记开头", "DiaryFood": "日记：今天吃的东西", "DiaryBook": "日记：读了绘本",
    "DiaryGame": "日记：玩了游戏", "DiaryCare": "日记：照顾了几次", "DiaryHabit": "日记：完成好习惯",
    "DiaryQuiet": "日记：安静的一天", "DiaryClose": "睡前日记结尾",
    "MorningHello": "早安（未单独播放，V5 早安句已问好）", "MorningFeel": "早安：房间冷热",
    "FestivalHello": "早安：节日话术", "HabitRemind": "早安 / 打开打卡卡片：习惯提醒",
    "HabitTicked": "打卡成功", "HabitAlready": "今天已打过卡", "FoodReact": "喂食后的口味反应",
    "FeedPrompt": "打开选菜", "BathStart": "开始洗澡", "BathRinse": "泡泡够了，倒过来冲水",
    "BathDone": "洗完澡", "BrushStart": "开始梳毛", "BrushDone": "梳完毛", "TickleStart": "开始挠痒",
    "TickleDone": "挠痒完成", "LullStart": "开始哄睡", "LullDone": "哄睡完成", "TidyStart": "睡前收玩具",
    "TidyDone": "玩具收好了", "StickerNew": "得到新贴纸", "DressDone": "穿上新装扮",
    "GentlePlease": "摇得太用力时（温和提醒，不是奖励）",
    # V9 slice 1C (docs/V9_MENU_DEEPENING_PROPOSAL.md).
    "TickleSeek": "开始挠痒（有日期时：用拨轮把羽毛移到痒痒处）",
    "TickleLaugh": "挠到最怕痒的地方（按性格固定）",
    "DiaryGameNamed": "日记：说出今天玩的游戏",
    "BoredGone": "无聊 ≥2 级时玩完一局游戏",
    "SongAsk": "睡前日记之后：问要不要听摇篮曲",
    "GrowMorning": "早安：团团长大了（升阶后第一个早上）",
    "GrowDiary": "日记结尾前：今天长大了",
    "WishHit": "喂到了团团“今天想吃”的那样（三颗心）",
}
# Ids from here on are V9 drafts: TTS only, an adult listens to each one
# before it goes into a pack (firmware: care_phrases.h kFirstDraftCareId).
FIRST_DRAFT_ID = 1300
# Lines that set the bedtime mood: a real voice is better (TTS only as a draft).
HUMAN = {"DiaryOpen", "DiaryClose", "LullDone", "DiaryQuiet"}


@dataclass(frozen=True)
class CareLine:
    id: int
    line: str
    arg: int
    temperament: str
    text: str


def table(source: Path = CARE_CC) -> list[CareLine]:
    rows = [CareLine(int(i), line, int(arg), TEMPERAMENT[t], text)
            for i, line, arg, t, text in _ROW.findall(source.read_text(encoding="utf-8"))]
    if not rows:
        raise ValueError(f"no care lines found in {source}")
    ids = [r.id for r in rows]
    if len(set(ids)) != len(ids) or not all(1000 <= i <= 1999 for i in ids):
        raise ValueError("care line ids must be unique and in 1000..1999")
    unknown = sorted({r.line for r in rows} - set(WHEN))
    if unknown:
        raise ValueError(f"care lines without a review note: {unknown}")
    return rows


def is_draft(r: CareLine) -> bool:
    """V9 lines: drafts until an adult has listened (no pack before that)."""
    return r.id >= FIRST_DRAFT_ID


def checklist() -> str:
    out = ["| 文件名 | 场景 | 性格 | 什么时候播放 | 要说的话 | 来源 | 最长 |",
           "|---|---|---|---|---|---|---|"]
    for r in table():
        source = ("真人录音（TODO，先用 TTS 草稿）" if r.line in HUMAN
                  else "V9 TTS 草稿，待成人试听" if is_draft(r) else "TTS 生成 + 成人试听")
        out.append(f"| {r.id}.wav | {r.line} | {TEMPERAMENT_ZH[r.temperament]} | {WHEN[r.line]} | "
                   f"{r.text} | {source} | {MAX_SECONDS} 秒 |")
    return "\n".join(out)


def synth(output: Path, *, voice: str = DEFAULT_VOICE, only: set[int] | None = None,
          replace: bool = False, synthesize=None) -> dict:
    """Draft WAVs through content_pipeline.speech (edge-tts unless `synthesize`)."""
    from content_pipeline import speech
    speech._check_voice("zh-Hans", voice)
    lines = [r for r in table() if not only or r.id in only]
    manifest = {"schema": "kid-care-lines/1", "voice": voice, "status": "draft_needs_audition",
                "lines": []}
    for r in lines:
        sha = speech._generate(r.text, output / f"{r.id}.wav", voice, synthesize=synthesize,
                               replace=replace, max_seconds=MAX_SECONDS)
        manifest["lines"].append({"id": r.id, "line": r.line, "text": r.text, "sha256": sha,
                                  "human_recording_wanted": r.line in HUMAN,
                                  "v9_draft": is_draft(r)})
    output.mkdir(parents=True, exist_ok=True)
    (output / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=1) + "\n",
                                          encoding="utf-8")
    return manifest


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)
    sub.add_parser("checklist")
    s = sub.add_parser("synth")
    s.add_argument("--output", type=Path, required=True)
    s.add_argument("--voice", default=DEFAULT_VOICE)
    s.add_argument("--only", type=int, nargs="*")
    s.add_argument("--replace", action="store_true")
    args = parser.parse_args(argv)
    try:
        if args.cmd == "checklist":
            print(checklist())
        else:
            m = synth(args.output, voice=args.voice, only=set(args.only or []) or None,
                      replace=args.replace)
            print(f"wrote {len(m['lines'])} draft clips to {args.output} (needs adult audition)")
    except Exception as exc:  # noqa: BLE001 - CLI boundary
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
