#!/usr/bin/env python3
"""Voice interaction probe: play Chinese test sentences from the PC speaker to the board.

Why: testing the child voice path (wake word -> ASR -> guard -> PetLearning ->
TTS -> device) used to need a person talking again and again. This tool
synthesises each sentence with Windows SAPI (zh-CN voice), plays it through the
PC speakers next to the board, and records both sides while it does:

* the board's serial console (COM port, **read only**, DTR/RTS held low so the
  board is not reset), and
* ``docker logs --since`` of the xiaozhi-server and pet-learning containers on
  the server VM, over SSH.

Per sentence it reports the ASR text, the guard ``kind``, the spoken reply, the
device action, and the delay from end of speech to the first reply audio.

Stdlib + optional ``pyserial``. No secrets are read or printed; everything that
is saved passes through :func:`redact`.

    python tools/voice_probe.py list
    python tools/voice_probe.py listen --seconds 10 --port COM10     # passive, plays nothing
    python tools/voice_probe.py run --cases tools/voice_probe_cases.toml --port COM10 \
        --ssh-key C:/SPB_Data/.ssh/pet_server_ed25519 --ssh-host root@192.168.1.110 \
        --out C:/Users/bird/dev/xiaozhi-pet-backups/voice/run1
    python tools/voice_probe.py run ... --dry-run     # synthesise only, play nothing
"""
from __future__ import annotations

import argparse
import dataclasses
import datetime as dt
import difflib
import json
import os
import re
import subprocess
import sys
import tempfile
import threading
import time
import tomllib
import unicodedata
from pathlib import Path

DEFAULT_CASES = Path(__file__).with_name("voice_probe_cases.toml")
WAKE_PHRASE = "你好小智"
CONTAINERS = {"xiaozhi": "pet-server-xiaozhi-server-1", "learning": "pet-server-pet-learning-1"}
KINDS = ("fixed", "approved_content", "local_command", "fallback")

# ---------------------------------------------------------------------------
# Redaction


_REDACTIONS = [
    (re.compile(r"(?i)(authorization['\"]?\s*[:=]\s*['\"]?)(bearer\s+)?[A-Za-z0-9._\-+/=]{8,}"), r"\1<redacted>"),
    (re.compile(r"(?i)\b(bearer)\s+[A-Za-z0-9._\-+/=]{16,}"), r"\1 <redacted>"),
    (re.compile(r"(?i)((?:token|secret|password|passwd|psk|api[_-]?key|auth[_-]?key|sign(?:ature)?)['\"]?\s*[:=]\s*['\"]?)[^\s'\",;}]{6,}"),
     r"\1<redacted>"),
    (re.compile(r"(?i)((?:ssid|wifi[_-]?pass\w*)['\"]?\s*[:=]\s*['\"]?)[^\s'\",;}]+"), r"\1<redacted>"),
]


def redact(text: str) -> str:
    for pattern, repl in _REDACTIONS:
        text = pattern.sub(repl, text)
    return text


# ---------------------------------------------------------------------------
# Cases


@dataclasses.dataclass(frozen=True)
class Case:
    id: str
    say: str
    group: str = "misc"
    # Expectations. Empty/None means "not checked".
    kind: str = ""            # fixed | approved_content | local_command | fallback
    reason: str = ""          # PetLearning reason, e.g. fixed_command, quiet_hours, no_match
    action: str = ""          # command name the device should log (PET_VOICE_CMD name=...)
    tts_contains: str = ""    # substring of the spoken reply
    asr: str = ""             # expected ASR text; default: the sentence itself
    quiet: str = "any"        # any | only (needs quiet hours) | never (needs non-quiet hours)
    # Overrides that apply when the run happens in quiet hours (empty = same as above).
    kind_quiet: str = ""
    reason_quiet: str = ""
    action_quiet: str = ""    # "none" = the device must not act
    tts_contains_quiet: str = ""
    skip: str = ""            # non-empty: never played (reason shown in the report); --include-skipped overrides
    notes: str = ""


def parse_cases(text: str) -> list[Case]:
    data = tomllib.loads(text)
    rows = data.get("case")
    if not isinstance(rows, list) or not rows:
        raise ValueError("no [[case]] entries")
    fields = {f.name for f in dataclasses.fields(Case)}
    seen: set[str] = set()
    cases = []
    for row in rows:
        unknown = set(row) - fields
        if unknown:
            raise ValueError(f"case {row.get('id', '?')}: unknown keys {sorted(unknown)}")
        if not row.get("id") or not row.get("say"):
            raise ValueError(f"case needs id and say: {row}")
        if row["id"] in seen:
            raise ValueError(f"duplicate case id {row['id']}")
        seen.add(row["id"])
        case = Case(**row)
        if (case.kind and case.kind not in KINDS) or (case.kind_quiet and case.kind_quiet not in KINDS):
            raise ValueError(f"case {case.id}: kind must be one of {KINDS}")
        if case.quiet not in ("any", "only", "never"):
            raise ValueError(f"case {case.id}: quiet must be any/only/never")
        cases.append(case)
    return cases


def load_cases(path: Path) -> list[Case]:
    return parse_cases(path.read_text(encoding="utf-8"))


# ---------------------------------------------------------------------------
# Quiet hours


