"""KPB2 picture-book packages: grey pages, narration and dial choice questions.

Byte layout and rules: docs/V4_KPB2_CONTENT_FORMAT.md (the C++ reader in
firmware/components/kid_app/src/content_pack_reader.cc enforces the same).
Compiling is a trusted offline step after an adult reviewed every page,
question, option icon and recording; it never publishes anything.

Images: 800x480 page and option icons as PNG (needs Pillow) or binary PGM
(P5, maxval 255; no extra dependency). Audio: 24 kHz mono PCM16 WAV.
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import struct
import wave
import zlib

WIDTH, HEIGHT = 800, 480
MONO_BYTES = WIDTH * HEIGHT // 8      # 48,000
GREY_BYTES = WIDTH * HEIGHT // 4      # 96,000
GREY_ROW_BYTES = WIDTH // 4           # 200
SAMPLE_RATE = 24000
MAX_PAGES = 32
MAX_QUESTIONS_PER_PAGE = 2
MAX_QUESTIONS = 64
MAX_PAGE_PCM = SAMPLE_RATE * 2 * 60
MAX_PROMPT_PCM = SAMPLE_RATE * 2 * 15
MAX_OPTION_PCM = SAMPLE_RATE * 2 * 5
MAX_TITLE_BYTES, MAX_PAGE_TEXT_BYTES = 320, 1280
MAX_PROMPT_TEXT_BYTES, MAX_LABEL_BYTES = 400, 96
ICON_MIN, ICON_MAX = 48, 240
HIGHLIGHT_MARGIN = 8
NON_LIKE_OPTION_BASE = 0xFF00
MAX_PACKAGE_BYTES = 64 * 1024 * 1024
MONO_THRESHOLD = 170  # same luminance threshold as KPB1

MAGIC = b"KPB2"
VERSION = 2          # linear books: byte-identical to what shipped in V4
VERSION_BRANCH = 3   # may branch (header flag) and/or belong to a series
HEADER_FLAG_BRANCHING = 0x0001
# v3 page links (page record bytes 64..66): 0 = the next page (END after the
# last), NEXT_END = an ending, NEXT_RESTART = back to page 1, else a page.
NEXT_END, NEXT_RESTART = 0xFFFF, 0x8001
GOTO_RESTART = 0x81  # option jump byte: 1..32 = page, 0x81 = back to page 1
MAX_SERIES = 16
# package_type values. Only BOOK is compiled and read today; the rest are
# reserved so other TF-card packs can never be opened as a book.
PACKAGE_TYPES = {"book": 1, "soundpack": 2, "game": 3, "costume": 4, "phrases": 5}
PAGE_FLAG_GREY = 0x0001
QUESTION_FLAG_VOICE = 0x01
OPTION_FLAG_LIKE = 0x01

HEADER = struct.Struct("<4s10H10I28sI")      # 96 B
PAGE = struct.Struct("<4H3I3I3IIHH3I32s")    # 96 B
QUESTION = struct.Struct("<HBBBBH3I3I")      # 32 B, then 3 options + 16 B
OPTION = struct.Struct("<HBB4H3I3I3I")       # 48 B
QUESTION_BYTES = QUESTION.size + 3 * OPTION.size + 16  # 192 B

# Preference topics a like question may write (ADR-012, V5-03). Slot and
# option ids are published values: never renumber or reuse one; append new
# ones. Device memory stores only these numbers.
PREFERENCE_TOPICS: dict[str, tuple[int, str, dict[str, tuple[int, str]]]] = {
    "favorite_animal": (1, "最喜欢的动物", {
        "cat": (1, "小猫"), "dog": (2, "小狗"), "rabbit": (3, "小兔子"),
        "bird": (4, "小鸟"), "fish": (5, "小鱼"), "elephant": (6, "大象"),
        "panda": (7, "熊猫"), "bear": (8, "小熊"), "duck": (9, "小鸭子"),
        "turtle": (10, "小乌龟"),
    }),
    "favorite_color": (2, "最喜欢的颜色", {
        "red": (1, "红色"), "yellow": (2, "黄色"), "blue": (3, "蓝色"),
        "green": (4, "绿色"), "pink": (5, "粉色"), "purple": (6, "紫色"),
        "orange": (7, "橙色"), "white": (8, "白色"), "black": (9, "黑色"),
    }),
    "favorite_food": (3, "最喜欢的食物", {
        "carrot": (1, "胡萝卜"), "apple": (2, "苹果"), "banana": (3, "香蕉"),
        "strawberry": (4, "草莓"), "rice": (5, "米饭"), "noodles": (6, "面条"),
        "egg": (7, "鸡蛋"), "milk": (8, "牛奶"),
    }),
    "favorite_toy": (4, "最喜欢的玩具", {
        "ball": (1, "小球"), "blocks": (2, "积木"), "doll": (3, "娃娃"),
        "car": (4, "小汽车"), "kite": (5, "风筝"), "drum": (6, "小鼓"),
    }),
}

# Highest registered option id per like slot; the C++ reader keeps the same
# table (kLikeOptionMax in content_pack_reader.cc) and rejects anything else.
LIKE_OPTION_MAX = {slot: max(i for i, _ in options.values())
                   for slot, _, options in PREFERENCE_TOPICS.values()}

BOOK_RE = re.compile(r"^[a-z0-9][a-z0-9_-]{2,63}$")
KEY_RE = re.compile(r"^[a-z][a-z0-9_]{0,23}$")


def crc32(data: bytes) -> int:
    return zlib.crc32(data) & 0xFFFFFFFF


# ---------------------------------------------------------------- inputs

def reviewed_text(value: object, max_chars: int, what: str) -> str:
    if not isinstance(value, str) or not value.strip() or len(value) > max_chars:
        raise ValueError(f"Reviewed {what} is missing or too long")
    if any(ord(ch) < 32 for ch in value) or "‮" in value:
        raise ValueError(f"Controls and bidirectional override forbidden in {what}")
    return value.strip()


def asset_path(root: Path, path: object, suffixes: tuple[str, ...], what: str) -> Path:
    if not isinstance(path, str) or not path or "\\" in path:
        raise ValueError(f"Invalid {what} path")
    relative = Path(path)
    if relative.is_absolute() or any(part in (".", "..") for part in relative.parts):
        raise ValueError(f"{what} must be a relative in-project path")
    full = (root / relative).resolve()
    if not full.is_relative_to(root.resolve()) or not full.is_file():
        raise ValueError(f"{what} asset missing or outside book directory")
    if full.suffix.lower() not in suffixes:
        raise ValueError(f"{what} must be one of {', '.join(suffixes)}")
    return full


def read_pgm(path: Path) -> tuple[int, int, bytes]:
    """Binary PGM (P5, maxval 255) -> (width, height, luminance bytes)."""
    data = path.read_bytes()
    fields: list[bytes] = []
    pos = 0
    while len(fields) < 4:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while pos < len(data) and data[pos:pos + 1] not in (b"\n", b"\r"):
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        if start == pos:
            raise ValueError("Truncated PGM header")
        fields.append(data[start:pos])
    if fields[0] != b"P5" or not all(f.isdigit() for f in fields[1:]):
        raise ValueError("Only binary PGM (P5) images are supported")
    width, height, maxval = (int(f) for f in fields[1:])
    if maxval != 255 or width <= 0 or height <= 0:
        raise ValueError("PGM must be 8-bit (maxval 255)")
    pixels = data[pos + 1:]
    if len(pixels) != width * height:
        raise ValueError("PGM pixel data has the wrong length")
    return width, height, bytes(pixels)


def luminance(path: Path) -> tuple[int, int, bytes]:
    """Image -> 8-bit luminance; transparent PNG pixels count as white."""
    if path.suffix.lower() == ".pgm":
        return read_pgm(path)
    from PIL import Image

    with Image.open(path) as image:
        image.load()
        rgba = image.convert("RGBA")
        canvas = Image.new("RGBA", rgba.size, (255, 255, 255, 255))
        canvas.alpha_composite(rgba)
        grey = canvas.convert("L")
        return grey.width, grey.height, grey.tobytes()


def write_pgm(path: Path, width: int, height: int, pixels: bytes) -> None:
    path.write_bytes(b"P5\n%d %d\n255\n" % (width, height) + bytes(pixels))


def wav_pcm(path: Path, max_bytes: int, what: str) -> bytes:
    with wave.open(str(path), "rb") as wf:
        if wf.getcomptype() != "NONE" or wf.getnchannels() != 1 or \
                wf.getsampwidth() != 2 or wf.getframerate() != SAMPLE_RATE:
            raise ValueError(f"{what} must be uncompressed 24 kHz mono PCM16 WAV")
        frames = wf.getnframes()
        if not 0 < frames * 2 <= max_bytes:
            raise ValueError(f"{what} length must be >0 and <={max_bytes // 48000} seconds")
        data = wf.readframes(frames)
    if len(data) != frames * 2:
        raise ValueError(f"Truncated {what} WAV")
    return data


# ----------------------------------------------------------- conversions

_LEVEL = bytes((v * 3 + 127) // 255 for v in range(256))  # 0 black .. 3 white
# 2x2 ordered dither of a grey level into the 1-bit fallback: level L is
# white where L > cell (L1 -> 25 % white, L2 -> 75 %, L0/L3 solid).
_DITHER = ((0, 1), (1, 2))


def grey_levels(lum: bytes) -> bytes:
    return lum.translate(_LEVEL)


def pack_grey(levels: bytes) -> bytes:
    a, b, c, d = levels[0::4], levels[1::4], levels[2::4], levels[3::4]
    return bytes((w << 6) | (x << 4) | (y << 2) | z for w, x, y, z in zip(a, b, c, d))


def unpack_grey(grey: bytes) -> bytes:
    out = bytearray(len(grey) * 4)
    for shift, start in ((6, 0), (4, 1), (2, 2), (0, 3)):
        out[start::4] = bytes((v >> shift) & 3 for v in grey)
    return bytes(out)


def pack_bits(white: bytes) -> bytes:
    """One byte per pixel (0/1, 1 = white) -> MSB-first bit rows."""
    out = bytearray(len(white) // 8)
    for bit in range(8):
        weight = 0x80 >> bit
        for i, v in enumerate(white[bit::8]):
            if v:
                out[i] |= weight
    return bytes(out)


def unpack_bits(packed: bytes) -> bytes:
    out = bytearray(len(packed) * 8)
    for bit in range(8):
        out[bit::8] = bytes((v >> (7 - bit)) & 1 for v in packed)
    return bytes(out)


def mono_threshold(lum: bytes) -> bytes:
    return pack_bits(bytes(v >= MONO_THRESHOLD for v in lum))


def mono_dither(levels: bytes, width: int) -> bytes:
    white = bytearray(len(levels))
    for y in range(len(levels) // width):
        row = _DITHER[y & 1]
        base = y * width
        for x in range(width):
            white[base + x] = levels[base + x] > row[x & 1]
    return pack_bits(bytes(white))


def page_frames(path: Path, mode: str) -> tuple[bytes, bytes | None]:
    width, height, lum = luminance(path)
    if (width, height) != (WIDTH, HEIGHT):
        raise ValueError("Book page must be exactly 800 x 480")
    if mode == "mono":
        return mono_threshold(lum), None
    levels = grey_levels(lum)
    return mono_dither(levels, WIDTH), pack_grey(levels)


def icon_bits(path: Path) -> tuple[int, int, bytes]:
    width, height, lum = luminance(path)
    if width % 8 or not ICON_MIN <= width <= ICON_MAX or not ICON_MIN <= height <= ICON_MAX:
        raise ValueError("Option icon must be 48..240 px, width a multiple of 8")
    return width, height, mono_threshold(lum)


# --------------------------------------------------------- story graph

def page_edges(number: int, count: int, nxt: int,
               branch: list[int] | None) -> tuple[set[int], bool, bool, bool]:
    """Edges of one page: (successor pages, is an ending, has a restart edge,
    carries an explicit link). `branch` = the jump bytes of its branch
    question (every option jumps), None = a plain page that uses `nxt`."""
    succ: set[int] = set()
    if branch is not None:
        if nxt:
            raise ValueError(f"Page {number}: a branch page has no next link")
        restart = False
        for k, go in enumerate(branch):
            if go == GOTO_RESTART:
                if number == 1 or k == 0:
                    # the first option is the bedtime road: it must lead on
                    raise ValueError("Page 1 cannot restart onto itself, and the first "
                                     "option of a branch may not restart")
                restart = True
            elif 1 <= go <= count and go != number:
                succ.add(go)
            else:
                raise ValueError(f"Page {number}: branch target {go} does not exist")
        return succ, False, restart, True
    if nxt == 0:
        return ({number + 1}, False, False, False) if number < count else (set(), True, False, False)
    if nxt == NEXT_END:
        return set(), True, False, True
    if nxt == NEXT_RESTART:
        if number == 1:
            raise ValueError("Page 1 cannot restart onto itself")
        return set(), False, True, True
    if 1 <= nxt <= count and nxt != number:
        return {nxt}, False, False, True
    raise ValueError(f"Page {number}: next page {nxt} does not exist")


def check_story_graph(count: int, links: list[tuple[int, list[int] | None]]) -> bool:
    """links[i] = (next, branch jumps or None) of page i+1. Rejects missing
    targets, unreachable pages, loops (a restart edge to page 1 is the only
    allowed way back) and pages that cannot reach an ending. Returns whether
    any page carries an explicit link."""
    succ, ends, explicit = {}, {}, False
    for number, (nxt, branch) in enumerate(links, 1):
        s_, e_, _, x_ = page_edges(number, count, nxt, branch)
        succ[number], ends[number] = s_, e_
        explicit = explicit or x_
    reach, stack = {1}, [1]
    while stack:
        for n in succ[stack.pop()]:
            if n not in reach:
                reach.add(n)
                stack.append(n)
    if len(reach) != count:
        raise ValueError("Some pages can never be reached from page 1")
    removed: set[int] = set()
    changed = True
    while changed:  # a loop never peels away
        changed = False
        for n in succ:
            if n not in removed and succ[n] <= removed:
                removed.add(n)
                changed = True
    if len(removed) != count:
        raise ValueError("The story contains a loop (only a restart to page 1 may go back)")
    can_end: set[int] = set()
    changed = True
    while changed:
        changed = False
        for n in succ:
            if n not in can_end and (ends[n] or succ[n] & can_end):
                can_end.add(n)
                changed = True
    if len(can_end) != count:
        raise ValueError("Some page cannot reach an ending")
    return explicit


def check_series(content_id: int, series: tuple[int, int, int, int]) -> None:
    sid, index, count, nxt = series
    if (sid, index, count, nxt) == (0, 0, 0, 0):
        return
    if not (sid and 1 <= count <= MAX_SERIES and 1 <= index <= count):
        raise ValueError("Invalid series id/index/count")
    if (index < count and (not nxt or nxt == content_id)) or (index == count and nxt):
        raise ValueError("Series next_content_id must name the following book "
                         "(and be 0 for the last one)")


# ------------------------------------------------------------ the source

def _exact_keys(item: object, required: set[str], optional: set[str], what: str) -> dict:
    if not isinstance(item, dict) or not required <= set(item) or \
            not set(item) <= required | optional:
        raise ValueError(f"{what} has missing or unknown fields")
    return item


def _bool(item: dict, key: str, what: str) -> bool:
    value = item.get(key, False)
    if type(value) is not bool:
        raise ValueError(f"{what}.{key} must be true or false")
    return value


def _int(value: object, low: int, high: int, what: str) -> int:
    if type(value) is not int or not low <= value <= high:
        raise ValueError(f"{what} must be an integer in {low}..{high}")
    return value


def _rect(x: int, y: int, w: int, h: int) -> tuple[int, int, int, int]:
    m = HIGHLIGHT_MARGIN
    return x - m, y - m, w + 2 * m, h + 2 * m


def _overlap(a: tuple[int, int, int, int], b: tuple[int, int, int, int]) -> bool:
    return a[0] < b[0] + b[2] and b[0] < a[0] + a[2] and \
        a[1] < b[1] + b[3] and b[1] < a[1] + a[3]


def _check_box(x: int, y: int, w: int, h: int) -> None:
    m = HIGHLIGHT_MARGIN
    if x % 8 or x < m or y < m or x + w + m > WIDTH or y + h + m > HEIGHT:
        raise ValueError("Option icon must sit inside the page with an 8 px "
                         "highlight margin; x must be a multiple of 8")


def _question(root: Path, item: object, where: str) -> dict:
    q = _exact_keys(item, {"prompt", "prompt_audio", "approved", "options"},
                    {"answer", "like_topic", "voice_answer"}, where)
    if q["approved"] is not True:
        raise ValueError(f"{where} requires explicit review")
    prompt = reviewed_text(q["prompt"], 100, "question prompt")
    prompt_pcm = wav_pcm(asset_path(root, q["prompt_audio"], (".wav",), "Prompt audio"),
                         MAX_PROMPT_PCM, "Prompt audio")
    options = q["options"]
    if not isinstance(options, list) or not 2 <= len(options) <= 3:
        raise ValueError(f"{where} needs 2 or 3 options")
    topic_key = q.get("like_topic")
    answer = q.get("answer")
    if topic_key is not None and answer is not None:
        raise ValueError(f"{where}: a like question has no right answer")
    if topic_key is not None and (not isinstance(topic_key, str) or
                                  topic_key not in PREFERENCE_TOPICS):
        raise ValueError(f"{where}: unknown like_topic {topic_key!r}")
    slot, _, registry = PREFERENCE_TOPICS[topic_key] if topic_key else (0, "", {})
    parsed = []
    keys: set[str] = set()
    for n, raw in enumerate(options, 1):
        o = _exact_keys(raw, {"key", "label", "icon", "audio", "x", "y"}, {"like", "goto"},
                        f"{where} option {n}")
        key = o["key"]
        if not isinstance(key, str) or not KEY_RE.fullmatch(key) or key in keys:
            raise ValueError(f"{where}: option keys must be unique a-z/0-9/_ names")
        keys.add(key)
        like = _bool(o, "like", "option")
        if like and not topic_key:
            raise ValueError(f"{where}: like options need the question's like_topic")
        if like and key not in registry:
            raise ValueError(f"{where}: {key!r} is not a registered {topic_key} option")
        option_id = registry[key][0] if like else NON_LIKE_OPTION_BASE + n
        width, height, icon = icon_bits(asset_path(root, o["icon"], (".png", ".pgm"), "Icon"))
        x = _int(o["x"], 0, WIDTH, "Option x")
        y = _int(o["y"], 0, HEIGHT, "Option y")
        _check_box(x, y, width, height)
        box = _rect(x, y, width, height)
        if any(_overlap(box, _rect(*p["box"])) for p in parsed):
            raise ValueError(f"{where}: option icons (with margin) overlap")
        go = o.get("goto", 0)
        if go == "restart":
            go = GOTO_RESTART
        elif go != 0:
            go = _int(go, 1, MAX_PAGES, f"{where} option {n} goto")
        parsed.append({
            "key": key, "id": option_id, "like": like, "goto": go,
            "label": reviewed_text(o["label"], 24, "option label"),
            "box": (x, y, width, height), "icon": icon,
            "pcm": wav_pcm(asset_path(root, o["audio"], (".wav",), "Option audio"),
                           MAX_OPTION_PCM, "Option audio"),
        })
    if topic_key and not any(p["like"] for p in parsed):
        raise ValueError(f"{where}: a like question needs at least one like option")
    jumps = sum(1 for p in parsed if p["goto"])
    if jumps and (jumps != len(parsed) or answer is not None):
        raise ValueError(f"{where}: a branch question needs a goto on every option "
                         "and no right answer")
    correct = 0
    if answer is not None:
        if not isinstance(answer, str) or answer not in keys:
            raise ValueError(f"{where}: answer must be one of the option keys")
        correct = [p["key"] for p in parsed].index(answer) + 1
    return {"prompt": prompt, "prompt_pcm": prompt_pcm, "options": parsed,
            "branch": bool(jumps), "correct": correct, "like_slot": slot, "like_topic": topic_key,
            "voice": _bool(q, "voice_answer", "question")}


def load_source(source: Path) -> dict:
    """Strictly validate a reviewed schema v2 book folder; return assets."""
    doc = json.loads(source.read_text(encoding="utf-8"))
    doc = _exact_keys(doc, {"schema_version", "package_type", "book_id", "content_id",
                            "revision", "title", "reviewer", "approved", "pages"},
                      {"credits", "checked_hash", "series"}, "Book manifest")
    if doc["schema_version"] != 2 or doc["approved"] is not True:
        raise ValueError("Book manifest must be approved schema v2")
    if doc["package_type"] != "book":
        raise ValueError("Only package_type 'book' can be compiled; other types are reserved")
    book_id = doc["book_id"]
    if not isinstance(book_id, str) or not BOOK_RE.fullmatch(book_id):
        raise ValueError("Invalid book ID")
    pages = doc["pages"]
    if not isinstance(pages, list) or not 1 <= len(pages) <= MAX_PAGES:
        raise ValueError("Package has invalid page count")
    root = source.parent
    parsed = []
    total_questions = 0
    for i, raw in enumerate(pages, 1):
        page = _exact_keys(raw, {"number", "text", "approved", "image", "audio"},
                           {"image_mode", "questions", "next"}, f"Page {i}")
        if type(page["number"]) is not int or page["number"] != i or \
                page["approved"] is not True:
            raise ValueError("Every numbered page requires explicit review")
        mode = page.get("image_mode", "grey")
        if mode not in ("grey", "mono"):
            raise ValueError("image_mode must be 'grey' or 'mono'")
        mono, grey = page_frames(asset_path(root, page["image"], (".png", ".pgm"), "Page image"),
                                 mode)
        questions = page.get("questions", [])
        if not isinstance(questions, list) or len(questions) > MAX_QUESTIONS_PER_PAGE:
            raise ValueError(f"Page {i} may hold at most 2 questions")
        total_questions += len(questions)
        nxt = page.get("next", 0)
        if nxt == "end":
            nxt = NEXT_END
        elif nxt == "restart":
            nxt = NEXT_RESTART
        elif nxt != 0:
            nxt = _int(nxt, 1, MAX_PAGES, f"Page {i} next")
        parsed.append({
            "next": nxt,
            "text": reviewed_text(page["text"], 320, "page text"),
            "mono": mono, "grey": grey,
            "pcm": wav_pcm(asset_path(root, page["audio"], (".wav",), "Page audio"),
                           MAX_PAGE_PCM, "Page audio"),
            "questions": [_question(root, q, f"Page {i} question {j}")
                          for j, q in enumerate(questions, 1)],
        })
    if total_questions > MAX_QUESTIONS:
        raise ValueError("Too many questions in one package")
    links = []
    for number, page in enumerate(parsed, 1):
        qs = page["questions"]
        branch_qs = [q for q in qs if q["branch"]]
        if branch_qs and (len(branch_qs) != 1 or not qs[-1]["branch"]):
            raise ValueError(f"Page {number}: only the last question may branch")
        links.append((page["next"],
                      [o["goto"] for o in branch_qs[0]["options"]] if branch_qs else None))
    branching = check_story_graph(len(parsed), links)
    series = (0, 0, 0, 0)
    if doc.get("series") is not None:
        raw = _exact_keys(doc["series"], {"id", "index", "count", "next_content_id"}, set(),
                          "series")
        series = (_int(raw["id"], 1, 0xFFFF, "series id"),
                  _int(raw["index"], 1, MAX_SERIES, "series index"),
                  _int(raw["count"], 1, MAX_SERIES, "series count"),
                  _int(raw["next_content_id"], 0, 0xFFFF, "series next_content_id"))
        check_series(_int(doc["content_id"], 1, 0xFFFF, "content_id"), series)
    credits = doc.get("credits")
    if credits is not None and (not isinstance(credits, dict) or
                                len(json.dumps(credits, ensure_ascii=False)) > 4096):
        raise ValueError("credits must be a small JSON object")
    checked = doc.get("checked_hash")
    if checked is not None and (not isinstance(checked, str) or
                                not re.fullmatch(r"[0-9a-f]{64}", checked)):
        raise ValueError("checked_hash must be a SHA-256 hex digest")
    return {
        "credits": credits, "checked_hash": checked,
        "book_id": book_id,
        "content_id": _int(doc["content_id"], 1, 0xFFFF, "content_id"),
        "revision": _int(doc["revision"], 1, 0xFFFF, "revision"),
        "title": reviewed_text(doc["title"], 80, "title"),
        "reviewer": reviewed_text(doc["reviewer"], 40, "reviewer"),
        "pages": parsed,
        "branching": branching,
        "series": series,
    }


# ------------------------------------------------------------- compiling

class _Data:
    """Canonical data region: 4-byte aligned, zero padded, in order."""

    def __init__(self, start: int) -> None:
        self.start = start
        self.blob = bytearray()

    def add(self, data: bytes) -> tuple[int, int, int]:
        while len(self.blob) % 4:
            self.blob.append(0)
        offset = self.start + len(self.blob)
        self.blob.extend(data)
        if self.start + len(self.blob) > MAX_PACKAGE_BYTES:
            raise ValueError("Package too large for supported SD resources")
        return offset, len(data), crc32(data)


def build(book: dict) -> bytes:
    pages = book["pages"]
    questions = [q for p in pages for q in p["questions"]]
    page_table = HEADER.size
    question_table = page_table + len(pages) * PAGE.size
    data_start = question_table + len(questions) * QUESTION_BYTES
    data = _Data(data_start)
    title = data.add(book["title"].encode("utf-8"))
    page_records = []
    question_records = []
    for number, page in enumerate(pages, 1):
        mono = data.add(page["mono"])
        grey = data.add(page["grey"]) if page["grey"] is not None else (0, 0, 0)
        pcm = data.add(page["pcm"])
        text = data.add(page["text"].encode("utf-8"))
        page_records.append(PAGE.pack(
            number, PAGE_FLAG_GREY if page["grey"] is not None else 0,
            len(question_records), len(page["questions"]),
            *mono, *grey, *pcm, SAMPLE_RATE, 1, 16, *text,
            struct.pack("<H", page["next"]).ljust(32, b"\0")))
        for index, q in enumerate(page["questions"]):
            prompt_pcm = data.add(q["prompt_pcm"])
            prompt_text = data.add(q["prompt"].encode("utf-8"))
            options = b""
            for o in q["options"]:
                icon = data.add(o["icon"])
                pcm = data.add(o["pcm"])
                label = data.add(o["label"].encode("utf-8"))
                options += OPTION.pack(o["id"], OPTION_FLAG_LIKE if o["like"] else 0, o["goto"],
                                       *o["box"], *icon, *pcm, *label)
            options = options.ljust(3 * OPTION.size + 16, b"\0")
            question_records.append(QUESTION.pack(
                number, index, len(q["options"]), q["correct"],
                QUESTION_FLAG_VOICE if q["voice"] else 0, q["like_slot"],
                *prompt_pcm, *prompt_text) + options)
    tables = b"".join(page_records) + b"".join(question_records)
    total = data_start + len(data.blob)
    series = book["series"]
    v3 = book["branching"] or series != (0, 0, 0, 0)
    head = HEADER.pack(MAGIC, VERSION_BRANCH if v3 else VERSION, HEADER.size,
                       PACKAGE_TYPES["book"],
                       HEADER_FLAG_BRANCHING if book["branching"] else 0,
                       WIDTH, HEIGHT, book["content_id"], book["revision"],
                       len(pages), len(questions),
                       page_table, len(page_records) * PAGE.size,
                       question_table, len(question_records) * QUESTION_BYTES,
                       data_start, total, *title, crc32(tables),
                       struct.pack("<4H", *series).ljust(28, b"\0"), 0)
    head = head[:92] + struct.pack("<I", crc32(head[:92]))
    blob = head + tables + bytes(data.blob)
    if len(blob) != total:
        raise AssertionError("Internal layout mismatch")
    return blob


def sidecar(book: dict, blob: bytes, parsed: dict) -> dict:
    """Parent-readable record of exactly what went into the package."""
    pages = []
    for page, info in zip(book["pages"], parsed["pages"]):
        pages.append({
            "page": info["number"], "text": page["text"], "next": info["next"],
            "image_mode": "grey" if page["grey"] is not None else "mono",
            "mono_sha256": hashlib.sha256(page["mono"]).hexdigest(),
            "grey_sha256": hashlib.sha256(page["grey"]).hexdigest() if page["grey"] else None,
            "pcm_sha256": hashlib.sha256(page["pcm"]).hexdigest(),
            "duration_ms": len(page["pcm"]) * 1000 // 48000,
            "questions": [{
                "prompt": q["prompt"],
                "answer": q["options"][q["correct"] - 1]["key"] if q["correct"] else None,
                "like_topic": q["like_topic"], "like_slot": q["like_slot"],
                "voice_answer": q["voice"], "branch": q["branch"],
                "options": [{"key": o["key"], "label": o["label"], "id": o["id"],
                             "writes_like": o["like"], "box": list(o["box"]),
                             "goto": o["goto"]}
                            for o in q["options"]],
            } for q in page["questions"]],
        })
    result = {"schema_version": 2, "format": "KPB2", "package_type": "book",
              "book_id": book["book_id"], "content_id": book["content_id"],
              "revision": book["revision"], "title": book["title"],
              "reviewer": book["reviewer"], "approved": True,
              "width": WIDTH, "height": HEIGHT, "pages": pages,
              "question_count": parsed["question_count"],
              "branching": book["branching"],
              "series": ({"id": book["series"][0], "index": book["series"][1],
                          "count": book["series"][2], "next_content_id": book["series"][3]}
                         if book["series"][0] else None),
              "package_bytes": len(blob), "sha256": hashlib.sha256(blob).hexdigest(),
              "integrity": "CRC32 per table and payload; no cryptographic authentication"}
    if book.get("credits") is not None:
        result["credits"] = book["credits"]
    if book.get("checked_hash") is not None:
        # Hash of the staged content a human attested; catalog refuses without it.
        result["checked_hash"] = book["checked_hash"]
    return result


def compile_book(source: Path) -> tuple[bytes, dict, list[Path]]:
    """Return (package bytes, sidecar manifest, source asset paths)."""
    source = source.resolve()
    book = load_source(source)
    blob = build(book)
    parsed = parse(blob)  # never emit what the strict reader would reject
    doc = json.loads(source.read_text(encoding="utf-8"))
    root = source.parent
    assets = [source]
    for page in doc["pages"]:
        assets += [(root / page["image"]).resolve(), (root / page["audio"]).resolve()]
        for q in page.get("questions", []):
            assets.append((root / q["prompt_audio"]).resolve())
            for o in q["options"]:
                assets += [(root / o["icon"]).resolve(), (root / o["audio"]).resolve()]
    return blob, sidecar(book, blob, parsed), assets


# --------------------------------------------------------------- reading

def _seg(fields: tuple, at: int) -> tuple[int, int, int]:
    return fields[at], fields[at + 1], fields[at + 2]


class _Chain:
    def __init__(self, blob: bytes, start: int) -> None:
        self.blob = blob
        self.end = start

    def take(self, seg: tuple[int, int, int], what: str) -> bytes:
        offset, size, crc = seg
        aligned = (self.end + 3) & ~3
        if offset != aligned or offset + size > len(self.blob):
            raise ValueError(f"{what} is not at its canonical offset")
        if any(self.blob[self.end:aligned]):
            raise ValueError("Non-zero padding between segments")
        data = self.blob[offset:offset + size]
        if crc32(data) != crc:
            raise ValueError(f"{what} CRC mismatch")
        self.end = offset + size
        return data


def _pcm_ok(seg: tuple[int, int, int], limit: int) -> bool:
    return 2 <= seg[1] <= limit and seg[1] % 2 == 0


def _text_ok(seg: tuple[int, int, int], limit: int) -> bool:
    return 1 <= seg[1] <= limit


def _parse_question(raw: bytes, page: int, index: int, branching: bool = False) -> dict:
    f = QUESTION.unpack_from(raw)
    q_page, q_index, count, correct, flags, slot = f[:6]
    prompt_pcm, prompt_text = _seg(f, 6), _seg(f, 9)
    if q_page != page or q_index != index or not 2 <= count <= 3 or correct > count or \
            flags & ~QUESTION_FLAG_VOICE or not _pcm_ok(prompt_pcm, MAX_PROMPT_PCM) or \
            not _text_ok(prompt_text, MAX_PROMPT_TEXT_BYTES) or any(raw[176:192]):
        raise ValueError(f"Invalid question record on page {page}")
    options = []
    for k in range(3):
        at = QUESTION.size + k * OPTION.size
        if k >= count:
            if any(raw[at:at + OPTION.size]):
                raise ValueError("Unused option slot must be zero")
            continue
        o = OPTION.unpack_from(raw, at)
        option_id, oflags, go, x, y, w, h = o[:7]
        icon, pcm, label = _seg(o, 7), _seg(o, 10), _seg(o, 13)
        like = bool(oflags & OPTION_FLAG_LIKE)
        if oflags & ~OPTION_FLAG_LIKE or (go and not branching):
            raise ValueError("Unknown option flags")
        if go and go != GOTO_RESTART and not 1 <= go <= MAX_PAGES:
            raise ValueError("Invalid option jump")
        if like and not 0 < option_id <= LIKE_OPTION_MAX.get(slot, 0):
            raise ValueError("Like option needs a registered slot and option id")
        if not like and option_id != NON_LIKE_OPTION_BASE + k + 1:
            raise ValueError("Non-like option id must be 0xFF00 + position")
        if w % 8 or not ICON_MIN <= w <= ICON_MAX or not ICON_MIN <= h <= ICON_MAX:
            raise ValueError("Invalid option icon size")
        _check_box(x, y, w, h)
        if icon[1] != w * h // 8 or not _pcm_ok(pcm, MAX_OPTION_PCM) or \
                not _text_ok(label, MAX_LABEL_BYTES):
            raise ValueError("Invalid option asset sizes")
        box = _rect(x, y, w, h)
        for other in options:
            if _overlap(box, _rect(*other["box"])) or \
                    (like and other["like"] and other["id"] == option_id):
                raise ValueError("Options overlap or repeat an id")
        options.append({"id": option_id, "like": like, "goto": go, "box": (x, y, w, h),
                        "icon": icon, "pcm": pcm, "label": label})
    if slot and (correct or not any(o["like"] for o in options)):
        raise ValueError("Like question must have no answer and a like option")
    jumps = sum(1 for o in options if o["goto"])
    if jumps and (jumps != len(options) or correct):
        raise ValueError("A branch question needs a jump on every option and no right answer")
    return {"branch": bool(jumps), "page": page, "index": index, "correct": correct, "like_slot": slot,
            "voice": bool(flags & QUESTION_FLAG_VOICE), "prompt_pcm": prompt_pcm,
            "prompt_text": prompt_text, "options": options}


def _parse_kpb1(blob: bytes) -> dict:
    from tools.story_pack import HEADER as H1, RECORD as R1

    if len(blob) < H1.size:
        raise ValueError("Truncated KPB1 header")
    magic, version, width, height, count, toc, toc_bytes, first, total, crc = \
        H1.unpack_from(blob)
    if version != 1 or (width, height) != (WIDTH, HEIGHT) or crc32(blob[:28]) != crc or \
            not 1 <= count <= MAX_PAGES or toc != H1.size or toc_bytes != count * R1.size or \
            first != toc + toc_bytes or total != len(blob):
        raise ValueError("Invalid KPB1 header")
    end = first
    pages = []
    for i in range(count):
        r = R1.unpack_from(blob, toc + i * R1.size)
        number, reserved = r[0], r[1]
        segs = [_seg(r, 2), _seg(r, 5), _seg(r, 11)]
        rate, channels, bits = r[8:11]
        if number != i + 1 or reserved or any(r[14]) or segs[0][1] != MONO_BYTES or \
                (rate, channels, bits) != (SAMPLE_RATE, 1, 16) or \
                not _pcm_ok(segs[1], MAX_PAGE_PCM) or not _text_ok(segs[2], MAX_PAGE_TEXT_BYTES):
            raise ValueError(f"Invalid KPB1 page {i + 1}")
        for offset, size, seg_crc in segs:
            if offset % 4 or not end <= offset <= end + 3 or offset + size > total or \
                    crc32(blob[offset:offset + size]) != seg_crc:
                raise ValueError(f"Invalid KPB1 segment on page {i + 1}")
            end = offset + size
        pages.append({"number": number, "grey": False, "questions": [],
                      "text": blob[segs[2][0]:segs[2][0] + segs[2][1]]
                      .decode("utf-8", errors="replace")})
    if end != total:
        raise ValueError("Trailing bytes after KPB1 data")
    return {"format": "KPB1", "package_type": "book", "content_id": 0, "revision": 0,
            "title": None, "pages": pages, "question_count": 0}


def parse(blob: bytes) -> dict:
    """Strictly validate a KPB1 or KPB2 package (same rules as the device)."""
    if len(blob) > MAX_PACKAGE_BYTES:
        raise ValueError("Package too large")
    if blob[:4] == b"KPB1":
        return _parse_kpb1(blob)
    if blob[:4] != MAGIC or len(blob) < HEADER.size:
        raise ValueError("Not a KPB1/KPB2 package")
    h = HEADER.unpack_from(blob)
    (_, version, header_bytes, ptype, flags, width, height, content_id, revision,
     count, questions, page_table, page_bytes, question_table, question_bytes,
     data_start, total) = h[:17]
    title, tables_crc, reserved, head_crc = _seg(h, 17), h[20], h[21], h[22]
    v3 = version == VERSION_BRANCH
    series = struct.unpack_from("<4H", reserved) if v3 else (0, 0, 0, 0)
    reserved = reserved[8:] if v3 else reserved
    branching = v3 and bool(flags & HEADER_FLAG_BRANCHING)
    if ptype != PACKAGE_TYPES["book"]:
        raise ValueError("Package type is reserved or unknown; only books are read")
    if version not in (VERSION, VERSION_BRANCH) or header_bytes != HEADER.size or \
            flags & ~(HEADER_FLAG_BRANCHING if v3 else 0) or \
            (width, height) != (WIDTH, HEIGHT) or not content_id or not revision or \
            not 1 <= count <= MAX_PAGES or questions > MAX_QUESTIONS or \
            page_table != HEADER.size or page_bytes != count * PAGE.size or \
            question_table != page_table + page_bytes or \
            question_bytes != questions * QUESTION_BYTES or \
            data_start != question_table + question_bytes or total != len(blob) or \
            not _text_ok(title, MAX_TITLE_BYTES) or any(reserved) or \
            crc32(blob[:92]) != head_crc:
        raise ValueError("Invalid KPB2 header")
    check_series(content_id, series)
    if crc32(blob[page_table:data_start]) != tables_crc:
        raise ValueError("KPB2 table CRC mismatch")
    chain = _Chain(blob, data_start)
    result = {"format": "KPB2", "package_type": "book", "content_id": content_id,
              "revision": revision, "title": chain.take(title, "Title").decode("utf-8"),
              "pages": [], "question_count": questions, "version": version,
              "branching": branching,
              "series": {"id": series[0], "index": series[1], "count": series[2],
                         "next_content_id": series[3]}}
    seen = 0
    links: list[tuple[int, list[int] | None]] = []
    for i in range(count):
        f = PAGE.unpack_from(blob, page_table + i * PAGE.size)
        number, pflags, first, qcount = f[:4]
        mono, grey, pcm = _seg(f, 4), _seg(f, 7), _seg(f, 10)
        rate, channels, bits = f[13:16]
        text = _seg(f, 16)
        has_grey = bool(pflags & PAGE_FLAG_GREY)
        if number != i + 1 or pflags & ~PAGE_FLAG_GREY or first != seen or \
                qcount > MAX_QUESTIONS_PER_PAGE or seen + qcount > questions or \
                any(f[19][2:] if branching else f[19]) or \
                mono[1] != MONO_BYTES or \
                (grey[1] != GREY_BYTES if has_grey else any(grey)) or \
                (rate, channels, bits) != (SAMPLE_RATE, 1, 16) or \
                not _pcm_ok(pcm, MAX_PAGE_PCM) or not _text_ok(text, MAX_PAGE_TEXT_BYTES):
            raise ValueError(f"Invalid KPB2 page record {i + 1}")
        page = {"number": number, "mono": chain.take(mono, "Mono page"),
                "next": struct.unpack_from("<H", f[19])[0] if branching else 0}
        page["grey"] = chain.take(grey, "Grey page") if has_grey else None
        chain.take(pcm, "Page audio")
        page["text"] = chain.take(text, "Page text").decode("utf-8")
        page["questions"] = []
        for j in range(qcount):
            at = question_table + (seen + j) * QUESTION_BYTES
            q = _parse_question(blob[at:at + QUESTION_BYTES], number, j, branching)
            chain.take(q["prompt_pcm"], "Prompt audio")
            q["prompt"] = chain.take(q["prompt_text"], "Prompt text").decode("utf-8")
            for o in q["options"]:
                o["icon_bits"] = chain.take(o["icon"], "Option icon")
                chain.take(o["pcm"], "Option audio")
                o["label_text"] = chain.take(o["label"], "Option label").decode("utf-8")
            page["questions"].append(q)
        branch_qs = [q for q in page["questions"] if q["branch"]]
        if branch_qs and (len(branch_qs) != 1 or not page["questions"][-1]["branch"]):
            raise ValueError(f"Page {number}: only the last question may branch")
        links.append((page["next"],
                      [o["goto"] for o in branch_qs[0]["options"]] if branch_qs else None))
        seen += qcount
        result["pages"].append(page)
    if seen != questions or chain.end != total:
        raise ValueError("Question count mismatch or trailing bytes")
    explicit = check_story_graph(count, links)
    if branching and not explicit:
        raise ValueError("Branching flag set but no page has a link")
    return result


# ----------------------------------------------------------------- shelf

def shelf_key(info: dict) -> tuple[int, int, int, int]:
    """Bookshelf order: a series sits together, in reading order; standalone
    books and series are placed by their id (series id / content id). The C++
    `ShelfBefore` (book_reader.h) uses the same key."""
    series = info.get("series") or {}
    sid = series.get("id", 0)
    return (sid or info["content_id"], 1 if sid else 0, series.get("index", 0),
            info["content_id"])


def shelf_order(infos: list[dict]) -> list[dict]:
    return sorted(infos, key=shelf_key)


def next_in_series(infos: list[dict], info: dict) -> dict | None:
    """The book `info` points to (series.next_content_id) among `infos`."""
    wanted = (info.get("series") or {}).get("next_content_id", 0)
    return next((i for i in infos if wanted and i["content_id"] == wanted), None)


# --------------------------------------------------------------- preview

def _png(path: Path, width: int, height: int, lum: bytes) -> None:
    """8-bit greyscale PNG without Pillow, for parents to look at."""
    rows = b"".join(b"\0" + lum[y * width:(y + 1) * width] for y in range(height))

    def chunk(kind: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + kind + body + \
            struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    path.write_bytes(b"\x89PNG\r\n\x1a\n" +
                     chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def preview(package: Path, output: Path) -> list[Path]:
    """Write what the device will show: grey page, 1-bit fallback, and each
    question with its icons drawn over the page and highlight rings."""
    info = parse(package.read_bytes())
    output.mkdir(parents=True, exist_ok=True)
    written = []
    for page in info["pages"]:
        n = page["number"]
        mono = bytes(255 * v for v in unpack_bits(page["mono"])) if "mono" in page else None
        if mono is None:
            continue  # KPB1 previews: use the source PNG
        base = mono
        if page["grey"] is not None:
            base = bytes(85 * v for v in unpack_grey(page["grey"]))
            target = output / f"page{n:02d}_grey.png"
            _png(target, WIDTH, HEIGHT, base)
            written.append(target)
        target = output / f"page{n:02d}_mono.png"
        _png(target, WIDTH, HEIGHT, mono)
        written.append(target)
        for q in page["questions"]:
            canvas = bytearray(base)
            for o in q["options"]:
                x0, y0, w, h = o["box"]
                bits = unpack_bits(o["icon_bits"])
                for y in range(h):
                    canvas[(y0 + y) * WIDTH + x0:(y0 + y) * WIDTH + x0 + w] = \
                        bytes(255 * v for v in bits[y * w:(y + 1) * w])
                rx, ry, rw, rh = _rect(x0, y0, w, h)
                for y in range(ry + 2, ry + rh - 2):
                    for x in range(rx + 2, rx + rw - 2):
                        if not (x0 - 3 <= x < x0 + w + 3 and y0 - 3 <= y < y0 + h + 3):
                            canvas[y * WIDTH + x] = 0
            target = output / f"page{n:02d}_q{q['index'] + 1}.png"
            _png(target, WIDTH, HEIGHT, bytes(canvas))
            written.append(target)
    return written
