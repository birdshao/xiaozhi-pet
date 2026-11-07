#!/usr/bin/env python3
"""KPG1 mini-game pack (V4-06): reviewed rounds for the home rail's Game item.

The firmware plays a few rounds from /sdcard/games/default.kpg; without a
valid pack it uses its small built-in set (same shape, same number; no
recordings needed). Every round is reviewed content: nothing is improvised.

  pack <game.json> --output X.kpg --reviewed
                         compile a reviewed source + X.manifest.json
  verify <X.kpg>         check a pack the way the firmware does
  demo --output <dir>    placeholder tones in <dir>/games/default.kpg for a
                         TF smoke test (NOT child content)

Source (game.json, schema_version 1; WAVs: 24 kHz mono PCM16, <= 10 s):
  {"schema_version": 1, "package_type": "game", "game_id": "animal_sounds",
   "content_id": 201, "revision": 1, "title": "...", "reviewer": "...",
   "approved": true,
   "session_audio": {"right": "right.wav", "try_again": "...",
                     "reveal": "...", "bye": "..."},        (optional, each optional)
   "rounds": [{"kind": "animal_sound", "prompt": "...", "prompt_audio": "q.wav",
               "question_art": "listen", "answer": "cat", "approved": true,
               "options": [{"art": "cat", "label": "...", "audio": "cat.wav"}, ...]}]}

Binary layout (little-endian), see docs/V4_MINI_GAMES.md:
  header 32 B  <4sHHHHIIIII  magic KPG1, version 1, round count, clip count,
               content id (>= 100), revision, sample rate 24000, total bytes,
               CRC32 of both tables, CRC32 of header bytes 0..27
  round 24 B   <BBBBH3HH3HI  kind, option count, correct (1-based), 0,
               question art, 3 option arts, prompt clip, 3 option clips, 0
  clip 16 B    <HHIII  id (ascending), 0, offset, bytes, CRC32
  data         PCM16 per clip, each starting 4-byte aligned, zero padding
No text goes into the pack: prompts and labels stay in the manifest for the
reviewing adult.
"""
from __future__ import annotations

import argparse
import array
import hashlib
import json
import math
import re
import struct
import sys
import tempfile
import wave
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GAME_VIEW_H = ROOT / "firmware/components/kid_app/include/kid_app/game_view.h"

MAGIC = b"KPG1"
VERSION = 1
RATE = 24000
HEADER = struct.Struct("<4sHHHHIIIII")  # 32 B
ROUND = struct.Struct("<BBBBH3HH3HI")   # 24 B
CLIP = struct.Struct("<HHIII")          # 16 B
MAX_ROUNDS = 64
MAX_CLIPS = 128
MAX_CLIP_BYTES = 10 * RATE * 2
MAX_BYTES = 8 * 1024 * 1024
MIN_SAMPLES = RATE // 10  # 0.1 s
MIN_CONTENT_ID = 100      # 1..99: built-in sets in the firmware
ROUND_CLIP_MIN, ROUND_CLIP_MAX = 16, 999
PACK_ID_RE = re.compile(r"^[a-z0-9][a-z0-9_-]{2,62}$")
DEFAULT_PACK_ID = "default"

# GameKind (mini_game.h).
KINDS = {"match_shape": 1, "count": 2, "animal_sound": 3, "big_small": 4,
         "english_word": 5}
# Session lines (mini_game.h kGameClip*): a pack may record them.
SESSION_CLIPS = {"right": 2, "try_again": 3, "reveal": 4, "bye": 5}
# GameArt (game_view.h): append only, never renumber. "listen" is a
# question picture only.
ART = {"circle": 1, "square": 2, "triangle": 3, "star": 4, "heart": 5,
       "dots1": 11, "dots2": 12, "dots3": 13, "dots4": 14, "dots5": 15,
       "blocks1": 21, "blocks2": 22, "blocks3": 23, "blocks4": 24, "blocks5": 25,
       "ball_big": 31, "ball_small": 32,
       "cat": 41, "dog": 42, "bird": 43, "fish": 44}
QUESTION_ONLY_ART = {"listen": 90}
ART_NAMES = {v: k for k, v in {**ART, **QUESTION_ONLY_ART}.items()}


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


def _align4(value: int) -> int:
    return (value + 3) & ~3


# ---------------------------------------------------------------- source

def _exact_keys(item: object, required: set[str], optional: set[str], what: str) -> dict:
    if not isinstance(item, dict) or not required <= set(item) or \
            not set(item) <= required | optional:
        raise ValueError(f"{what} has missing or unknown fields")
    return item