def parse_quiet_window(serial_text: str):
    """``quiet=20:30-07:00`` from the board's PET_PARENT_SETTINGS line -> (start_min, end_min)."""
    found = None
    for m in re.finditer(r"PET_PARENT_SETTINGS[^\n]*?\bquiet=(\d{1,2}):(\d{2})-(\d{1,2}):(\d{2})", serial_text):
        found = (int(m[1]) * 60 + int(m[2]), int(m[3]) * 60 + int(m[4]))
    return found


def in_quiet(window, now: dt.datetime) -> bool | None:
    if window is None:
        return None
    start, end = window
    minute = now.hour * 60 + now.minute
    if start == end:
        return False
    return start <= minute < end if start < end else (minute >= start or minute < end)


def case_applicable(case: Case, quiet: bool | None) -> tuple[bool, str]:
    if case.quiet == "any":
        return True, ""
    if quiet is None:
        return True, "quiet window unknown"
    if case.quiet == "only" and not quiet:
        return False, "needs quiet hours"
    if case.quiet == "never" and quiet:
        return False, "needs non-quiet hours"
    return True, ""


# ---------------------------------------------------------------------------
# Log parsing. Each parser returns a list of events {"t": epoch seconds, "ev": name, ...}.

_DOCKER_TS = re.compile(r"^(\d{4}-\d{2}-\d{2})T(\d{2}:\d{2}:\d{2})(?:\.(\d+))?Z\s?(.*)$")


def parse_docker_ts(line: str):
    """``--timestamps`` line -> (epoch seconds UTC, message) or None."""
    m = _DOCKER_TS.match(line.strip("\r\n"))
    if not m:
        return None
    frac = float("0." + m[3]) if m[3] else 0.0
    base = dt.datetime.strptime(f"{m[1]} {m[2]}", "%Y-%m-%d %H:%M:%S").replace(tzinfo=dt.timezone.utc)
    return base.timestamp() + frac, m[4]


_ANSI = re.compile(r"\x1b\[[0-9;]*m")
_XZ_PATTERNS = [
    ("wake", re.compile(r"收到listen消息.*\"state\":\"detect\".*?\"text\":\"([^\"]*)\"")),
    ("listen_start", re.compile(r"收到listen消息.*\"state\":\"start\"")),
    ("asr", re.compile(r"(?:识别文本|ASR结果|识别结果)[:：]\s*(.+)$")),
    ("tts_first", re.compile(r"发送第一段语音[:：]\s*(.*)$")),
    ("tts_ok", re.compile(r"语音生成成功[:：]\s*(.*?)(?:，重试\d+次)?$")),
    ("tts_end", re.compile(r"发送音频消息: SentenceType\.LAST")),
    ("closed", re.compile(r"客户端断开连接")),
]
_PL_TURN = re.compile(r"voice_turn kind=(\w+) reason=(\w+) ms=(\d+)")


def parse_xiaozhi_line(t: float, msg: str):
    msg = _ANSI.sub("", msg)
    for name, pat in _XZ_PATTERNS:
        m = pat.search(msg)
        if m:
            return {"t": t, "ev": name, "text": (m.group(1).strip() if m.groups() else "")}
    return None


def parse_learning_line(t: float, msg: str):
    m = _PL_TURN.search(_ANSI.sub("", msg))
    if m:
        return {"t": t, "ev": "turn", "kind": m[1], "reason": m[2], "ms": int(m[3])}
    return None


_SERIAL_PATTERNS = [
    ("stt", re.compile(r">>\s*(.+)$")),                       # xiaozhi Application: STT text
    ("tts_sentence", re.compile(r"<<\s*(.+)$")),              # xiaozhi Application: TTS sentence
    ("state", re.compile(r"(?i)\bSTATE:\s*(\w+)")),
    ("wake", re.compile(r"(?i)wake ?word detected|WakeWord detected|Wake word:\s*(\S+)")),
    ("voice_cmd", re.compile(r"PET_VOICE_CMD name=(\S+) result=(\S+)(?: action=(\S+))?")),
    ("power", re.compile(r"PET_POWER state=(\w+)")),
    ("voice_time", re.compile(r"PET_VOICE_TIME ")),
    ("voice_battery", re.compile(r"PET_VOICE_BATTERY ")),
]


def parse_serial_line(t: float, line: str):
    for name, pat in _SERIAL_PATTERNS:
        m = pat.search(line)
        if m:
            ev = {"t": t, "ev": name, "text": (m.group(1) or "").strip() if m.groups() else ""}
            if name == "voice_cmd":
                ev.update(name_=m[1], result=m[2], action=m[3] or "")
            return ev
    return None


# ---------------------------------------------------------------------------
# Judging


def norm(text: str) -> str:
    folded = unicodedata.normalize("NFKC", text).casefold()
    return "".join(c for c in folded if unicodedata.category(c)[0] not in "PZC")


def asr_similarity(expected: str, got: str) -> float:
    a, b = norm(expected), norm(got)
    if not a and not b:
        return 1.0
    return difflib.SequenceMatcher(None, a, b).ratio()


@dataclasses.dataclass
class Observation:
    """What was seen between 'sentence started' and the end of the window."""
    speech_end: float                     # PC clock, end of playback
    asr_texts: list = dataclasses.field(default_factory=list)   # (t, text) server+device STT
    kind: str = ""
    reason: str = ""
    turn_ms: int | None = None
    tts_text: str = ""
    t_asr: float | None = None            # server logged the ASR text, PC clock
    t_tts_server: float | None = None     # server first-sentence time, PC clock
    t_tts_device: float | None = None     # device speaking-state / '<<' time, PC clock
    device_cmds: list = dataclasses.field(default_factory=list)  # (name, result, action)
    woke: bool | None = None


