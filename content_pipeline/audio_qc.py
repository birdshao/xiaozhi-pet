"""Conservative, offline technical quality checks for 24kHz mono PCM16 speech.

Unusable clips (all-silent, near-silent below about -60 dBFS RMS, or mostly
silence) are hard errors. Warnings for low level,
clipping, DC and long trim regions require an adult's listening decision;
this is not pronunciation, pedagogy, language or child-safety approval.
"""
from __future__ import annotations

import array
import math
from pathlib import Path
import sys
import wave

from content_pipeline.schema import ContentError


NEAR_SILENT_RMS_DBFS = -60.0


def analyze_wav(path: Path, *, max_seconds: int = 60) -> dict:
    """Return pure-data QA metrics from the *actual* WAV PCM samples."""
    file = Path(path)
    try:
        with wave.open(str(file), "rb") as wav:
            if wav.getnchannels() != 1 or wav.getsampwidth() != 2 or \
                    wav.getframerate() != 24000 or wav.getcomptype() != "NONE":
                raise ContentError("audio QC: expected 24kHz mono signed PCM16")
            frames = wav.getnframes()
            if not 1 <= frames <= max_seconds * 24000:
                raise ContentError("audio QC: duration outside bounds")
            payload = wav.readframes(frames)
            if len(payload) != frames * 2:
                raise ContentError("audio QC: truncated WAV data")
    except (wave.Error, OSError, EOFError) as exc:
        raise ContentError("audio QC: cannot read WAV") from exc

    samples = array.array("h")
    samples.frombytes(payload)
    if sys.byteorder != "little":
        samples.byteswap()
    if not samples:
        raise ContentError("audio QC: no samples")
    peak = max(abs(int(sample)) for sample in samples)
    if peak == 0:
        raise ContentError("audio QC: all-silent clip")
    count = len(samples)
    norm = 32768.0
    rms = math.sqrt(sum(int(s) * int(s) for s in samples) / count)
    average = sum(int(s) for s in samples) / count
    clipping_samples = sum(abs(int(s)) >= 32760 for s in samples)
    # Silence is defined conservatively as below ~-40 dBFS (1% amplitude).
    quiet = 328
    leading = 0
    for sample in samples:
        if abs(int(sample)) >= quiet:
            break
        leading += 1
    trailing = 0
    for sample in reversed(samples):
        if abs(int(sample)) >= quiet:
            break
        trailing += 1
    duration_ms = int(round(count * 1000 / 24000))
    peak_dbfs = round(20 * math.log10(peak / norm), 1)
    rms_dbfs = round(20 * math.log10(max(rms, 1e-8) / norm), 1)
    quiet_fraction = round(sum(abs(int(s)) < quiet for s in samples) / count, 3)
    clipping_fraction = round(clipping_samples / count, 4)
    leading_ms = int(round(leading * 1000 / 24000))
    trailing_ms = int(round(trailing * 1000 / 24000))
    warnings = []
    if rms_dbfs < -37:
        warnings.append("low_rms_level")
    if peak_dbfs < -27:
        warnings.append("low_peak_level")
    if clipping_fraction > 0.001:
        warnings.append("possible_clipping")
    if abs(average / norm) > 0.04:
        warnings.append("dc_offset")
    if leading_ms > 550:
        warnings.append("long_leading_silence")
    if trailing_ms > 700:
        warnings.append("long_trailing_silence")
    if quiet_fraction > 0.85:
        warnings.append("mostly_silence")
    if rms_dbfs < NEAR_SILENT_RMS_DBFS or "mostly_silence" in warnings:
        raise ContentError(
            f"audio QC: near-silent clip (rms {rms_dbfs} dBFS, "
            f"{quiet_fraction:.0%} below -40 dBFS)")
    return {
        "sample_rate_hz": 24000,
        "channels": 1,
        "bits_per_sample": 16,
        "duration_ms": duration_ms,
        "peak_dbfs": peak_dbfs,
        "rms_dbfs": rms_dbfs,
        "quiet_fraction": quiet_fraction,
        "clipping_fraction": clipping_fraction,
        "leading_silence_ms": leading_ms,
        "trailing_silence_ms": trailing_ms,
        "warnings": warnings,
    }