def _int(value: object, low: int, high: int, what: str) -> int:
    if type(value) is not int or not low <= value <= high:
        raise ValueError(f"{what} must be an integer in {low}..{high}")
    return value


def _text(value: object, max_chars: int, what: str) -> str:
    if not isinstance(value, str) or not value.strip() or len(value) > max_chars:
        raise ValueError(f"reviewed {what} is missing or too long")
    if any(ord(ch) < 32 for ch in value) or "‮" in value:
        raise ValueError(f"control characters are not allowed in {what}")
    return value.strip()


def _approved(item: dict, what: str) -> None:
    if item.get("approved") is not True:
        raise ValueError(f"{what} is not approved")


def _asset(root: Path, path: object, what: str) -> Path:
    if not isinstance(path, str) or not path or "\\" in path:
        raise ValueError(f"invalid {what} path")
    relative = Path(path)
    if relative.is_absolute() or any(part in (".", "..") for part in relative.parts):
        raise ValueError(f"{what} must be a relative path inside the game directory")
    full = (root / relative).resolve()
    if not full.is_relative_to(root.resolve()) or not full.is_file():
        raise ValueError(f"{what} is missing or outside the game directory")
    if full.suffix.lower() != ".wav":
        raise ValueError(f"{what} must be a .wav file")
    return full


def read_wav(path: Path, what: str) -> bytes:
    """PCM16 bytes of a 24 kHz mono 16-bit WAV, 0.1..10 s."""
    try:
        with wave.open(str(path), "rb") as w:
            if w.getcomptype() != "NONE" or w.getnchannels() != 1 or \
                    w.getsampwidth() != 2 or w.getframerate() != RATE:
                raise ValueError(f"{what} must be uncompressed 24 kHz mono PCM16 WAV")
            frames = w.getnframes()
            pcm = w.readframes(frames)
    except wave.Error as exc:
        raise ValueError(f"{what}: not a PCM WAV ({exc})") from exc
    if frames < MIN_SAMPLES or frames * 2 > MAX_CLIP_BYTES:
        raise ValueError(f"{what} must be 0.1..10 s long")
    if len(pcm) != frames * 2:
        raise ValueError(f"{what}: truncated audio data")
    return pcm


def load_source(source: Path) -> dict:
    """Strictly checked game.json -> rounds, clips (id -> PCM) and review data."""
    root = source.parent
    try:
        raw = json.loads(source.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"cannot read {source}: {exc}") from exc
    game = _exact_keys(raw, {"schema_version", "package_type", "game_id", "content_id",
                             "revision", "title", "reviewer", "approved", "rounds"},
                       {"session_audio"}, "game")
    if game["schema_version"] != 1 or game["package_type"] != "game":
        raise ValueError("schema_version must be 1 and package_type 'game'")
    if not isinstance(game["game_id"], str) or not PACK_ID_RE.match(game["game_id"]):
        raise ValueError("game_id must look like 'animal_sounds'")
    content_id = _int(game["content_id"], MIN_CONTENT_ID, 0xFFFF, "content_id")
    revision = _int(game["revision"], 1, 0xFFFFFFFF, "revision")
    title = _text(game["title"], 40, "title")
    reviewer = _text(game["reviewer"], 40, "reviewer")
    _approved(game, "the game")

    clips: dict[int, bytes] = {}
    by_file: dict[Path, int] = {}
    clip_info: dict[int, dict] = {}

    def round_clip(path: object, what: str, text: str) -> int:
        full = _asset(root, path, what)
        if full in by_file:  # the same recording may serve several rounds
            return by_file[full]
        cid = ROUND_CLIP_MIN + len(by_file)
        if cid > ROUND_CLIP_MAX:
            raise ValueError("too many recordings")
        clips[cid] = read_wav(full, what)
        by_file[full] = cid
        clip_info[cid] = {"file": str(path), "text": text}
        return cid

    session = game.get("session_audio", {})
    _exact_keys(session, set(), set(SESSION_CLIPS), "session_audio")
    for name, cid in SESSION_CLIPS.items():
        if name in session:
            what = f"session_audio.{name}"
            clips[cid] = read_wav(_asset(root, session[name], what), what)
            clip_info[cid] = {"file": str(session[name]), "text": name}

    if not isinstance(game["rounds"], list) or not 1 <= len(game["rounds"]) <= MAX_ROUNDS:
        raise ValueError(f"a game has 1..{MAX_ROUNDS} rounds")
    rounds = []
    for n, item in enumerate(game["rounds"], 1):
        where = f"round {n}"
        r = _exact_keys(item, {"kind", "prompt", "prompt_audio", "answer", "approved",
                               "options"}, {"question_art"}, where)
        _approved(r, where)
        if r["kind"] not in KINDS:
            raise ValueError(f"{where}: unknown kind {r['kind']!r}")
        prompt = _text(r["prompt"], 100, f"{where} prompt")
        question = r.get("question_art", "listen")
        if question not in ART and question not in QUESTION_ONLY_ART:
            raise ValueError(f"{where}: unknown question_art {question!r}")
        options = r["options"]
        if not isinstance(options, list) or not 2 <= len(options) <= 3:
            raise ValueError(f"{where}: 2 or 3 options")
        arts, labels, option_clips = [], [], []
        for k, opt in enumerate(options, 1):
            o = _exact_keys(opt, {"art", "label", "audio"}, set(), f"{where} option {k}")
            if o["art"] not in ART:
                raise ValueError(f"{where} option {k}: unknown art {o['art']!r}")
            if o["art"] in arts:
                raise ValueError(f"{where}: the same picture twice")
            arts.append(o["art"])
            labels.append(_text(o["label"], 24, f"{where} option {k} label"))
        if r["answer"] not in arts:
            raise ValueError(f"{where}: answer must be one of the option arts")
        prompt_clip = round_clip(r["prompt_audio"], f"{where} prompt_audio", prompt)
        for k, opt in enumerate(options, 1):
            option_clips.append(round_clip(opt["audio"], f"{where} option {k} audio",
                                           labels[k - 1]))
        rounds.append({"kind": r["kind"], "prompt": prompt, "question_art": question,
                       "arts": arts, "labels": labels, "answer": r["answer"],
                       "correct": arts.index(r["answer"]) + 1,
                       "prompt_clip": prompt_clip, "option_clips": option_clips})
    if len(clips) > MAX_CLIPS:
        raise ValueError(f"a pack holds at most {MAX_CLIPS} clips")
    return {"game_id": game["game_id"], "content_id": content_id, "revision": revision,
            "title": title, "reviewer": reviewer, "rounds": rounds, "clips": clips,
            "clip_info": clip_info}