def build_observation(speech_start: float, speech_end: float, window_end: float,
                      xz: list, pl: list, ser: list, wake_counted: bool | None = None) -> Observation:
    obs = Observation(speech_end=speech_end, woke=wake_counted)
    for ev in xz:
        if not speech_start <= ev["t"] <= window_end:
            continue
        if ev["ev"] == "asr":
            obs.asr_texts.append((ev["t"], ev["text"]))
            if obs.t_asr is None and norm(ev["text"]) not in ("", norm(WAKE_PHRASE)):
                obs.t_asr = ev["t"]
        elif ev["ev"] == "tts_first" and obs.t_tts_server is None and ev["t"] >= speech_end - 0.5:
            obs.t_tts_server, obs.tts_text = ev["t"], ev["text"]
    for ev in pl:
        if ev["ev"] == "turn" and speech_start <= ev["t"] <= window_end and not obs.kind:
            obs.kind, obs.reason, obs.turn_ms = ev["kind"], ev["reason"], ev["ms"]
    for ev in ser:
        if not speech_start <= ev["t"] <= window_end:
            continue
        if ev["ev"] == "stt" and ev["t"] >= speech_start:
            obs.asr_texts.append((ev["t"], ev["text"]))
        elif ev["ev"] == "tts_sentence" and obs.t_tts_device is None and ev["t"] >= speech_end - 0.5:
            obs.t_tts_device = ev["t"]
            obs.tts_text = obs.tts_text or ev["text"]
        elif ev["ev"] == "state" and ev["text"].lower() == "speaking" and obs.t_tts_device is None \
                and ev["t"] >= speech_end - 0.5:
            obs.t_tts_device = ev["t"]
        elif ev["ev"] == "voice_cmd":
            obs.device_cmds.append((ev["name_"], ev["result"], ev["action"]))
    return obs


SLOW_S = 3.0


def latency(obs: Observation):
    ts = [t for t in (obs.t_tts_server, obs.t_tts_device) if t is not None]
    return round(min(ts) - obs.speech_end, 2) if ts else None


def stages(obs: Observation) -> dict:
    """Split speech-end -> first reply audio into ASR / server (guard+PetLearning) / TTS first packet.

    asr  = end of speech -> server logged the recognised text
    server = ASR text -> TTS asked to speak (guard + PetLearning turn, ``turn_ms`` is PetLearning alone)
    tts  = server asks to speak -> device starts speaking (serial); needs both sides, else None
    """
    out = {"asr_s": None, "server_s": None, "tts_s": None}
    if obs.t_asr is not None:
        out["asr_s"] = round(obs.t_asr - obs.speech_end, 2)
        if obs.t_tts_server is not None:
            out["server_s"] = round(obs.t_tts_server - obs.t_asr, 2)
    if obs.t_tts_server is not None and obs.t_tts_device is not None:
        out["tts_s"] = round(obs.t_tts_device - obs.t_tts_server, 2)
    return out


def percentile(values: list[float], p: float) -> float | None:
    if not values:
        return None
    s = sorted(values)
    k = (len(s) - 1) * p / 100
    lo, hi = int(k), min(int(k) + 1, len(s) - 1)
    return round(s[lo] + (s[hi] - s[lo]) * (k - lo), 2)


def summarize(results: list[dict]) -> dict:
    """Success rate per group plus median/P90 of the total latency and of each stage."""
    ran = [r for r in results if r.get("status") == "ran"]
    groups: dict[str, dict] = {}
    for r in ran:
        g = groups.setdefault(r.get("group", "misc"), {"n": 0, "passed": 0})
        g["n"] += 1
        g["passed"] += 1 if r["passed"] else 0

    def stat(key: str, src=lambda r: r):
        vals = [src(r)[key] for r in ran if src(r).get(key) is not None]
        return {"n": len(vals), "median": percentile(vals, 50), "p90": percentile(vals, 90)}

    return {"groups": groups, "total": stat("latency_s"),
            "stages": {k: stat(k, lambda r: r.get("stages", {})) for k in ("asr_s", "server_s", "tts_s")}}


def effective_case(case: Case, quiet: bool | None) -> Case:
    """Apply the *_quiet overrides when the run is inside quiet hours."""
    if not quiet or not (case.kind_quiet or case.action_quiet or case.reason_quiet):
        return case
    action = "" if case.action_quiet == "none" else (case.action_quiet or case.action)
    return dataclasses.replace(case, kind=case.kind_quiet or case.kind, reason=case.reason_quiet,
                               action=action, tts_contains=case.tts_contains_quiet)


