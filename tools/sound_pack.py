#!/usr/bin/env python3
"""KPS1 sound pack (V2-08): reviewed option names and pet lines for the TF card.

The firmware plays option names when the child stops on a home-rail item
and short pet lines (V5-04) on greeting, feeding, giggling, good morning and
good night. It looks for /sdcard/sounds/default.kps; without a valid pack it
uses the option clips embedded in the app and a short chime per line.

  checklist [--temperament T]     what to record (id, file name, wording)
  convert <recordings> <clips>    recordings/<name>.<any> -> clips/<name>.wav
                                  (ffmpeg: mono, 24 kHz, PCM16, trimmed, level)
  pack <clips> --output X.kps --reviewed [--revision N]
                                  compile reviewed clips + X.manifest.json
  build <recordings> --output X.kps --reviewed [--embed]
                                  convert + pack in one go; --embed also
                                  replaces the 5 option clips in the firmware
  demo --output <dir>             placeholder tones in <dir>/sounds/default.kps
                                  for a TF smoke test (NOT child content)
  verify <X.kps>                  check a pack the way the firmware does

File names: option clips use the home item (chat, story, game, feed, sleep),
pet lines their phrase id (e.g. 211). Missing files are allowed: the
firmware falls back clip by clip. The wording comes from the reviewed phrase
table in firmware/components/kid_app/src/personality.cc (single source).

Binary layout (little-endian), see docs/V2_SOUND_PACK.md:
  header 32 B  <4sHHIIIIII  magic KPS1, version 1, clip count, sample rate
               24000, revision, data offset, total bytes, table CRC32,
               CRC32 of header bytes 0..27
  table 16 B per clip  <HHIII  id (ascending), 0, offset, bytes, CRC32
  data  PCM16 per clip, each starting 4-byte aligned, zero padding
"""
from __future__ import annotations

import argparse
import array
import hashlib
import json
import math
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import wave
import zlib
from dataclasses import dataclass
from pathlib import Path

from tools.make_prompt_audio import ffmpeg_command as _prompt_ffmpeg

ROOT = Path(__file__).resolve().parent.parent
PERSONALITY_CC = ROOT / "firmware/components/kid_app/src/personality.cc"
PROMPT_DIR = ROOT / "firmware/components/kid_app/assets/prompts"

MAGIC = b"KPS1"
VERSION = 1
RATE = 24000
HEADER = struct.Struct("<4sHHIIIIII")  # 32 B
RECORD = struct.Struct("<HHIII")  # 16 B
MAX_CLIPS = 96
MAX_BYTES = 8 * 1024 * 1024
OPTION_SECONDS = 2
PHRASE_SECONDS = 6
MIN_SAMPLES = RATE // 10  # 0.1 s
PACK_ID_RE = re.compile(r"^[a-z0-9][a-z0-9_-]{2,62}$")
DEFAULT_PACK_ID = "default"

# HomeItem order: clip id = 1 + index (OptionClipId in sound_bank.h).
OPTIONS = (("chat", "聊天"), ("story", "故事"), ("game", "游戏"),
           ("feed", "喂食"), ("sleep", "睡觉"))
TEMPERAMENTS = ("curious", "gentle", "lively")  # Temperament order
TEMPERAMENT_ZH = {"curious": "好奇型", "gentle": "温柔型", "lively": "活泼型"}
CONTEXTS = ("greeting", "morning", "favorite", "feed", "story", "bedtime", "giggle")
# Contexts the firmware speaks today (V5-04 first half) and when.
PLAYED = {
    "greeting": "拿起团团 / 选聊天、游戏时打招呼",
    "morning": "定时开机的早安",
    "feed": "喂食后",
    "bedtime": "哄睡时（睡觉画面）",
    "giggle": "轻轻摇团团",
}
_PHRASE_RE = re.compile(
    r'\{(\d+),\s*T::(\w+),\s*C::(\w+),\s*S::(\w+),\s*(\d+),\s*"([^"]*)"\}')
_CONTEXT_OF = {"Greeting": "greeting", "MorningRecap": "morning",
               "FavoriteMention": "favorite", "Feed": "feed",
               "StoryInvite": "story", "Bedtime": "bedtime", "Giggle": "giggle"}


@dataclass(frozen=True)
class Clip:
    id: int
    name: str  # recording / clip file stem
    kind: str  # option | phrase
    text: str  # reviewed wording
    max_seconds: int
    temperament: str = ""
    context: str = ""


