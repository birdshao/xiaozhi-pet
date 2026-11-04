#!/usr/bin/env python3
"""Home-rail option prompts (V2-07): one short clip per rail item.

The firmware embeds firmware/components/kid_app/assets/prompts/prompt_<item>.wav
(24 kHz, mono, 16-bit PCM, <= 2 s) and plays the one the child stops on.

  tones                 write the placeholder melodies (no speech, deterministic)
  convert <dir>         turn a parent's recordings <dir>/<item>.<ext> into clips
                        with ffmpeg (mono, 24 kHz, trimmed silence, normalized)

Recordings are content: a parent listens to every clip before committing it.
"""
from __future__ import annotations

import argparse
import array
import math
import subprocess
import sys
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PROMPT_DIR = ROOT / "firmware/components/kid_app/assets/prompts"
ITEMS = ("chat", "story", "game", "feed", "sleep")  # HomeItem order
RATE = 24000

# Gentle, distinct pentatonic motifs (Hz, seconds) as placeholders.
MOTIFS = {
    "chat": [(784, 0.12), (1047, 0.18)],
    "story": [(659, 0.16), (587, 0.16), (523, 0.24)],
    "game": [(523, 0.09), (659, 0.09), (784, 0.09), (1047, 0.16)],
    "feed": [(523, 0.11), (523, 0.11), (784, 0.22)],
    "sleep": [(392, 0.26), (330, 0.26), (262, 0.40)],
}


def _write(path: Path, pcm: array.array) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(pcm.tobytes())


def tones(out_dir: Path = PROMPT_DIR) -> None:
    for item in ITEMS:
        pcm = array.array("h")
        for freq, seconds in MOTIFS[item]:
            n = int(RATE * seconds)
            for i in range(n):
                t = i / RATE
                # 8 ms attack, 40 ms release: no clicks at note edges.
                env = min(1.0, i / (0.008 * RATE), (n - i) / (0.040 * RATE))
                s = math.sin(2 * math.pi * freq * t) + 0.25 * math.sin(4 * math.pi * freq * t)
                pcm.append(int(0.22 * 32767 * env * s / 1.25))
            pcm.extend([0] * int(RATE * 0.03))  # short gap between notes
        _write(out_dir / f"prompt_{item}.wav", pcm)


def ffmpeg_command(src: Path, dst: Path, seconds: int = 2) -> list[str]:
    return ["ffmpeg", "-y", "-hide_banner", "-loglevel", "error", "-i", str(src),
            "-af", "silenceremove=start_periods=1:start_threshold=-45dB:"
                   "stop_periods=1:stop_threshold=-45dB,loudnorm=I=-24:TP=-9",
            "-ac", "1", "-ar", str(RATE), "-c:a", "pcm_s16le", "-t", str(seconds), str(dst)]


def convert(recordings: Path, out_dir: Path = PROMPT_DIR) -> None:
    for item in ITEMS:
        found = sorted(recordings.glob(f"{item}.*"))
        if not found:
            raise SystemExit(f"missing recording for {item!r} in {recordings}")
        subprocess.run(ffmpeg_command(found[0], out_dir / f"prompt_{item}.wav"), check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)
    sub.add_parser("tones")
    c = sub.add_parser("convert")
    c.add_argument("recordings", type=Path)
    args = parser.parse_args()
    if args.cmd == "tones":
        tones()
    else:
        convert(args.recordings)
    print(f"prompts written to {PROMPT_DIR}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