def judge(case: Case, obs: Observation, quiet: bool | None = None) -> dict:
    """-> {passed, asr, issues:[(severity, text)]}. Severity order: none > asr > action > slow > tts."""
    case = effective_case(case, quiet)
    issues: list[tuple[str, str]] = []
    heard = [txt for _, txt in obs.asr_texts if norm(txt) not in ("", norm(WAKE_PHRASE))]
    asr = heard[-1] if heard else ""
    got_anything = bool(asr or obs.tts_text or obs.kind or obs.device_cmds or obs.t_tts_device or obs.t_tts_server)
    if not got_anything:
        issues.append(("none", "完全没反应：没有识别文本、没有播报、没有设备动作"))
    else:
        want_asr = case.asr or case.say
        sim = asr_similarity(want_asr, asr) if asr else 0.0
        if not asr:
            issues.append(("none", "没有拿到识别文本（板子/服务端都没有 ASR 记录）"))
        elif norm(want_asr) not in norm(asr) and sim < 0.85:
            issues.append(("asr", f"识别错：期望“{want_asr}”，识别为“{asr}”（相似度 {sim:.2f}）"))
        if case.kind and obs.kind and obs.kind != case.kind:
            issues.append(("action", f"kind 不符：期望 {case.kind}，实际 {obs.kind}/{obs.reason}"))
        elif case.kind and not obs.kind:
            issues.append(("none", f"没有 kind（PetLearning 没收到这轮，期望 {case.kind}）"))
        if case.reason and obs.reason and obs.reason != case.reason:
            issues.append(("action", f"reason 不符：期望 {case.reason}，实际 {obs.reason}"))
        if case.action:
            acts = [c for c in obs.device_cmds if c[0] == case.action]
            if not acts:
                got = ",".join(c[0] for c in obs.device_cmds) or "无"
                issues.append(("action", f"设备没有执行 {case.action}（板子上的命令：{got}）"))
            elif acts[0][1] != "done":
                issues.append(("action", f"设备执行 {case.action} 结果 {acts[0][1]}（不是 done）"))
        elif not case.kind or case.kind in ("fallback", "fixed", "approved_content"):
            stray = [c for c in obs.device_cmds if c[1] == "done"]
            if stray:
                issues.append(("action", f"不该有设备动作却执行了 {stray[0][0]}"))
        if case.tts_contains and case.tts_contains not in obs.tts_text:
            issues.append(("tts", f"播报不含“{case.tts_contains}”，实际“{obs.tts_text or '无'}”"))
        if not obs.tts_text and not obs.t_tts_device and got_anything:
            issues.append(("none", "没有播报（服务端和板子都没有开始播放）"))
        lat = latency(obs)
        if lat is not None and lat > SLOW_S:
            issues.append(("slow", f"慢：从说完到开始播报 {lat:.1f}s（> {SLOW_S:.0f}s）"))
    return {"passed": not issues, "asr": asr, "issues": issues}


SEVERITY_ORDER = {"none": 0, "asr": 1, "action": 2, "slow": 3, "tts": 4}
SEVERITY_LABEL = {"none": "完全没反应", "asr": "识别错", "action": "动作错", "slow": "慢", "tts": "音色/话术"}


def worst_severity(issues) -> str | None:
    return min((s for s, _ in issues), key=SEVERITY_ORDER.get) if issues else None


# ---------------------------------------------------------------------------
# Report


def _cell(text) -> str:
    return str(text if text not in (None, "") else "-").replace("|", "\\|").replace("\n", " ")


def render_report(results: list[dict], meta: dict) -> str:
    out = ["# 语音交互基线报告", ""]
    for k, v in meta.items():
        out.append(f"- {k}：{v}")
    out += ["", "| # | 句子 | 识别文本 | kind/reason | 播报 | 设备动作 | 延迟(s) | 结果 |", "|---|---|---|---|---|---|---|---|"]
    for i, r in enumerate(results, 1):
        if r["status"] == "skipped":
            out.append(f"| {i} | {_cell(r['say'])} | - | - | - | - | - | 跳过：{_cell(r['skip_reason'])} |")
            continue
        kind = f"{r['kind']}/{r['reason']}" if r["kind"] else ""
        acts = ";".join(f"{n}={res}" for n, res, _ in r["device_cmds"])
        verdict = "通过" if r["passed"] else "不通过"
        out.append(f"| {i} | {_cell(r['say'])} | {_cell(r['asr'])} | {_cell(kind)} | {_cell(r['tts_text'])} "
                   f"| {_cell(acts)} | {_cell(r['latency_s'])} | {verdict} |")
    s = summarize(results)
    out += ["", "## 汇总", ""]
    for name, g in s["groups"].items():
        out.append(f"- {name}：{g['passed']}/{g['n']} 通过（{100 * g['passed'] // max(g['n'], 1)}%）")
    fmt = lambda d: f"n={d['n']} 中位数 {d['median']}s P90 {d['p90']}s" if d["n"] else "无数据"
    out.append(f"- 说完到开始播报：{fmt(s['total'])}")
    for key, label in (("asr_s", "ASR（说完→识别文本）"), ("server_s", "服务端（识别→请求播报）"),
                       ("tts_s", "TTS 首包（请求播报→板子开口）")):
        out.append(f"- {label}：{fmt(s['stages'][key])}")
    issues = [(worst_severity(r["issues"]), r) for r in results if r.get("issues")]
    out += ["", "## 问题清单（按严重程度）", ""]
    if not issues:
        out.append("无。")
    n = 0
    for sev in sorted(SEVERITY_ORDER, key=SEVERITY_ORDER.get):
        for s, r in issues:
            if s != sev:
                continue
            for isev, text in r["issues"]:
                if isev == sev:
                    n += 1
                    out.append(f"{n}. 【{SEVERITY_LABEL[sev]}】“{r['say']}”：{text}")
    return "\n".join(out) + "\n"


# ---------------------------------------------------------------------------
# I/O helpers (not unit tested: need hardware / network / Windows)