def phrase_table(source: Path = PERSONALITY_CC) -> list[Clip]:
    """Slot-free reviewed lines of the played contexts, from personality.cc."""
    clips = []
    for m in _PHRASE_RE.finditer(source.read_text(encoding="utf-8")):
        pid, temperament, context, slot, _, text = m.groups()
        context = _CONTEXT_OF[context]
        if slot != "None" or context not in PLAYED:
            continue  # {book}/{like} lines need synthesis, not recordings
        clips.append(Clip(int(pid), pid, "phrase", text, PHRASE_SECONDS,
                          temperament.lower(), context))
    if not clips:
        raise ValueError(f"no reviewed phrases found in {source}")
    return clips


def catalog() -> list[Clip]:
    options = [Clip(1 + i, name, "option", zh, OPTION_SECONDS)
               for i, (name, zh) in enumerate(OPTIONS)]
    return options + phrase_table()


def _by_name() -> dict[str, Clip]:
    return {c.name: c for c in catalog()}


def read_clip(path: Path, clip: Clip) -> bytes:
    """PCM16 bytes of a 24 kHz mono 16-bit WAV, within the clip's limits."""
    try:
        with wave.open(str(path), "rb") as w:
            if w.getnchannels() != 1:
                raise ValueError(f"{path.name}: must be mono")
            if w.getsampwidth() != 2 or w.getcomptype() != "NONE":
                raise ValueError(f"{path.name}: must be 16-bit PCM")
            if w.getframerate() != RATE:
                raise ValueError(f"{path.name}: must be {RATE} Hz")
            frames = w.getnframes()
            pcm = w.readframes(frames)
    except wave.Error as exc:
        raise ValueError(f"{path.name}: not a PCM WAV ({exc})") from exc
    if frames < MIN_SAMPLES:
        raise ValueError(f"{path.name}: shorter than 0.1 s")
    if frames > clip.max_seconds * RATE:
        raise ValueError(f"{path.name}: longer than {clip.max_seconds} s")
    if len(pcm) != frames * 2:
        raise ValueError(f"{path.name}: truncated audio data")
    return pcm


def _align4(value: int) -> int:
    return (value + 3) & ~3


def encode(clips: dict[int, bytes], revision: int = 0) -> bytes:
    """KPS1 bytes for {clip id: PCM16}; deterministic for the same input."""
    if not clips or len(clips) > MAX_CLIPS:
        raise ValueError(f"a pack holds 1..{MAX_CLIPS} clips")
    if not 0 <= revision <= 0xFFFFFFFF:
        raise ValueError("revision must fit in 32 bits")
    ids = sorted(clips)
    data_offset = HEADER.size + RECORD.size * len(ids)
    table, body = bytearray(), bytearray()
    for cid in ids:
        pcm = clips[cid]
        if not 1 <= cid <= 999 or not pcm or len(pcm) % 2:
            raise ValueError(f"clip {cid}: bad id or PCM length")
        body.extend(b"\0" * (_align4(data_offset + len(body)) - data_offset - len(body)))
        table += RECORD.pack(cid, 0, data_offset + len(body), len(pcm), zlib.crc32(pcm))
        body += pcm
    total = data_offset + len(body)
    if total > MAX_BYTES:
        raise ValueError(f"pack larger than {MAX_BYTES} bytes")
    head = HEADER.pack(MAGIC, VERSION, len(ids), RATE, revision, data_offset, total,
                       zlib.crc32(table), 0)[:28]
    return head + struct.pack("<I", zlib.crc32(head)) + bytes(table) + bytes(body)


def decode(blob: bytes) -> tuple[int, dict[int, bytes]]:
    """(revision, {id: PCM16}) after the checks the firmware reader makes."""
    if len(blob) < HEADER.size or len(blob) > MAX_BYTES:
        raise ValueError("pack size out of range")
    magic, version, count, rate, revision, data_offset, total, table_crc, crc = \
        HEADER.unpack_from(blob)
    if magic != MAGIC or version != VERSION or rate != RATE:
        raise ValueError("not a KPS1 v1 24 kHz pack")
    if crc != zlib.crc32(blob[:28]):
        raise ValueError("header CRC mismatch")
    if not 1 <= count <= MAX_CLIPS or data_offset != HEADER.size + RECORD.size * count \
            or total != len(blob):
        raise ValueError("header sizes inconsistent")
    table = blob[HEADER.size:data_offset]
    if zlib.crc32(table) != table_crc:
        raise ValueError("clip table CRC mismatch")
    clips, end, last = {}, data_offset, 0
    for i in range(count):
        cid, reserved, offset, size, ccrc = RECORD.unpack_from(table, i * RECORD.size)
        if cid <= last or cid > 999 or reserved or size < 2 or size % 2 \
                or size > PHRASE_SECONDS * RATE * 2 or offset != _align4(end):
            raise ValueError(f"clip {cid}: bad table entry")
        pcm = blob[offset:offset + size]
        if len(pcm) != size or zlib.crc32(pcm) != ccrc:
            raise ValueError(f"clip {cid}: CRC mismatch")
        clips[cid], end, last = pcm, offset + size, cid
    if end != total:
        raise ValueError("unexpected bytes after the last clip")
    return revision, clips


