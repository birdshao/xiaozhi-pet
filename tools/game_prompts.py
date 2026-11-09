#!/usr/bin/env python3
"""V9 1E (G4): the spoken rule of every V8 play game (review list and TTS drafts).

When a game opens, TuanTuan says its rule in one or two child-voice sentences
and repeats it after 12 s without input. The wording lives once, in
PlayGamePromptText() of firmware/components/kid_app/src/play_content.cc (the
same strings the quiz review checklist shows); this tool reads it, never the
other way round. Clip ids are KPS1 ids 600 + game id (game_rules.h): memory 4
and memory 6 say the same words and share clip 621.

  checklist                       Markdown table for the adult review list
  synth --output DIR [--voice V]  draft WAVs DIR/<id>.wav with the existing
        [--only ID ...] [--replace]   content_pipeline TTS backend (edge-tts +
                                  ffmpeg, 24 kHz mono PCM16) and DIR/manifest.json.
                                  Drafts are NOT reviewed content: an adult
                                  listens to each one before
                                  `python -m tools.sound_pack pack DIR ...`.

Without a TF sound pack, or without a clip in it, the game plays the old
two-note prompt tone instead. Nothing here is shown on the screen.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PLAY_CONTENT_CC = ROOT / "firmware/components/kid_app/src/play_content.cc"
GAME_RULES_H = ROOT / "firmware/components/kid_app/include/kid_app/game_rules.h"
KPS_BASE = 600          # kGameRuleKpsBase
MEMORY4, MEMORY6 = 21, 22  # kGameRuleMemory4 / kGameRuleMemory6: one shared clip
MAX_SECONDS = 8
DEFAULT_VOICE = "zh-CN-XiaoxiaoNeural"  # same as the care lines until the audition

_GAME_ROW = re.compile(r'\{PlayGame::(\w+), (\d+), "(\w+)"')
# Game names for the review list, by PlayGame enumerator.
NAMES = {
    "Memory4": "记忆翻牌（4 张）", "Memory6": "记忆翻牌（6 张）", "Maze": "迷宫", "Dice": "摇骰子",
    "Tree": "摇果树", "Statue": "木头人", "DayNight": "天黑天亮", "Shapes": "找形状",
    "Counting": "数数", "SpotDiff": "找不同", "MamaBaby": "妈妈宝宝", "SizeOrder": "大小排队",
    "Pattern": "找规律", "Classify": "分类", "Shadow": "找影子", "LeftRight": "左右",
    "Clock": "认时钟", "Weather": "天气穿衣",
}


@dataclass(frozen=True)
class Rule:
    id: int                 # KPS1 clip id (also the file stem)
    games: tuple[int, ...]  # growth memory ids of the games that say it
    names: tuple[str, ...]  # PlayGame enumerators
    text: str


def kps_id(game_id: int) -> int:
    """Mirror of GameRuleKpsId() in game_rules.h."""
    return KPS_BASE + (MEMORY4 if game_id == MEMORY6 else game_id)


def table(source: Path = PLAY_CONTENT_CC, play_prompts: list[tuple[str, str]] | None = None
          ) -> list[Rule]:
    """One row per distinct clip, in id order."""
    from tools import v8_quiz_content
    ids = {name: int(i) for name, i, _ in _GAME_ROW.findall(source.read_text(encoding="utf-8"))}
    prompts = play_prompts if play_prompts is not None else v8_quiz_content.play_prompts()
    said = dict(prompts)
    if set(said) != set(ids):
        raise ValueError(f"games without a rule (or rule without a game): "
                         f"{sorted(set(ids) ^ set(said))}")
    by_clip: dict[int, list[str]] = {}
    for name in ids:
        by_clip.setdefault(kps_id(ids[name]), []).append(name)
    rows = []
    for clip_id, names in sorted(by_clip.items()):
        texts = {said[n] for n in names}
        if len(texts) != 1:
            raise ValueError(f"games sharing clip {clip_id} must say the same words: {names}")
        if not 600 <= clip_id <= 999:
            raise ValueError(f"clip {clip_id} outside the free 600..999 range")
        rows.append(Rule(clip_id, tuple(sorted(ids[n] for n in names)), tuple(sorted(names)),
                         texts.pop()))
    return rows


def where(rule: Rule) -> str:
    return "、".join(NAMES.get(n, n) for n in rule.names)


def checklist() -> str:
    out = ["| 文件名 | 游戏 | 什么时候播放 | 要说的话 | 来源 | 最长 |",
           "|---|---|---|---|---|---|"]
    for r in table():
        out.append(f"| {r.id}.wav | {where(r)} | 进入游戏时；12 秒没有操作再说一遍 | {r.text} | "
                   f"TTS 生成 + 成人试听 | {MAX_SECONDS} 秒 |")
    return "\n".join(out)


def synth(output: Path, *, voice: str = DEFAULT_VOICE, only: set[int] | None = None,
          replace: bool = False, synthesize=None) -> dict:
    """Draft WAVs through content_pipeline.speech (edge-tts unless `synthesize`)."""
    from content_pipeline import speech
    speech._check_voice("zh-Hans", voice)
    rules = [r for r in table() if not only or r.id in only]
    manifest = {"schema": "kid-game-rules/1", "voice": voice, "status": "draft_needs_audition",
                "lines": []}
    for r in rules:
        sha = speech._generate(r.text, output / f"{r.id}.wav", voice, synthesize=synthesize,
                               replace=replace, max_seconds=MAX_SECONDS)
        manifest["lines"].append({"id": r.id, "games": list(r.games), "text": r.text,
                                  "sha256": sha})
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