_PS_SYNTH = r"""
param([string]$TextFile,[string]$Out,[string]$Voice,[int]$Rate)
Add-Type -AssemblyName System.Speech
$s = New-Object System.Speech.Synthesis.SpeechSynthesizer
if ($Voice) { $s.SelectVoice($Voice) } else { $s.SelectVoiceByHints('NotSet','NotSet',0,[Globalization.CultureInfo]'zh-CN') }
$s.Rate = $Rate
$s.SetOutputToWaveFile($Out)
$s.Speak([IO.File]::ReadAllText($TextFile,[Text.Encoding]::UTF8))
$s.Dispose()
"""
_PS_PLAY = r"""
param([string]$Wav)
$p = New-Object System.Media.SoundPlayer $Wav
$p.PlaySync()
"""


def _run_ps(script: str, *args: str, timeout: float = 60.0) -> None:
    with tempfile.NamedTemporaryFile("w", suffix=".ps1", delete=False, encoding="utf-8-sig") as f:
        f.write(script)
        path = f.name
    try:
        subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", path, *args],
                       check=True, timeout=timeout, capture_output=True)
    finally:
        os.unlink(path)


def synth_wav(text: str, out: Path, voice: str = "", rate: int = 0) -> Path:
    """Windows SAPI (zh-CN voice) -> wav. Cached by caller."""
    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False, encoding="utf-8") as f:
        f.write(text)
        txt = f.name
    try:
        _run_ps(_PS_SYNTH, txt, str(out), voice, str(rate))
    finally:
        os.unlink(txt)
    return out


def play_wav(path: Path) -> None:
    _run_ps(_PS_PLAY, str(path), timeout=120.0)


DEFAULT_NOISE_MAX_DBFS = -45.0


def wav_dbfs(path: Path) -> float:
    """RMS level of a 16-bit PCM wav in dBFS (-120 for silence)."""
    import array
    import math
    import wave
    with wave.open(str(path), "rb") as w:
        if w.getsampwidth() != 2:
            raise ValueError("need 16-bit PCM")
        samples = array.array("h")
        samples.frombytes(w.readframes(w.getnframes()))
    if sys.byteorder == "big":
        samples.byteswap()
    if not samples:
        return -120.0
    rms = math.sqrt(sum(s * s for s in samples) / len(samples))
    return max(-120.0, 20 * math.log10(rms / 32768.0)) if rms > 0 else -120.0


def noise_verdict(dbfs: float, max_dbfs: float) -> tuple[bool, str]:
    if dbfs > max_dbfs:
        return False, f"环境太吵：底噪 {dbfs:.1f} dBFS，高于阈值 {max_dbfs:.1f} dBFS。请关掉电视/风扇后重试"
    return True, f"底噪 {dbfs:.1f} dBFS（阈值 {max_dbfs:.1f}），安静，可以开跑"


def record_wav(path: Path, seconds: float) -> None:
    """Record the default Windows input device with winmm MCI (no extra packages)."""
    import ctypes
    mci = ctypes.windll.winmm.mciSendStringW
    buf = ctypes.create_unicode_buffer(256)

    def send(cmd: str) -> None:
        rc = mci(cmd, buf, 255, 0)
        if rc:
            err = ctypes.create_unicode_buffer(256)
            ctypes.windll.winmm.mciGetErrorStringW(rc, err, 255)
            raise RuntimeError(f"mci '{cmd}': {err.value}")

    send("open new type waveaudio alias vprec")
    try:
        send("set vprec bitspersample 16 channels 1 samplespersec 16000 alignment 2 bytespersec 32000")
        send("record vprec")
        time.sleep(seconds)
        send("stop vprec")
        send(f'save vprec "{path}"')
    finally:
        mci("close vprec", buf, 255, 0)


def measure_noise(seconds: float = 2.0) -> float:
    with tempfile.TemporaryDirectory() as d:
        wav = Path(d) / "noise.wav"
        record_wav(wav, seconds)
        return wav_dbfs(wav)


class SerialTail(threading.Thread):
    """Read-only serial capture. DTR/RTS are forced low BEFORE the port opens so the board is not reset."""

    def __init__(self, port: str, baud: int = 115200):
        super().__init__(daemon=True)
        import serial  # pyserial
        self._ser = serial.Serial()
        self._ser.port, self._ser.baudrate, self._ser.timeout = port, baud, 0.2
        self._ser.dtr = False
        self._ser.rts = False
        self._ser.open()
        self._stop = threading.Event()
        self.lines: list[tuple[float, str]] = []
        self._lock = threading.Lock()

    def run(self) -> None:
        buf = b""
        while not self._stop.is_set():
            try:
                chunk = self._ser.read(4096)
            except Exception:
                break
            if not chunk:
                continue
            now = time.time()
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                text = _ANSI.sub("", raw.decode("utf-8", "replace").rstrip("\r"))
                with self._lock:
                    self.lines.append((now, text))

    def snapshot(self) -> list[tuple[float, str]]:
        with self._lock:
            return list(self.lines)

    def stop(self) -> None:
        self._stop.set()
        self.join(timeout=2)
        self._ser.close()