def _write_atomic(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as tmp:
        tmp.write(data)
    Path(tmp.name).replace(path)


def make_pack(clips_dir: Path, output: Path, *, reviewed: bool, revision: int = 0,
              placeholder: bool = False) -> dict:
    """Compile <clips_dir>/<name>.wav into output (.kps) + .manifest.json."""
    if not reviewed:
        raise ValueError("pass --reviewed: an adult listened to every clip")
    if output.suffix != ".kps" or not PACK_ID_RE.match(output.stem):
        raise ValueError("output must be <pack_id>.kps, pack_id like 'default'")
    known = _by_name()
    pcm: dict[int, bytes] = {}
    entries = []
    for path in sorted(clips_dir.glob("*.wav")):
        clip = known.get(path.stem)
        if clip is None:
            raise ValueError(f"{path.name}: not in the checklist (see 'checklist')")
        pcm[clip.id] = read_clip(path, clip)
        entries.append({"id": clip.id, "name": clip.name, "kind": clip.kind,
                        "temperament": clip.temperament, "context": clip.context,
                        "text": clip.text,
                        "duration_ms": len(pcm[clip.id]) * 1000 // (2 * RATE),
                        "crc32": f"{zlib.crc32(pcm[clip.id]):08x}"})
    if not pcm:
        raise ValueError(f"no clips in {clips_dir}")
    blob = encode(pcm, revision)
    decode(blob)  # never write what the firmware would refuse
    _write_atomic(output, blob)
    entries.sort(key=lambda e: e["id"])
    missing = sorted(c.name for c in known.values() if c.id not in pcm)
    manifest = {
        "format": "KPS1", "version": VERSION, "pack_id": output.stem,
        "revision": revision, "bytes": len(blob),
        "sha256": hashlib.sha256(blob).hexdigest(),
        "reviewed": True, "placeholder": placeholder,
        "sd_path": f"/sounds/{output.stem}.kps",
        "clips": entries, "missing_fall_back_to_builtin": missing,
    }
    manifest_path = output.with_suffix(".manifest.json")
    _write_atomic(manifest_path, (json.dumps(manifest, ensure_ascii=False, indent=2)
                                  + "\n").encode("utf-8"))
    return manifest


def ffmpeg_command(src: Path, dst: Path, seconds: int) -> list[str]:
    """Same conversion as the embedded option prompts, longer limit."""
    return _prompt_ffmpeg(src, dst, seconds)


def convert(recordings: Path, clips_dir: Path) -> list[str]:
    """ffmpeg every known recording; returns the converted names."""
    if shutil.which("ffmpeg") is None:
        raise ValueError("ffmpeg not found on PATH")
    clips_dir.mkdir(parents=True, exist_ok=True)
    done = []
    for clip in catalog():
        found = sorted(p for p in recordings.glob(f"{clip.name}.*") if p.is_file())
        if not found:
            continue
        dst = clips_dir / f"{clip.name}.wav"
        subprocess.run(ffmpeg_command(found[0], dst, clip.max_seconds), check=True)
        done.append(clip.name)
    unknown = sorted(p.name for p in recordings.iterdir()
                     if p.is_file() and p.stem not in _by_name())
    if unknown:
        print("ignored (not in the checklist):", ", ".join(unknown))
    return done


def _tone(freqs: list[int], note_s: float) -> bytes:
    pcm = array.array("h")
    for freq in freqs:
        n = int(RATE * note_s)
        for i in range(n):
            env = min(1.0, i / (0.008 * RATE), (n - i) / (0.040 * RATE))
            s = math.sin(2 * math.pi * freq * i / RATE)
            pcm.append(int(0.18 * 32767 * env * s))
        pcm.extend([0] * int(RATE * 0.03))
    if sys.byteorder != "little":
        pcm.byteswap()
    return pcm.tobytes()


DEMO_TONES = {"greeting": [880, 1175], "morning": [523, 784, 1047],
              "feed": [784, 1047, 784], "bedtime": [392, 330],
              "giggle": [1319, 1568, 1319, 1568]}


def demo(out_dir: Path) -> dict:
    """Placeholder pack (synthetic tones) to check the TF path on the board."""
    with tempfile.TemporaryDirectory() as tmp:
        clips = Path(tmp)
        for name, _ in OPTIONS:
            shutil.copyfile(PROMPT_DIR / f"prompt_{name}.wav", clips / f"{name}.wav")
        for clip in phrase_table():
            # Temperament shifts the pitch so a listener hears which one played.
            shift = 1 + 0.12 * TEMPERAMENTS.index(clip.temperament)
            pcm = _tone([int(f * shift) for f in DEMO_TONES[clip.context]], 0.12)
            with wave.open(str(clips / f"{clip.name}.wav"), "wb") as w:
                w.setnchannels(1)
                w.setsampwidth(2)
                w.setframerate(RATE)
                w.writeframes(pcm)
        manifest = make_pack(clips, out_dir / "sounds" / f"{DEFAULT_PACK_ID}.kps",
                             reviewed=True, placeholder=True)
    (out_dir / "README-SMOKE-ONLY.txt").write_text(
        "Synthetic tones for checking the TF sound pack path only. Not child content.\n"
        "Copy sounds/default.kps to /sounds/default.kps on a FAT32 TF card.\n",
        encoding="utf-8")
    return manifest


def checklist(temperament: str = "all") -> str:
    rows = ["| 文件名 | 类型 | 性格 | 什么时候播放 | 要说的话 | 最长 |",
            "|---|---|---|---|---|---|"]
    for clip in catalog():
        if clip.kind == "phrase" and temperament not in ("all", clip.temperament):
            continue
        when = "拨轮停在该选项时" if clip.kind == "option" else PLAYED[clip.context]
        kind = "选项名" if clip.kind == "option" else "宠物短句"
        who = TEMPERAMENT_ZH.get(clip.temperament, "全部")
        rows.append(f"| {clip.name} | {kind} | {who} | {when} | {clip.text} | "
                    f"{clip.max_seconds} 秒 |")
    return "\n".join(rows)


def _embed(clips_dir: Path) -> None:
    for name, _ in OPTIONS:
        src = clips_dir / f"{name}.wav"
        if src.exists():
            read_clip(src, _by_name()[name])
            shutil.copyfile(src, PROMPT_DIR / f"prompt_{name}.wav")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("checklist")
    c.add_argument("--temperament", choices=("all",) + TEMPERAMENTS, default="all")
    c = sub.add_parser("convert")
    c.add_argument("recordings", type=Path)
    c.add_argument("clips", type=Path)
    for name in ("pack", "build"):
        c = sub.add_parser(name)
        c.add_argument("source", type=Path)
        c.add_argument("--output", type=Path, required=True)
        c.add_argument("--reviewed", action="store_true")
        c.add_argument("--revision", type=int, default=0)
        if name == "build":
            c.add_argument("--embed", action="store_true")
    c = sub.add_parser("demo")
    c.add_argument("--output", type=Path, required=True)
    c = sub.add_parser("verify")
    c.add_argument("pack", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.cmd == "checklist":
            print(checklist(args.temperament))
        elif args.cmd == "convert":
            print("converted:", ", ".join(convert(args.recordings, args.clips)) or "nothing")
        elif args.cmd == "pack":
            m = make_pack(args.source, args.output, reviewed=args.reviewed,
                          revision=args.revision)
            print(f"{args.output}: {len(m['clips'])} clips, {m['bytes']} bytes")
        elif args.cmd == "build":
            if not args.reviewed:
                raise ValueError("pass --reviewed: an adult listened to every clip")
            with tempfile.TemporaryDirectory() as tmp:
                convert(args.source, Path(tmp))
                m = make_pack(Path(tmp), args.output, reviewed=True, revision=args.revision)
                if args.embed:
                    _embed(Path(tmp))
            print(f"{args.output}: {len(m['clips'])} clips, {m['bytes']} bytes; "
                  f"built-in for: {', '.join(m['missing_fall_back_to_builtin']) or 'none'}")
        elif args.cmd == "demo":
            m = demo(args.output)
            print(f"{args.output / 'sounds/default.kps'}: {len(m['clips'])} clips (tones only)")
        else:
            revision, clips = decode(args.pack.read_bytes())
            print(f"OK revision={revision} clips={','.join(map(str, clips))}")
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
