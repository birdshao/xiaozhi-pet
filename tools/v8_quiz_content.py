#!/usr/bin/env python3
"""V8 quiz banks for the three voice kinds (docs/V8_MINI_GAMES.md).

animal_sound, big_small and english_word need spoken questions, so they are
KPG1 game packs (TF card), not firmware tables. This tool holds the reviewed
question text, writes a game.json source with one WAV per unique line and the
review checklist for the grown-up who listens to every line.

  source <dir> --pack animals|bigsmall|english [--voice tones|edge] [--approve]
  checklist [--output docs/V8_QUIZ_REVIEW_CHECKLIST.md]
  packs                    print pack ids and round counts

Voices: `tones` writes deterministic placeholder beeps (tests, never child
content); `edge` uses the online Edge TTS through content_pipeline.speech
(needs edge-tts and ffmpeg; check the service terms, see
docs/LEARNING_REAL_ASSETS_TTS.md). TTS output is a draft: nothing is
approved until a grown-up listened (--approve only after that).
Animal calls are spoken onomatopoeia for now; TODO real recordings.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path

from tools import game_pack as gp

ROOT = Path(__file__).resolve().parent.parent
PLAY_CONTENT_CC = ROOT / "firmware/components/kid_app/src/play_content.cc"
ZH_VOICE = "zh-CN-XiaoxiaoNeural"
EN_VOICE = "en-US-AnaNeural"


@dataclass(frozen=True)
class Line:
    """One spoken line: the text, its language and what it is for."""
    text: str
    lang: str  # "zh" or "en"


@dataclass(frozen=True)
class Round:
    kind: str
    prompt: Line
    answer: str
    options: tuple[tuple[str, Line], ...]  # (art, spoken name)


@dataclass(frozen=True)
class Pack:
    pack_id: str
    game_id: str
    content_id: int
    title: str
    rounds: tuple[Round, ...]


ANIMALS = {  # art -> (name, what it says)
    "cat": ("小猫", "喵喵喵，是谁在叫？"),
    "dog": ("小狗", "汪汪汪，是谁在叫？"),
    "bird": ("小鸟", "叽叽喳喳，是谁在叫？"),
    "fish": ("小鱼", "咕嘟咕嘟，是谁在吐泡泡？"),
}


def _others(items: list[str], skip: str) -> list[str]:
    return [i for i in items if i != skip]


def animal_pack() -> Pack:
    arts = list(ANIMALS)
    rounds: list[Round] = []
    for target in arts:
        rest = _others(arts, target)
        # Three cards: the target and two of the other three; two cards: one.
        sets = [[target, rest[i], rest[j]] for i, j in ((0, 1), (0, 2), (1, 2))]
        sets += [[target, other] for other in rest]
        for n, cards in enumerate(sets):
            cards = cards[n % len(cards):] + cards[:n % len(cards)]  # rotate positions
            rounds.append(Round(
                "animal_sound", Line(ANIMALS[target][1], "zh"), target,
                tuple((a, Line(ANIMALS[a][0], "zh")) for a in cards)))
    return Pack("animals", "v8_animal_sounds", 201, "听声音猜动物", tuple(rounds))


def bigsmall_pack() -> Pack:
    rounds: list[Round] = []
    size_name = {"small": "小的", "medium": "中间的", "large": "大的"}
    for shape in gp.SIZED_SHAPES:
        art = {s: f"{shape}_{s}" for s in gp.SIZED_NAMES}

        def opts(sizes: list[str]) -> tuple[tuple[str, Line], ...]:
            return tuple((art[s], Line(size_name[s], "zh")) for s in sizes)

        rounds += [
            Round("big_small", Line("哪个大？", "zh"), art["large"], opts(["small", "large"])),
            Round("big_small", Line("哪个小？", "zh"), art["small"], opts(["large", "small"])),
            Round("big_small", Line("哪个最大？", "zh"), art["large"],
                  opts(["medium", "large", "small"])),
            Round("big_small", Line("哪个最小？", "zh"), art["small"],
                  opts(["large", "medium", "small"])),
            Round("big_small", Line("哪个是中间的？", "zh"), art["medium"],
                  opts(["small", "large", "medium"])),
        ]
    return Pack("bigsmall", "v8_big_small", 202, "大和小", tuple(rounds))


WORDS = {  # art -> English word
    "cat": "cat", "dog": "dog", "bird": "bird", "fish": "fish", "circle": "circle",
    "square": "square", "triangle": "triangle", "star": "star", "heart": "heart",
    "dots1": "one", "dots2": "two", "dots3": "three", "dots4": "four", "dots5": "five",
}


def english_pack() -> Pack:
    arts = list(WORDS)
    rounds: list[Round] = []
    for n, target in enumerate(arts):
        for k in range(2 if n < 10 else 1):  # 24 rounds, every word at least once
            i = (n * 3 + k * 5 + 1) % len(arts)
            j = (n * 5 + k * 3 + 2) % len(arts)
            while arts[i] == target:
                i = (i + 1) % len(arts)
            while arts[j] in (target, arts[i]):
                j = (j + 1) % len(arts)
            cards = [target, arts[i], arts[j]]
            shift = (n + k) % 3
            cards = cards[shift:] + cards[:shift]
            prompt = f"Show me {WORDS[target]}." if WORDS[target] in (
                "one", "two", "three", "four", "five") else f"Where is the {WORDS[target]}?"
            rounds.append(Round("english_word", Line(prompt, "en"), target,
                                tuple((a, Line(WORDS[a], "en")) for a in cards)))
    return Pack("english", "v8_english_words", 203, "英语单词卡", tuple(rounds))


PACKS = {"animals": animal_pack, "bigsmall": bigsmall_pack, "english": english_pack}


def wav_name(line: Line) -> str:
    digest = hashlib.sha256(f"{line.lang}:{line.text}".encode()).hexdigest()[:10]
    return f"{line.lang}_{digest}.wav"


def _placeholder(line: Line) -> bytes:
    seed = int(hashlib.sha256(line.text.encode()).hexdigest()[:6], 16)
    return gp.tone([300 + seed % 400, 500 + (seed >> 8) % 400], 0.12)


def _synthesize_edge(line: Line, path: Path) -> None:
    from content_pipeline.speech import _edge_backend  # optional online service

    _edge_backend(line.text, path, ZH_VOICE if line.lang == "zh" else EN_VOICE)


def unique_lines(pack: Pack) -> list[Line]:
    seen: dict[str, Line] = {}
    for r in pack.rounds:
        for line in (r.prompt, *(o[1] for o in r.options)):
            seen.setdefault(wav_name(line), line)
    return list(seen.values())


def write_source(pack: Pack, out_dir: Path, voice: str, approve: bool) -> Path:
    """game.json plus one WAV per unique line; approved only on request."""
    out_dir.mkdir(parents=True, exist_ok=True)
    for line in unique_lines(pack):
        path = out_dir / wav_name(line)
        if voice == "edge":
            _synthesize_edge(line, path)
        else:
            gp.write_wav(path, _placeholder(line))
    rounds = [{
        "kind": r.kind, "prompt": r.prompt.text, "prompt_audio": wav_name(r.prompt),
        "question_art": "listen", "answer": r.answer, "approved": approve,
        "options": [{"art": a, "label": line.text, "audio": wav_name(line)}
                    for a, line in r.options],
    } for r in pack.rounds]
    source = out_dir / "game.json"
    source.write_text(json.dumps({
        "schema_version": 1, "package_type": "game", "game_id": pack.game_id,
        "content_id": pack.content_id, "revision": 1, "title": pack.title,
        "reviewer": "待审核", "approved": approve, "rounds": rounds,
    }, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return source


def play_prompts() -> list[tuple[str, str]]:
    """(game, text) of every PlayGamePromptText case in play_content.cc."""
    text = PLAY_CONTENT_CC.read_text(encoding="utf-8")
    body = text[text.index("const char* PlayGamePromptText"):]
    out: list[tuple[str, str]] = []
    pending: list[str] = []
    for line in body.splitlines():
        m = re.search(r"case PlayGame::(\w+):", line)
        if m and m.group(1) != "None":
            pending.append(m.group(1))
        r = re.search(r'return "([^"]+)";', line)
        if r and pending:
            out += [(g, r.group(1)) for g in pending]
            pending = []
    return out


def checklist() -> str:
    lines = [
        "# V8 题库审核清单（自动生成，勿手改）", "",
        "由 `python -m tools.v8_quiz_content checklist` 生成。每一条都要大人**亲耳听过**再勾选；",
        "TTS 只是草稿，勾选之前 `approved` 保持 false。动物叫声 TODO：先用拟声兜底，后续换真人录音。", "",
    ]
    for make in PACKS.values():
        pack = make()
        lines += [f"## {pack.title}（`{pack.pack_id}`，content_id {pack.content_id}，"
                  f"{len(pack.rounds)} 题）", ""]
        for line in unique_lines(pack):
            voice = ZH_VOICE if line.lang == "zh" else EN_VOICE
            lines.append(f"- [ ] `{wav_name(line)}` ({voice}) {line.text}")
        lines.append("")
    lines += ["## 内置游戏的口头提示（固件暂用提示音，TODO 录音/TTS 后接入）", ""]
    lines += [f"- [ ] `{game}`：{text}" for game, text in play_prompts()]
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="cmd", required=True)
    src = sub.add_parser("source")
    src.add_argument("dir", type=Path)
    src.add_argument("--pack", choices=sorted(PACKS), required=True)
    src.add_argument("--voice", choices=["tones", "edge"], default="tones")
    src.add_argument("--approve", action="store_true")
    chk = sub.add_parser("checklist")
    chk.add_argument("--output", type=Path, default=ROOT / "docs/V8_QUIZ_REVIEW_CHECKLIST.md")
    sub.add_parser("packs")
    args = parser.parse_args(argv)
    if args.cmd == "source":
        print(write_source(PACKS[args.pack](), args.dir, args.voice, args.approve))
    elif args.cmd == "checklist":
        args.output.write_text(checklist(), encoding="utf-8", newline="\n")
        print(args.output)
    else:
        for name, make in PACKS.items():
            pack = make()
            print(f"{name} id={pack.content_id} rounds={len(pack.rounds)} "
                  f"lines={len(unique_lines(pack))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
