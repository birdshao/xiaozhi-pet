#!/usr/bin/env python3
"""V8 sound packs: routine/parrot/song prompts and the "guess the sound" libraries.

Same KPS1 container, reader and verification as the default sound pack
(tools/sound_pack.py, docs/V2_SOUND_PACK.md); only the clip catalogue differs.
Each pack is its own file /sounds/<pack>.kps on the TF card, so ids restart at 1
in every pack and the 96-clip / 8 MiB limits apply per pack.

  routines          spoken prompts of the brushing / hand-washing companion,
                    the parrot and the song player (reviewed wording below)
  guess_animals     sounds for the "what sound is this" games (animal_sound ...)
  guess_vehicles
  guess_music       instruments
  guess_daily       everyday sounds

The sound FILES are not in the repository: every row is a TODO for a real
recording or a licensed clip (docs/V8_SOUND_AND_ROUTINES.md section 6).

  checklist [--pack P]                 what to record (markdown table)
  catalog                              JSON catalogue for the game logic (session 2)
  convert <recordings> <clips> --pack P   ffmpeg to 24 kHz mono PCM16
  pack <clips> --pack P --output X.kps --reviewed [--revision N]
  build <recordings> --pack P --output X.kps --reviewed
  demo --output <dir>                  placeholder tones for every pack (NOT child content)
  verify <X.kps>

Clip ids are stable and append-only: never renumber or reuse one.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import array
import shutil
import subprocess
import sys
import tempfile
import zlib
from dataclasses import dataclass
from pathlib import Path

from tools import sound_pack as sp

SOUND_SECONDS = 4     # a guess-the-sound clip is at most 4 s
PROMPT_SECONDS = 6    # a spoken prompt is at most 6 s


@dataclass(frozen=True)
class Item:
    id: int
    name: str      # recording / clip file stem
    kind: str      # prompt | sound
    text: str      # prompt wording, or the Chinese name of the sound
    max_seconds: int
    note: str = ""  # where it plays / what the sound is


def _prompts() -> list[Item]:
    rows = [
        (1, "brush_start", "好，我们开始刷牙啦。", "刷牙：按下拨轮后"),
        (2, "brush_middle", "刷刷里面，上上下下，慢慢刷。", "刷牙：过半时"),
        (3, "brush_done", "刷牙完成，你真棒，牙齿亮晶晶。", "刷牙：完成后夸奖"),
        (11, "wash_start", "好，我们开始洗手啦。", "洗手：按下拨轮后"),
        (12, "wash_middle", "搓搓手心，搓搓手背，还有手指缝。", "洗手：过半时"),
        (13, "wash_done", "洗手完成，小手干干净净，真棒。", "洗手：完成后夸奖"),
        (21, "parrot_ready", "按住拨轮说话，我来学一学。", "鹦鹉：进入时"),
        (22, "parrot_again", "再说一次吧。", "鹦鹉：一次学完后"),
        (31, "song_none", "卡里还没有歌，请大人放进来吧。", "儿歌：卡里没有歌"),
        (32, "song_timer", "我们听着歌，慢慢睡着吧。", "摇篮曲：选定时间后"),
    ]
    return [Item(i, n, "prompt", t, PROMPT_SECONDS, w) for i, n, t, w in rows]


def _sounds(rows: list[tuple[str, str]]) -> list[Item]:
    return [Item(i + 1, n, "sound", zh, SOUND_SECONDS, "声音猜猜看") for i, (n, zh) in enumerate(rows)]


PACKS: dict[str, list[Item]] = {
    "routines": _prompts(),
    "guess_animals": _sounds([
        ("cat", "小猫"), ("dog", "小狗"), ("duck", "小鸭子"), ("cow", "奶牛"), ("pig", "小猪"),
        ("rooster", "公鸡"), ("sheep", "小羊"), ("frog", "青蛙"), ("bird", "小鸟"), ("horse", "小马"),
    ]),
    "guess_vehicles": _sounds([
        ("car", "汽车"), ("train", "火车"), ("bus", "公共汽车"), ("ambulance", "救护车"),
        ("plane", "飞机"), ("boat", "轮船"), ("bike_bell", "自行车铃"), ("fire_truck", "消防车"),
    ]),
    "guess_music": _sounds([
        ("drum", "小鼓"), ("piano", "钢琴"), ("violin", "小提琴"), ("guitar", "吉他"),
        ("trumpet", "小号"), ("bell", "铃铛"), ("flute", "笛子"), ("xylophone", "木琴"),
    ]),
    "guess_daily": _sounds([
        ("doorbell", "门铃"), ("phone", "电话"), ("water", "流水"), ("alarm_clock", "闹钟"),
        ("knock", "敲门"), ("clap", "拍手"), ("rain", "下雨"), ("kettle", "水壶"),
    ]),
}
CATEGORY_ZH = {"guess_animals": "动物", "guess_vehicles": "交通工具", "guess_music": "乐器",
               "guess_daily": "生活声音", "routines": "提示句"}


def items(pack: str) -> list[Item]:
    if pack not in PACKS:
        raise ValueError(f"unknown pack {pack!r}; one of {', '.join(PACKS)}")
    return PACKS[pack]


def catalog_json() -> dict:
    return {"format": "KPS1", "path": "/sounds/<pack>.kps",
            "packs": {p: {"category": CATEGORY_ZH[p],
                          "clips": [{"id": i.id, "name": i.name, "text": i.text, "kind": i.kind}
                                    for i in rows]}
                      for p, rows in PACKS.items()}}


def _clip(item: Item) -> sp.Clip:
    return sp.Clip(item.id, item.name, "option" if item.kind == "sound" else "phrase",
                   item.text, item.max_seconds)


def checklist(pack: str | None = None) -> str:
    out = []
    for name, rows in PACKS.items():
        if pack not in (None, name):
            continue
        out.append(f"### {name}（{CATEGORY_ZH[name]}）\n")
        out += ["| id | 文件名 | 类型 | 内容 | 用在哪 | 最长 |", "|---|---|---|---|---|---|"]
        for i in rows:
            kind = "提示句" if i.kind == "prompt" else "声音"
            out.append(f"| {i.id} | {i.name} | {kind} | {i.text} | {i.note} | {i.max_seconds} 秒 |")
        out.append("")
    return "\n".join(out)


def validate_catalog() -> None:
    """Called by the tests: ids unique and stable, sizes inside the KPS1 limits."""
    for name, rows in PACKS.items():
        if not sp.PACK_ID_RE.match(name):
            raise ValueError(f"{name}: bad pack id")
        ids = [r.id for r in rows]
        names = [r.name for r in rows]
        if len(set(ids)) != len(ids) or len(set(names)) != len(names):
            raise ValueError(f"{name}: duplicate id or file name")
        if not rows or len(rows) > sp.MAX_CLIPS or not all(1 <= i <= 999 for i in ids):
            raise ValueError(f"{name}: bad clip count / id")
        worst = sum(r.max_seconds * sp.RATE * 2 for r in rows) + 32 + 16 * len(rows) + 4 * len(rows)
        if worst > sp.MAX_BYTES:
            raise ValueError(f"{name}: worst case {worst} bytes exceeds {sp.MAX_BYTES}")
        if any(r.max_seconds > sp.PHRASE_SECONDS for r in rows):
            raise ValueError(f"{name}: clip longer than the KPS1 6 s limit")


def convert(recordings: Path, clips_dir: Path, pack: str) -> list[str]:
    if shutil.which("ffmpeg") is None:
        raise ValueError("ffmpeg not found on PATH")
    clips_dir.mkdir(parents=True, exist_ok=True)
    done = []
    for item in items(pack):
        found = sorted(p for p in recordings.glob(f"{item.name}.*") if p.is_file())
        if not found:
            continue
        dst = clips_dir / f"{item.name}.wav"
        subprocess.run(sp.ffmpeg_command(found[0], dst, item.max_seconds), check=True)
        done.append(item.name)
    return done


def make_pack(clips_dir: Path, output: Path, pack: str, *, reviewed: bool, revision: int = 0,
              placeholder: bool = False) -> dict:
    if not reviewed:
        raise ValueError("pass --reviewed: an adult listened to every clip")
    if output.suffix != ".kps" or output.stem != pack:
        raise ValueError(f"output must be {pack}.kps")
    known = {i.name: i for i in items(pack)}
    pcm: dict[int, bytes] = {}
    entries = []
    for path in sorted(clips_dir.glob("*.wav")):
        item = known.get(path.stem)
        if item is None:
            raise ValueError(f"{path.name}: not in the {pack} checklist")
        pcm[item.id] = sp.read_clip(path, _clip(item))
        entries.append({"id": item.id, "name": item.name, "kind": item.kind, "text": item.text,
                        "duration_ms": len(pcm[item.id]) * 1000 // (2 * sp.RATE),
                        "crc32": f"{zlib.crc32(pcm[item.id]):08x}"})
    if not pcm:
        raise ValueError(f"no clips in {clips_dir}")
    blob = sp.encode(pcm, revision)
    sp.decode(blob)  # never write what the firmware would refuse
    sp._write_atomic(output, blob)
    entries.sort(key=lambda e: e["id"])
    manifest = {
        "format": "KPS1", "version": sp.VERSION, "package_type": "soundpack", "pack_id": pack,
        "revision": revision, "bytes": len(blob), "sha256": hashlib.sha256(blob).hexdigest(),
        "reviewed": True, "placeholder": placeholder, "sd_path": f"/sounds/{pack}.kps",
        "clips": entries,
        "missing": sorted(i.name for i in known.values() if i.id not in pcm),
    }
    sp._write_atomic(output.with_suffix(".manifest.json"),
                     (json.dumps(manifest, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    return manifest


def _tone(freq: int, seconds: float = 0.4) -> bytes:
    pcm = array.array("h")
    for n in range(int(sp.RATE * seconds)):
        fade = min(1.0, n / 600, (sp.RATE * seconds - n) / 600)
        pcm.append(int(3000 * fade * math.sin(2 * math.pi * freq * n / sp.RATE)))
    return pcm.tobytes()


def demo(out_dir: Path) -> dict[str, dict]:
    """Placeholder tones for every pack (a smoke test of the TF path, not content)."""
    result = {}
    for pack, rows in PACKS.items():
        with tempfile.TemporaryDirectory() as tmp:
            for n, item in enumerate(rows):
                import wave
                with wave.open(str(Path(tmp) / f"{item.name}.wav"), "wb") as w:
                    w.setnchannels(1)
                    w.setsampwidth(2)
                    w.setframerate(sp.RATE)
                    w.writeframes(_tone(300 + 40 * n))
            result[pack] = make_pack(Path(tmp), out_dir / "sounds" / f"{pack}.kps", pack,
                                     reviewed=True, placeholder=True)
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("checklist")
    c.add_argument("--pack", choices=tuple(PACKS))
    sub.add_parser("catalog")
    c = sub.add_parser("convert")
    c.add_argument("recordings", type=Path)
    c.add_argument("clips", type=Path)
    c.add_argument("--pack", choices=tuple(PACKS), required=True)
    for name in ("pack", "build"):
        c = sub.add_parser(name)
        c.add_argument("source", type=Path)
        c.add_argument("--pack", choices=tuple(PACKS), required=True)
        c.add_argument("--output", type=Path, required=True)
        c.add_argument("--reviewed", action="store_true")
        c.add_argument("--revision", type=int, default=0)
    c = sub.add_parser("demo")
    c.add_argument("--output", type=Path, required=True)
    c = sub.add_parser("verify")
    c.add_argument("pack", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.cmd == "checklist":
            print(checklist(args.pack))
        elif args.cmd == "catalog":
            print(json.dumps(catalog_json(), ensure_ascii=False, indent=2))
        elif args.cmd == "convert":
            print("converted:", ", ".join(convert(args.recordings, args.clips, args.pack)) or "nothing")
        elif args.cmd == "pack":
            m = make_pack(args.source, args.output, args.pack, reviewed=args.reviewed,
                          revision=args.revision)
            print(f"{args.output}: {len(m['clips'])} clips, {m['bytes']} bytes")
        elif args.cmd == "build":
            with tempfile.TemporaryDirectory() as tmp:
                convert(args.source, Path(tmp), args.pack)
                m = make_pack(Path(tmp), args.output, args.pack, reviewed=args.reviewed,
                              revision=args.revision)
            print(f"{args.output}: {len(m['clips'])} clips, {m['bytes']} bytes; "
                  f"missing: {', '.join(m['missing']) or 'none'}")
        elif args.cmd == "demo":
            for pack, m in demo(args.output).items():
                print(f"{args.output / 'sounds' / (pack + '.kps')}: {len(m['clips'])} clips (tones only)")
        else:
            revision, clips = sp.decode(args.pack.read_bytes())
            print(f"OK revision={revision} clips={','.join(map(str, clips))}")
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