# ----------------------------------------------------------------- binary

def _art_id(name: str) -> int:
    return ART.get(name) or QUESTION_ONLY_ART[name]


def encode(game: dict) -> bytes:
    """KPG1 bytes for a loaded source; deterministic for the same input."""
    rounds, clips = game["rounds"], game["clips"]
    table = bytearray()
    for r in rounds:
        arts = [ART[a] for a in r["arts"]] + [0] * (3 - len(r["arts"]))
        oclips = list(r["option_clips"]) + [0] * (3 - len(r["option_clips"]))
        table += ROUND.pack(KINDS[r["kind"]], len(r["arts"]), r["correct"], 0,
                            _art_id(r["question_art"]), *arts, r["prompt_clip"], *oclips, 0)
    ids = sorted(clips)
    data_offset = HEADER.size + ROUND.size * len(rounds) + CLIP.size * len(ids)
    body = bytearray()
    for cid in ids:
        pcm = clips[cid]
        body.extend(b"\0" * (_align4(data_offset + len(body)) - data_offset - len(body)))
        table += CLIP.pack(cid, 0, data_offset + len(body), len(pcm), crc32(pcm))
        body += pcm
    total = data_offset + len(body)
    if total > MAX_BYTES:
        raise ValueError(f"pack larger than {MAX_BYTES} bytes")
    head = HEADER.pack(MAGIC, VERSION, len(rounds), len(ids), game["content_id"],
                       game["revision"], RATE, total, crc32(bytes(table)), 0)[:28]
    return head + struct.pack("<I", crc32(head)) + bytes(table) + bytes(body)


