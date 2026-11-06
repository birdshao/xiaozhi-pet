"""Batch narration of licensed story text and original preschool game prompts.

Default backend: optional edge-tts online service + ffmpeg conversion. Generated
WAV is a *draft* needing adult audition and provider terms review. Tests inject
an offline synthetic backend marked test_only. No child microphone data is used.
"""
from __future__ import annotations

import asyncio
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import wave
from typing import Callable

from content_pipeline.schema import ContentError, content_digest, validate_document, write_json
from content_pipeline.game_lessons import audio_map, audio_script, validate_lesson
from content_pipeline.packaging import _asset, _sha_file

VOICE = re.compile(r"^(zh-CN|en-US)-[A-Za-z]+Neural$")
MAX_WAV = 3 * 1024 * 1024
SpeechFn = Callable[[str, Path, str], None]


def _check_voice(locale: str, voice: str) -> None:
    expected = "zh-CN" if locale == "zh-Hans" else "en-US" if locale == "en" else None
    if expected is None or not isinstance(voice, str) or not VOICE.fullmatch(voice) \
            or not voice.startswith(expected + "-"):
        raise ContentError("speech: voice must match zh-Hans or en locale")


def _edge_backend(text: str, output: Path, voice: str) -> None:
    """Generate MP3 using edge-tts then transcode to exact 24 kHz PCM16 WAV."""
    try:
        import edge_tts
    except ImportError as exc:
        raise ContentError("speech: install optional edge-tts package") from exc
    if shutil.which("ffmpeg") is None:
        raise ContentError("speech: ffmpeg executable is required")
    with tempfile.TemporaryDirectory(prefix="lcp-tts-") as t:
        work = Path(t)
        mp3, wav = work / "voice.mp3", work / "voice.wav"
        try:
            asyncio.run(edge_tts.Communicate(text, voice).save(str(mp3)))
            subprocess.run(
                ["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error",
                 "-y", "-i", str(mp3), "-ar", "24000", "-ac", "1",
                 "-c:a", "pcm_s16le", str(wav)],
                check=True, capture_output=True, timeout=90,
            )
        except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired,
                RuntimeError) as exc:
            raise ContentError(f"speech: backend or ffmpeg failed: {type(exc).__name__}") from exc
        output.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(wav, output)


def _validate_wav(path: Path, max_seconds: int) -> None:
    from tools import game_pack
    _asset(path.parent, path.name, max_size=MAX_WAV)
    try:
        if max_seconds == 10:
            game_pack.read_wav(path, path.name)
            return
        with wave.open(str(path), "rb") as w:
            if w.getframerate() != 24000 or w.getnchannels() != 1 or \
                    w.getsampwidth() != 2 or w.getcomptype() != "NONE" or \
                    not 1 <= w.getnframes() <= 24000 * max_seconds:
                raise ContentError("speech: WAV must be 24 kHz mono PCM16 and within limit")
            if len(w.readframes(w.getnframes())) != w.getnframes() * 2:
                raise ContentError("speech: WAV truncated")
    except (OSError, wave.Error, EOFError) as exc:
        raise ContentError("speech: invalid WAV") from exc


def _generate(text: str, target: Path, voice: str, *, synthesize: SpeechFn | None,
              replace: bool, max_seconds: int) -> str:
    if target.exists() and not replace:
        raise ContentError(f"speech: {target.name} already exists; use --replace")
    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=target.parent, prefix=".tts-") as temp:
        scratch = Path(temp) / "voice.wav"
        backend = synthesize or _edge_backend
        backend(text, scratch, voice)
        _validate_wav(scratch, max_seconds)
        shutil.copyfile(scratch, target)
    return _sha_file(target, MAX_WAV)


def narrate_book(lcir_file: Path, output_dir: Path, *, voice: str,
                 synthesize: SpeechFn | None = None,
                 test_only: bool = False, replace: bool = False) -> dict:
    """Create one per-page narration and an auditable byte manifest."""
    if synthesize is not None and not test_only:
        raise ContentError("speech: injected synthesis must be explicitly test_only")
    doc = validate_document(json.loads(Path(lcir_file).read_text(encoding="utf-8")))
    _check_voice(doc["locale"], voice)
    base = Path(output_dir)
    manifest_path = base / "audio.manifest.json"
    if manifest_path.exists() and not replace:
        raise ContentError("speech: audio manifest already exists; use --replace")
    entries = []
    for p in doc["pages"]:
        n = p["number"]
        text = p["text"]
        if len(text) > 320:
            raise ContentError(f"speech: page {n} exceeds KPB2 320-char limit")
        rel = f"audio/page_{n:02d}.wav"
        digest = _generate(text, base / rel, voice, synthesize=synthesize,
                           replace=replace, max_seconds=60)
        entries.append({
            "page": n, "text_sha256": hashlib.sha256(text.encode("utf-8")).hexdigest(),
            "path": rel, "sha256": digest,
        })
    manifest = {
        "schema_version": "BOOKAUDIO1",
        "status": "test_only" if test_only else "draft",
        "provider": "offline_synthetic_fixture" if test_only else "edge-tts",
        "voice": voice, "item_uid": doc["item_uid"],
        "content_digest": content_digest(doc), "audio": entries,
        "note": "Speech is unreviewed; verify age suitability, pronunciation and provider terms.",
    }
    write_json(manifest_path, manifest)
    return manifest


def narrate_game(lesson_file: Path, output_dir: Path, *, voice: str,
                 synthesize: SpeechFn | None = None,
                 test_only: bool = False, replace: bool = False) -> dict:
    """Create de-duplicated WAVs and the existing stage_game audio_map.json."""
    if synthesize is not None and not test_only:
        raise ContentError("speech: injected synthesis must be explicitly test_only")
    lesson = validate_lesson(json.loads(Path(lesson_file).read_text(encoding="utf-8")))
    _check_voice(lesson["locale"], voice)
    script = audio_script(lesson)
    base = Path(output_dir)
    manifest_path = base / "audio.manifest.json"
    if manifest_path.exists() and not replace:
        raise ContentError("speech: audio manifest already exists; use --replace")
    entries = []
    for clip_id, text in script["clips"].items():
        rel = f"clips/{clip_id}.wav"
        digest = _generate(text, base / rel, voice,
                           synthesize=synthesize, replace=replace, max_seconds=10)
        entries.append({"clip_key": clip_id, "spoken_text": text,
                        "path": rel, "sha256": digest})
    manifest = {
        "schema_version": "GAMEAUDIO1",
        "status": "test_only" if test_only else "draft",
        "provider": "offline_synthetic_fixture" if test_only else "edge-tts",
        "voice": voice, "lesson_uid": lesson["lesson_uid"],
        "lesson_sha256": hashlib.sha256(Path(lesson_file).read_bytes()).hexdigest(),
        "audio": entries,
        "note": "Listen to every recording; generated voices are not approved automatically.",
    }
    write_json(manifest_path, manifest)
    write_json(base / "audio_map.json", audio_map(lesson))
    return manifest
