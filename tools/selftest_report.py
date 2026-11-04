#!/usr/bin/env python3
"""Host side of the scripted hardware self-test (no flashing here).

1. ``sector`` turns a PETSELFTEST1 scenario into the 4 KiB image the host
   writes to the last sector of ota_1 (erased 0xFF padding).
2. ``report`` checks a sanitized serial log against the scenario's
   ``# expect <name> <value>`` comments and prints PASS/FAIL per item.

Supported expectations: home_select / prompt_seq / phrase_seq (comma lists of
PET_HOME_SELECT / PET_PROMPT_PLAY indices / PET_PHRASE_PLAY phrase ids, in
order), commits (PET_EVENT_COMMITTED count), selftest_done
(1 = PET_SELFTEST_DONE seen), max_partial_ms / max_full_ms (upper bound on
PET_DIAG_EPD durations), full_refreshes / partial_refreshes (counts;
the once-a-minute status band refresh, ``src=status``, is counted apart as
status_refreshes because it depends on the wall clock, not the script),
min:<TEXT> N (TEXT appears at least N times), none:<TEXT> 0 (never appears).
Behaviour is counted after the last ``mark begin`` (warm-up excluded).
Panics, watchdog resets, a PET_SELFTEST_ABORT, or a log without the
monitor's SUMMARY line always fail.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

SECTOR = 4096
HEADER = "PETSELFTEST1"


def expectations(scenario: str) -> dict[str, str]:
    found: dict[str, str] = {}
    for line in scenario.splitlines():
        m = re.match(r"\s*#\s*expect\s+(\S+)\s+(\S+)\s*$", line)
        if m:
            found[m.group(1)] = m.group(2)
    return found


def sector_bytes(scenario: str) -> bytes:
    text = scenario.encode("ascii")
    if not scenario.lstrip().startswith(HEADER):
        raise ValueError("scenario must start with " + HEADER)
    if len(text) > SECTOR or b"\x00" in text or b"\xff" in text:
        raise ValueError("scenario must be plain ASCII and fit in 4096 bytes")
    return text + b"\xff" * (SECTOR - len(text))


def read_log(path: Path) -> str:
    """Decode a trial log; Windows PowerShell 5 Tee-Object writes UTF-16."""
    raw = path.read_bytes()
    if raw.startswith((b"\xff\xfe", b"\xfe\xff")):
        return raw.decode("utf-16", errors="replace")
    return raw.decode("utf-8-sig", errors="replace")


CRASH_TEXT = re.compile(r"Guru Meditation|abort\(\) was called|Task watchdog got triggered")


def _observe(log: str) -> dict:
    # Health is judged on the whole log; behaviour only after the last
    # "mark begin" so warm-up/normalization steps are not counted.
    marks = [m.end() for m in re.finditer(r"PET_SELFTEST_MARK name=begin", log)]
    body = log[marks[-1]:] if marks else log
    epd = re.findall(r"PET_DIAG_EPD .*?mode=(\w+) ok=(\d) ms=(\d+)(?:[^\n]*?src=(\w+))?",
                     body)
    summaries = re.findall(r"^SUMMARY .*$", log, re.MULTILINE)
    panic = wdt = 0
    for line in summaries:
        counts = dict(re.findall(r"(\w+)=(\d+)", line))
        panic += int(counts.get("panic", 0))
        wdt += int(counts.get("wdt", 0))
    panic += len(CRASH_TEXT.findall(log))
    return {
        "home_select": ",".join(re.findall(r"PET_HOME_SELECT index=(\d+)", body)),
        "prompt_seq": ",".join(re.findall(r"PET_PROMPT_PLAY index=(\d+)", body)),
        "phrase_seq": ",".join(re.findall(r"PET_PHRASE_PLAY id=(\d+)", body)),
        "commits": len(re.findall(r"PET_EVENT_COMMITTED", body)),
        "selftest_done": 1 if ("PET_SELFTEST_DONE" in log and
                               "PET_SELFTEST_ABORT" not in log) else 0,
        # Durations include every refresh; counts leave out status-band ones.
        "partial_ms": [int(ms) for mode, ok, ms, _ in epd if mode == "partial" and ok == "1"],
        "full_ms": [int(ms) for mode, ok, ms, _ in epd if mode == "full" and ok == "1"],
        "partial_count": sum(1 for mode, ok, _, src in epd
                             if mode == "partial" and ok == "1" and src != "status"),
        "full_count": sum(1 for mode, ok, _, src in epd
                          if mode == "full" and ok == "1" and src != "status"),
        "status_refreshes": sum(1 for _, ok, _, src in epd if ok == "1" and src == "status"),
        "failed_refreshes": sum(1 for _, ok, _, _ in epd if ok != "1"),
        "summaries": len(summaries),
        "body": body,
        "panic": panic,
        "watchdog": wdt,
    }


def evaluate(scenario: str, log: str) -> dict:
    exp = expectations(scenario)
    obs = _observe(log)
    checks: dict[str, bool] = {}
    for name, want in exp.items():
        if name in ("home_select", "prompt_seq", "phrase_seq"):
            checks[name] = obs[name] == want
        elif name in ("commits", "selftest_done"):
            checks[name] = obs[name] == int(want)
        elif name == "max_partial_ms":
            checks[name] = bool(obs["partial_ms"]) and max(obs["partial_ms"]) <= int(want)
        elif name == "max_full_ms":
            checks[name] = bool(obs["full_ms"]) and max(obs["full_ms"]) <= int(want)
        elif name == "partial_refreshes":
            checks[name] = obs["partial_count"] == int(want)
        elif name == "full_refreshes":
            checks[name] = obs["full_count"] == int(want)
        elif name.startswith("min:"):
            checks[name] = obs["body"].count(name[4:]) >= int(want)
        elif name.startswith("none:"):
            checks[name] = name[5:] not in obs["body"]
        else:
            checks[name] = False  # unknown expectation is a scenario bug
    # No monitor SUMMARY means the log is incomplete: fail closed.
    checks["summary"] = obs["summaries"] > 0
    checks["panic"] = obs["panic"] == 0
    checks["watchdog"] = obs["watchdog"] == 0
    checks["refresh_errors"] = obs["failed_refreshes"] == 0
    failed = [name for name, ok in checks.items() if not ok]
    return {"passed": not failed, "failed": failed, "checks": checks,
            "expected": exp, "observed": obs}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("sector", help="write the 4 KiB sector image")
    s.add_argument("scenario", type=Path)
    s.add_argument("output", type=Path)
    r = sub.add_parser("report", help="judge a sanitized serial log")
    r.add_argument("scenario", type=Path)
    r.add_argument("log", type=Path)
    args = parser.parse_args()

    scenario = args.scenario.read_text(encoding="ascii")
    if args.cmd == "sector":
        args.output.write_bytes(sector_bytes(scenario))
        print(f"sector image: {args.output} ({SECTOR} bytes)")
        return 0
    report = evaluate(scenario, read_log(args.log))
    for name, ok in report["checks"].items():
        want = report["expected"].get(name, "")
        print(f"{'PASS' if ok else 'FAIL'}  {name:18s} expected={want}")
    obs = report["observed"]
    print("observed:", json.dumps({k: obs[k] for k in ("home_select", "prompt_seq",
                                                        "phrase_seq", "commits",
                                                        "partial_ms", "full_ms",
                                                        "status_refreshes")}))
    print("RESULT:", "PASS" if report["passed"] else "FAIL")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