def parse(blob: bytes) -> dict:
    """Decoded pack after exactly the checks GamePackReader::Open makes."""
    if not HEADER.size <= len(blob) <= MAX_BYTES:
        raise ValueError("pack size out of range")
    magic, version, nrounds, nclips, content_id, revision, rate, total, table_crc, \
        head_crc = HEADER.unpack_from(blob)
    if magic != MAGIC or version != VERSION or rate != RATE:
        raise ValueError("not a KPG1 v1 24 kHz pack")
    if head_crc != crc32(blob[:28]):
        raise ValueError("header CRC mismatch")
    if not 1 <= nrounds <= MAX_ROUNDS or not 1 <= nclips <= MAX_CLIPS or \
            content_id < MIN_CONTENT_ID or revision == 0 or total != len(blob):
        raise ValueError("header fields out of range")
    clip_table = HEADER.size + ROUND.size * nrounds
    data_offset = clip_table + CLIP.size * nclips
    if total < data_offset:
        raise ValueError("tables run past the end")
    if crc32(blob[HEADER.size:data_offset]) != table_crc:
        raise ValueError("table CRC mismatch")
    rounds = []
    for i in range(nrounds):
        (kind, count, correct, flags, qart, a0, a1, a2, pclip, c0, c1, c2,
         reserved) = ROUND.unpack_from(blob, HEADER.size + i * ROUND.size)
        arts, oclips = [a0, a1, a2], [c0, c1, c2]
        where = f"round {i + 1}"
        if kind not in KINDS.values() or not 2 <= count <= 3 or \
                not 1 <= correct <= count or flags or reserved:
            raise ValueError(f"{where}: bad kind, option count or answer")
        if qart and qart not in ART_NAMES:
            raise ValueError(f"{where}: unknown question picture")
        used = arts[:count]
        if any(a not in ART.values() for a in used) or len(set(used)) != count or \
                any(arts[count:]) or any(oclips[count:]):
            raise ValueError(f"{where}: bad option pictures")
        if not all(ROUND_CLIP_MIN <= c <= ROUND_CLIP_MAX for c in [pclip] + oclips[:count]):
            raise ValueError(f"{where}: bad clip ids")
        rounds.append({"kind": kind, "correct": correct, "question_art": qart,
                       "arts": used, "prompt_clip": pclip, "option_clips": oclips[:count]})
    clips, end, last = {}, data_offset, 0
    for i in range(nclips):
        cid, zero, offset, size, ccrc = CLIP.unpack_from(blob, clip_table + i * CLIP.size)
        if not (cid in SESSION_CLIPS.values() or ROUND_CLIP_MIN <= cid <= ROUND_CLIP_MAX) \
                or cid <= last or zero or size < 2 or size % 2 or size > MAX_CLIP_BYTES \
                or offset != _align4(end) or offset + size > total:
            raise ValueError(f"clip {cid}: bad table entry")
        if any(blob[end:offset]):
            raise ValueError(f"clip {cid}: padding is not zero")
        pcm = blob[offset:offset + size]
        if crc32(pcm) != ccrc:
            raise ValueError(f"clip {cid}: CRC mismatch")
        clips[cid] = pcm
        end, last = offset + size, cid
    if end != total:
        raise ValueError("unexpected bytes after the last clip")
    referenced = {c for r in rounds for c in [r["prompt_clip"]] + r["option_clips"]}
    if not referenced <= set(clips):
        raise ValueError("a round refers to a missing clip")
    if any(c >= ROUND_CLIP_MIN and c not in referenced for c in clips):
        raise ValueError("a clip is not used by any round")
    return {"content_id": content_id, "revision": revision, "rounds": rounds, "clips": clips}


# ------------------------------------------------------------------ files