class Remote:
    """docker logs over ssh; also measures the clock offset (server - PC)."""

    def __init__(self, host: str, key: str | None, containers=None):
        self.host, self.key = host, key
        self.containers = containers or CONTAINERS

    def _ssh(self, command: str, timeout: float = 30.0) -> str:
        argv = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8"]
        if self.key:
            argv += ["-i", self.key]
        argv += [self.host, command]
        done = subprocess.run(argv, capture_output=True, timeout=timeout)
        return done.stdout.decode("utf-8", "replace") + done.stderr.decode("utf-8", "replace")

    def clock_offset(self) -> float:
        """server_epoch - pc_epoch, corrected for half the round trip."""
        t0 = time.time()
        out = self._ssh("date +%s.%N").strip().splitlines()[-1]
        t1 = time.time()
        return float(out) - (t0 + t1) / 2

    def device_quiet(self, business_id: str) -> bool | None:
        """Read-only: the quiet flag PetLearning holds for the device (what the voice turn really uses)."""
        if not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", business_id):
            raise ValueError("bad business id")
        code = ("import sqlite3;c=sqlite3.connect('file:/data/pet_learning.sqlite3?mode=ro',uri=True);"
                f"r=c.execute('select quiet from devices where device_id=?',('{business_id}',)).fetchone();"
                "print('Q=%s'%(r[0] if r else 'none'))")
        out = self._ssh(f'docker exec pet-server-pet-learning-1 python -c "{code}"')
        m = re.search(r"Q=(\d|none)", out)
        return None if not m or m[1] == "none" else m[1] != "0"

    def logs(self, name: str, since_epoch: float) -> list[tuple[float, str]]:
        since = dt.datetime.fromtimestamp(since_epoch, dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
        raw = self._ssh(f"docker logs --timestamps --since {since} {self.containers[name]} 2>&1", timeout=60)
        rows = []
        for line in raw.splitlines():
            parsed = parse_docker_ts(line)
            if parsed:
                rows.append(parsed)
        return rows


# ---------------------------------------------------------------------------
# Orchestration


def _events_from(rows, parser, offset=0.0):
    out = []
    for t, msg in rows:
        ev = parser(t - offset, msg)
        if ev:
            out.append(ev)
    return out


def latest_state(ser_events: list, before: float) -> str:
    state = ""
    for ev in ser_events:
        if ev["t"] <= before and ev["ev"] == "state":
            state = ev["text"].lower()
    return state


def last_power(ser_events: list, before: float) -> str:
    """Latest PET_POWER state (active/idle/nap/...) the serial log showed before `before`; '' = none seen."""
    state = ""
    for ev in ser_events:
        if ev["ev"] == "power" and ev["t"] <= before:
            state = ev["text"]
    return state


SESSION_IDLE_S = 150.0   # the server closes a silent connection after ~180 s


def session_open(xz: list, now: float) -> bool:
    """True when the board's xiaozhi session is (probably) still open and listening."""
    last_open = last_closed = None
    for ev in xz:
        if ev["ev"] in ("wake", "listen_start", "asr", "tts_first", "tts_end"):
            last_open = ev["t"]
        elif ev["ev"] == "closed":
            last_closed = ev["t"]
    if last_open is None:
        return False
    if last_closed is not None and last_closed >= last_open:
        return False
    return now - last_open < SESSION_IDLE_S


def wait_for_event(remote, offset: float, name: str, after: float, timeout: float, poll: float = 1.2) -> bool:
    """Poll the server log until an event `name` newer than `after` (PC clock) shows up."""
    end = time.time() + timeout
    while time.time() < end:
        evs = _events_from(remote.logs("xiaozhi", after - 3), parse_xiaozhi_line, offset)
        if any(e["ev"] == name and e["t"] >= after for e in evs):
            return True
        time.sleep(poll)
    return False


def cmd_list(args) -> int:
    for c in load_cases(Path(args.cases)):
        print(f"{c.id:18s} [{c.group}] {c.say}  -> {c.kind or '?'}{'/' + c.action if c.action else ''}  quiet={c.quiet}")
    return 0


def cmd_listen(args) -> int:
    tail = SerialTail(args.port)
    tail.start()
    time.sleep(args.seconds)
    tail.stop()
    lines = tail.snapshot()
    text = "\n".join(redact(line) for _, line in lines)
    if args.out:
        Path(args.out).write_text(text, encoding="utf-8")
    events = [e for e in (parse_serial_line(t, line) for t, line in lines) if e]
    print(f"{len(lines)} lines, {len(events)} voice-related events")
    for e in events[:50]:
        print(" ", {k: v for k, v in e.items() if k != "t"})
    return 0


def cmd_noise(args) -> int:
    level = measure_noise(args.seconds)
    ok, msg = noise_verdict(level, args.max_dbfs)
    print(msg)
    return 0 if ok else 3


def wake_hits(attempts: list[tuple[float, float]], xz: list, ser: list) -> list[dict]:
    """attempts = [(t_play_start, t_window_end)]; a hit = server 'detect' or serial wake/listening state."""
    rows = []
    for t0, t1 in attempts:
        det = [e for e in xz if e["ev"] == "wake" and t0 <= e["t"] <= t1]
        ser_hit = [e for e in ser if t0 <= e["t"] <= t1 and (e["ev"] == "wake" or
                   (e["ev"] == "state" and e["text"].lower() in ("listening", "connecting")))]
        hit = bool(det or ser_hit)
        first = min([e["t"] for e in det + ser_hit], default=None)
        rows.append({"hit": hit, "delay_s": round(first - t0, 2) if first is not None else None})
    return rows


def cmd_wake(args) -> int:
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    wav = out / "wav" / "_wake.wav"
    if not wav.exists():
        synth_wav(WAKE_PHRASE, wav, args.voice, args.rate)
    tail = SerialTail(args.port) if args.port else None
    remote = Remote(args.ssh_host, args.ssh_key) if args.ssh_host else None
    offset = remote.clock_offset() if remote else 0.0
    if tail:
        tail.start()
        time.sleep(1.0)
    attempts = []
    for i in range(args.attempts):
        t0 = time.time()
        play_wav(wav)
        time.sleep(args.settle)
        attempts.append((t0, time.time()))
        print(f"wake attempt {i + 1}/{args.attempts} done", flush=True)
        time.sleep(args.gap)
    xz = _events_from(remote.logs("xiaozhi", attempts[0][0] - 2), parse_xiaozhi_line, offset) if remote else []
    ser = _events_from(tail.snapshot(), parse_serial_line) if tail else []
    if tail:
        tail.stop()
        (out / "serial.log").write_text("\n".join(redact(line) for _, line in tail.snapshot()), encoding="utf-8")
    rows = wake_hits(attempts, xz, ser)
    delays = [r["delay_s"] for r in rows if r["delay_s"] is not None]
    summary = {"attempts": len(rows), "hits": sum(r["hit"] for r in rows),
               "delay_median_s": percentile(delays, 50), "delay_p90_s": percentile(delays, 90), "rows": rows}
    (out / "wake.json").write_text(json.dumps(summary, ensure_ascii=False, indent=1), encoding="utf-8")
    print(f"唤醒命中 {summary['hits']}/{summary['attempts']}，延迟中位数 {summary['delay_median_s']}s "
          f"P90 {summary['delay_p90_s']}s")
    return 0


def cmd_run(args) -> int:
    cases = load_cases(Path(args.cases))
    if args.only:
        wanted = set(args.only.split(","))
        cases = [c for c in cases if c.id in wanted or c.group in wanted]
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    wavdir = out / "wav"
    wake_wav = wavdir / "_wake.wav"
    if not wake_wav.exists():
        synth_wav(WAKE_PHRASE, wake_wav, args.voice, args.rate)
    wavs = {}
    for c in cases:
        wavs[c.id] = wavdir / f"{c.id}.wav"
        if not wavs[c.id].exists():
            synth_wav(c.say, wavs[c.id], args.voice, args.rate)
    if args.dry_run:
        print(f"dry run: synthesised {len(wavs) + 1} wav files into {wavdir}")
        return 0

    noise_note = "未检测"
    if not args.no_noise_check:
        try:
            level = measure_noise(args.noise_seconds)
        except Exception as exc:  # no microphone, driver error ...
            print(f"无法检测底噪（{exc}）。接好麦克风，或确认环境安静后加 --no-noise-check。", file=sys.stderr)
            return 3
        ok, msg = noise_verdict(level, args.noise_max_dbfs)
        print(msg)
        noise_note = f"{level:.1f} dBFS"
        if not ok:
            return 3
    elif args.noise_note:
        noise_note = args.noise_note
    tail = SerialTail(args.port) if args.port else None
    remote = Remote(args.ssh_host, args.ssh_key) if args.ssh_host else None
    offset = remote.clock_offset() if remote else 0.0
    if tail:
        tail.start()
        time.sleep(1.5)
    start_ts = time.time() - 5
    ser_text = "\n".join(line for _, line in tail.snapshot()) if tail else ""
    # PET_PARENT_SETTINGS is only printed at boot; a running board falls back to the parent default window.
    window = parse_quiet_window(ser_text) or parse_quiet_window(args.quiet_window_line)
    quiet_src = "时钟+默认窗口"
    if args.quiet != "auto":
        quiet = args.quiet == "yes"
        quiet_src = "命令行指定"
    else:
        quiet = None
        if remote and args.business_id:
            quiet = remote.device_quiet(args.business_id)
            quiet_src = "服务端设备投影"
        if quiet is None:
            quiet = in_quiet(window, dt.datetime.now())
            quiet_src = "时钟+默认窗口"
    print(f"quiet={quiet}（来源：{quiet_src}）")

    results = []
    plan = [c for c in cases for _ in range(args.repeat)]
    for c in plan:
        ok, why = case_applicable(c, quiet)
        if c.skip and not args.include_skipped:
            ok, why = False, c.skip
        if not ok:
            if not any(r["id"] == c.id and r["status"] == "skipped" for r in results):
                results.append({"id": c.id, "say": c.say, "status": "skipped", "skip_reason": why})
            continue
        t_begin = time.time()
        # An open xiaozhi session keeps listening (auto mode): a wake word played then is just speech.
        needs_wake = not session_open(
            _events_from(remote.logs("xiaozhi", t_begin - 200), parse_xiaozhi_line, offset) if remote else [],
            t_begin)
        if needs_wake:
            t_w = time.time()
            play_wav(wake_wav)
            if remote:  # the board answers the wake word; let that reply finish before speaking
                wait_for_event(remote, offset, "tts_end", t_w, args.wake_settle + 12)
            time.sleep(1.5)
        t_say = time.time()
        play_wav(wavs[c.id])
        t_end = time.time()
        if remote:
            wait_for_event(remote, offset, "tts_end", t_end - 0.5, args.window)
            time.sleep(1.0)
        else:
            time.sleep(args.window)
        t_stop = time.time()
        time.sleep(args.gap)
        ser_events = _events_from([(t, line) for t, line in tail.snapshot()], parse_serial_line) if tail else []
        xz = pl = []
        if remote:
            xz = _events_from(remote.logs("xiaozhi", t_begin - 2), parse_xiaozhi_line, offset)
            pl = _events_from(remote.logs("learning", t_begin - 2), parse_learning_line, offset)
        obs = build_observation(t_say - 0.2, t_end, t_stop, xz, pl, ser_events, wake_counted=needs_wake)
        verdict = judge(c, obs, quiet)
        res = {"id": c.id, "say": c.say, "group": c.group, "status": "ran", "woke": needs_wake,
               "asr": verdict["asr"], "kind": obs.kind, "reason": obs.reason, "tts_text": obs.tts_text,
               "device_cmds": obs.device_cmds, "latency_s": latency(obs), "turn_ms": obs.turn_ms,
               "stages": stages(obs), "power": last_power(ser_events, t_begin),
               "passed": verdict["passed"], "issues": verdict["issues"]}
        results.append(res)
        print(f"[{'PASS' if res['passed'] else 'FAIL'}] {c.id}: asr={res['asr']!r} kind={obs.kind or '?'} "
              f"lat={res['latency_s']}")

    if tail:
        tail.stop()
        (out / "serial.log").write_text("\n".join(redact(line) for _, line in tail.snapshot()), encoding="utf-8")
    if remote:
        for name in CONTAINERS:
            rows = remote.logs(name, start_ts)
            (out / f"{name}.log").write_text(
                "\n".join(redact(f"{dt.datetime.fromtimestamp(t - offset).strftime('%H:%M:%S.%f')[:-3]} {m}")
                          for t, m in rows if "/healthz" not in m), encoding="utf-8")
    meta = {"时间": dt.datetime.now().strftime("%Y-%m-%d %H:%M"), "音色": args.voice or "SAPI zh-CN 默认",
            "安静时段": {None: "未知", True: "是", False: "否"}[quiet] + f"（{quiet_src}）",
            "环境底噪": noise_note, "服务器时钟偏差(s)": round(offset, 2)}
    (out / "results.json").write_text(json.dumps(results, ensure_ascii=False, indent=1), encoding="utf-8")
    (out / "report.md").write_text(render_report(results, meta), encoding="utf-8")
    print(f"report: {out / 'report.md'}")
    return 0 if all(r.get("passed", True) for r in results) else 1


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("list")
    p.add_argument("--cases", default=str(DEFAULT_CASES))
    p.set_defaults(fn=cmd_list)
    p = sub.add_parser("listen")
    p.add_argument("--port", required=True)
    p.add_argument("--seconds", type=float, default=10)
    p.add_argument("--out")
    p.set_defaults(fn=cmd_listen)
    p = sub.add_parser("run")
    p.add_argument("--cases", default=str(DEFAULT_CASES))
    p.add_argument("--only", help="comma separated case ids or groups")
    p.add_argument("--port", help="serial port, read only (e.g. COM10)")
    p.add_argument("--ssh-host")
    p.add_argument("--ssh-key")
    p.add_argument("--out", required=True)
    p.add_argument("--voice", default="")
    p.add_argument("--rate", type=int, default=-1)
    p.add_argument("--wake-settle", type=float, default=3.0, help="seconds to wait after the wake word")
    p.add_argument("--window", type=float, default=8.0, help="seconds to wait for a reply after speech")
    p.add_argument("--gap", type=float, default=4.0, help="idle seconds between cases")
    p.add_argument("--quiet", choices=("auto", "yes", "no"), default="auto")
    p.add_argument("--quiet-window", dest="quiet_window", default="20:30-07:00",
                   help="quiet hours HH:MM-HH:MM when the serial log has no PET_PARENT_SETTINGS line")
    p.add_argument("--dry-run", action="store_true", help="synthesise only, play nothing")
    p.add_argument("--noise-max-dbfs", type=float, default=DEFAULT_NOISE_MAX_DBFS,
                   help="refuse to run when the 2 s ambient level is above this (see the 'noise' command)")
    p.add_argument("--noise-seconds", type=float, default=2.0)
    p.add_argument("--no-noise-check", action="store_true")
    p.add_argument("--noise-note", default="", help="free text for the report when noise is not measured")
    p.add_argument("--business-id", help="PetLearning device id: read its quiet flag from the server")
    p.add_argument("--repeat", type=int, default=1, help="play every case N times")
    p.add_argument("--include-skipped", action="store_true", help="also play cases marked skip")
    p.set_defaults(fn=cmd_run)
    p = sub.add_parser("wake", help="wake-word hit rate: play the wake phrase N times, count server 'detect' events")
    p.add_argument("--port")
    p.add_argument("--ssh-host")
    p.add_argument("--ssh-key")
    p.add_argument("--out", required=True)
    p.add_argument("--attempts", type=int, default=10)
    p.add_argument("--settle", type=float, default=6.0, help="seconds to watch after each wake phrase")
    p.add_argument("--gap", type=float, default=25.0, help="idle seconds so the board is back to idle")
    p.add_argument("--voice", default="")
    p.add_argument("--rate", type=int, default=-1)
    p.set_defaults(fn=cmd_wake)
    p = sub.add_parser("noise", help="measure ambient level with the PC microphone and print it (calibration)")
    p.add_argument("--seconds", type=float, default=2.0)
    p.add_argument("--max-dbfs", type=float, default=DEFAULT_NOISE_MAX_DBFS)
    p.set_defaults(fn=cmd_noise)
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    args = ap.parse_args(argv)
    if getattr(args, "quiet_window", None):
        args.quiet_window_line = f"PET_PARENT_SETTINGS quiet={args.quiet_window}"
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
