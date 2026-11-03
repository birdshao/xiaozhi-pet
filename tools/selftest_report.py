#!/usr/bin/env python3
"""Host side of the scripted hardware self-test (no flashing here).

1. ``sector`` turns a PETSELFTEST1 scenario into the 4 KiB image the host
   writes to the last sector of ota_1 (erased 0xFF padding).
2. ``report`` checks a sanitized serial log against the scenario's
   ``# expect <name> <value>`` comments and prints PASS/FAIL per item.

Supported expectations: home_select (comma list of PET_HOME_SELECT
indices, in order), commits (PET_EVENT_COMMITTED count), selftest_done
(1 = PET_SELFTEST_DONE seen), max_partial_ms / max_full_ms (upper bound on
PET_DIAG_EPD durations), full_refreshes / partial_refreshes (counts).
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
        m = re.match(r"\s*#\s*expect\s+(\w+)\s+(\S+)\s*$", line)
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


CRASH_TEXT = re.compile(r"Guru Meditation|abort\(\) was called|Task watchdog got triggered")


def _observe(log: str) -> dict:
    # Health is judged on the whole log; behaviour only after the last
    # "mark begin" so warm-up/normalization steps are not counted.
    marks = [m.end() for m in re.finditer(r"PET_SELFTEST_MARK name=begin", log)]
    body = log[marks[-1]:] if marks else log
    epd = re.findall(r"PET_DIAG_EPD .*?mode=(\w+) ok=(\d) ms=(\d+)", body)
    summaries = re.findall(r"^SUMMARY .*$", log, re.MULTILINE)
    panic = wdt = 0
    for line in summaries:
        counts = dict(re.findall(r"(\w+)=(\d+)", line))
        panic += int(counts.get("panic", 0))
        wdt += int(counts.get("wdt", 0))
    panic += len(CRASH_TEXT.findall(log))
    return {
        "home_select": ",".join(re.findall(r"PET_HOME_SELECT index=(\d+)", body)),
        "commits": len(re.findall(r"PET_EVENT_COMMITTED", body)),
        "selftest_done": 1 if ("PET_SELFTEST_DONE" in log and
                               "PET_SELFTEST_ABORT" not in log) else 0,
        "partial_ms": [int(ms) for mode, ok, ms in epd if mode == "partial" and ok == "1"],
        "full_ms": [int(ms) for mode, ok, ms in epd if mode == "full" and ok == "1"],
        "failed_refreshes": sum(1 for _, ok, _ in epd if ok != "1"),
        "summaries": len(summaries),
        "panic": panic,
        "watchdog": wdt,
    }


def evaluate(scenario: str, log: str) -> dict:
    exp = expectations(scenario)
    obs = _observe(log)
    checks: dict[str, bool] = {}
    for name, want in exp.items():
        if name == "home_select":
            checks[name] = obs["home_select"] == want
        elif name in ("commits", "selftest_done"):
            checks[name] = obs[name] == int(want)
        elif name == "max_partial_ms":
            checks[name] = bool(obs["partial_ms"]) and max(obs["partial_ms"]) <= int(want)
        elif name == "max_full_ms":
            checks[name] = bool(obs["full_ms"]) and max(obs["full_ms"]) <= int(want)
        elif name == "partial_refreshes":
            checks[name] = len(obs["partial_ms"]) == int(want)
        elif name == "full_refreshes":
            checks[name] = len(obs["full_ms"]) == int(want)
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
    report = evaluate(scenario, args.log.read_text(encoding="utf-8", errors="replace"))
    for name, ok in report["checks"].items():
        want = report["expected"].get(name, "")
        print(f"{'PASS' if ok else 'FAIL'}  {name:18s} expected={want}")
    obs = report["observed"]
    print("observed:", json.dumps({k: obs[k] for k in ("home_select", "commits",
                                                        "partial_ms", "full_ms")}))
    print("RESULT:", "PASS" if report["passed"] else "FAIL")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