def _write_atomic(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as tmp:
        tmp.write(data)
    Path(tmp.name).replace(path)


def make_pack(source: Path, output: Path, *, reviewed: bool, placeholder: bool = False,
              checked_hash: str | None = None) -> dict:
    """Compile a reviewed game.json into output (.kpg) + .manifest.json."""
    if not reviewed:
        raise ValueError("pass --reviewed: an adult checked every round and recording")
    if output.suffix != ".kpg" or not PACK_ID_RE.match(output.stem):
        raise ValueError("output must be <pack_id>.kpg, pack_id like 'default'")
    game = load_source(source)
    blob = encode(game)
    parse(blob)  # never write what the firmware would refuse
    _write_atomic(output, blob)
    manifest = {
        # Own magic, so not a KPB2 package; the type name follows the value
        # KPB2 reserved for games (tools/kpb2.py PACKAGE_TYPES).
        "format": "KPG1", "version": VERSION, "package_type": "game",
        "game_id": game["game_id"], "content_id": game["content_id"],
        "revision": game["revision"], "title": game["title"],
        "reviewer": game["reviewer"], "reviewed": True, "placeholder": placeholder,
        "bytes": len(blob), "sha256": hashlib.sha256(blob).hexdigest(),
        "sd_path": f"/games/{output.stem}.kpg",
        "rounds": [{"kind": r["kind"], "prompt": r["prompt"],
                    "question_art": r["question_art"], "answer": r["answer"],
                    "options": [{"art": a, "label": lab, "clip": c} for a, lab, c in
                                zip(r["arts"], r["labels"], r["option_clips"])],
                    "prompt_clip": r["prompt_clip"]} for r in game["rounds"]],
        "clips": [{"id": cid, **game["clip_info"][cid],
                   "duration_ms": len(pcm) * 1000 // (2 * RATE),
                   "sha256": hashlib.sha256(pcm).hexdigest()}
                  for cid, pcm in sorted(game["clips"].items())],
        "session_lines_built_in": sorted(n for n, c in SESSION_CLIPS.items()
                                         if c not in game["clips"]),
    }
    if checked_hash is not None:
        manifest["checked_hash"] = checked_hash
    _write_atomic(output.with_suffix(".manifest.json"),
                  (json.dumps(manifest, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    return manifest


def tone(freqs: list[int], note_s: float = 0.12) -> bytes:
    pcm = array.array("h")
    for freq in freqs:
        n = int(RATE * note_s)
        for i in range(n):
            env = min(1.0, i / (0.008 * RATE), (n - i) / (0.040 * RATE))
            pcm.append(int(0.18 * 32767 * env * math.sin(2 * math.pi * freq * i / RATE)))
        pcm.extend([0] * int(RATE * 0.03))
    if sys.byteorder != "little":
        pcm.byteswap()
    return pcm.tobytes()


def write_wav(path: Path, pcm: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(pcm)


# Placeholder rounds for the TF smoke test (one per kind; tones, not words).
DEMO_ROUNDS = [
    ("animal_sound", "listen", ["cat", "dog", "bird"], "cat"),
    ("big_small", "listen", ["ball_big", "ball_small"], "ball_big"),
    ("count", "blocks3", ["dots2", "dots3", "dots5"], "dots3"),
    ("match_shape", "star", ["heart", "star", "triangle"], "star"),
    ("english_word", "listen", ["fish", "cat"], "fish"),
]


def demo_source(directory: Path, content_id: int = 900, note_s: float = 0.12) -> Path:
    """A reviewed-looking source made of synthetic tones (test/smoke only)."""
    rounds = []
    for n, (kind, question, arts, answer) in enumerate(DEMO_ROUNDS, 1):
        write_wav(directory / f"q{n}.wav", tone([523 + 40 * n, 784 + 40 * n], note_s))
        options = []
        for art in arts:
            name = f"{art}.wav"
            if not (directory / name).exists():
                write_wav(directory / name, tone([300 + 7 * ART[art]] * 2, note_s))
            options.append({"art": art, "label": art, "audio": name})
        rounds.append({"kind": kind, "prompt": f"smoke round {n}", "prompt_audio": f"q{n}.wav",
                       "question_art": question, "answer": answer, "approved": True,
                       "options": options})
    write_wav(directory / "right.wav", tone([523, 659, 784, 1047], note_s))
    source = directory / "game.json"
    source.write_text(json.dumps({
        "schema_version": 1, "package_type": "game", "game_id": "smoke_game",
        "content_id": content_id, "revision": 1, "title": "smoke test (tones only)",
        "reviewer": "ci", "approved": True, "session_audio": {"right": "right.wav"},
        "rounds": rounds}, ensure_ascii=False, indent=2), encoding="utf-8")
    return source


def demo(out_dir: Path) -> dict:
    """Placeholder pack (synthetic tones) to check the TF path on the board."""
    with tempfile.TemporaryDirectory() as tmp:
        manifest = make_pack(demo_source(Path(tmp)), out_dir / "games" / f"{DEFAULT_PACK_ID}.kpg",
                             reviewed=True, placeholder=True)
    (out_dir / "README-SMOKE-ONLY.txt").write_text(
        "Synthetic tones for checking the TF game pack path only. Not child content.\n"
        "Copy games/default.kpg to /games/default.kpg on a FAT32 TF card.\n",
        encoding="utf-8")
    return manifest


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("pack")
    c.add_argument("source", type=Path)
    c.add_argument("--output", type=Path, required=True)
    c.add_argument("--reviewed", action="store_true")
    c = sub.add_parser("verify")
    c.add_argument("pack", type=Path)
    c = sub.add_parser("demo")
    c.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        if args.cmd == "pack":
            m = make_pack(args.source, args.output, reviewed=args.reviewed)
            print(f"{args.output}: {len(m['rounds'])} rounds, {len(m['clips'])} clips, "
                  f"{m['bytes']} bytes")
        elif args.cmd == "demo":
            m = demo(args.output)
            print(f"{args.output / 'games/default.kpg'}: {len(m['rounds'])} rounds (tones only)")
        else:
            p = parse(args.pack.read_bytes())
            print(f"PACKAGE_VALID content_id={p['content_id']} revision={p['revision']} "
                  f"rounds={len(p['rounds'])} clips={len(p['clips'])}")
    except (OSError, ValueError) as exc:
        print(f"PACKAGE_INVALID {exc}" if args.cmd == "verify" else f"error: {exc}",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
