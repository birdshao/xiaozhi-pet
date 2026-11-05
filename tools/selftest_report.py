#!/usr/bin/env python3
"""Host side of the scripted hardware self-test (no flashing here).

1. ``sector`` turns a PETSELFTEST1 scenario into the 4 KiB image the host
   writes to the last sector of ota_1 (erased 0xFF padding).
2. ``report`` checks a sanitized serial log against the scenario's
   ``# expect <name> <value>`` comments and prints PASS/FAIL per item.
3. ``plan`` estimates how long the host has to monitor each scenario and
   which log line ends it (PET_SELFTEST_DONE, or the alarm boot after a
   power-off), so the runner need not use fixed worst-case durations.

``--timescale N`` (all commands) rewrites the scenario's ``timescale`` step
and its ``# expect timescale`` line: ``--timescale 100`` is the real-speed
run of a time-scaled scenario (its ``twait`` steps then last as written).

Supported expectations: home_select / prompt_seq / phrase_seq (comma lists of
PET_HOME_SELECT / PET_PROMPT_PLAY indices / PET_PHRASE_PLAY phrase ids, in
order), commits (PET_EVENT_COMMITTED count), selftest_done
(1 = PET_SELFTEST_DONE seen), max_partial_ms / max_full_ms (upper bound on
PET_DIAG_EPD durations), full_refreshes / partial_refreshes (counts;
the once-a-minute status band refresh, ``src=status``, is counted apart as
status_refreshes because it depends on the wall clock, not the script),
min:<TEXT> N (TEXT appears at least N times), none:<TEXT> 0 (never appears),
timescale N (the board ran the test-only time scale N %; 100 = no scaling
was ever applied; checked on the whole log),
ledger_restored (1 = after a sleep power-off the next boot's PET_LEDGER_READY
restored exactly that power-off's ledger_gen and the last committed fed=).
Behaviour is counted after the last ``mark begin`` (warm-up excluded).
Panics, watchdog resets, a PET_SELFTEST_ABORT, a failed ledger commit
(PET_LEDGER_PERSIST_FAILED, anywhere in the log) or a log without the
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


TIMESCALE_STEP = re.compile(r"^(\s*)timescale\s+\d+\s*$", re.MULTILINE)
TIMESCALE_EXPECT = re.compile(r"^(\s*#\s*expect\s+timescale\s+)\d+\s*$", re.MULTILINE)


def apply_timescale(scenario: str, pct: int | None) -> str:
    """The scenario as run at ``pct`` % (None: as written)."""
    if pct is None:
        return scenario
    if not 10 <= pct <= 100:
        raise ValueError("timescale must be 10..100")
    if not TIMESCALE_STEP.search(scenario):
        if pct == 100:
            return scenario  # already a real-time scenario
        raise ValueError("scenario has no timescale step: it only runs at real time")
    scenario = TIMESCALE_STEP.sub(lambda m: f"{m.group(1)}timescale {pct}", scenario)
    return TIMESCALE_EXPECT.sub(lambda m: f"{m.group(1)}{pct}", scenario)


# Board timing outside the script (see MaybeRunSelfTest / pet_boot_diag).
BOOT_TO_SCRIPT_S = 13   # pet_boot_diag waits 12 s, then the hardware diag
READY_EST_S = 6         # waitidle: XiaoZhi idle (OTA check seen at ~14 s uptime)
TOGGLE_S = 1            # "pet" / "morning": Pet mode toggles in ~1 s
ALARM_BOOT_S = 15       # alarm power-on to PET_SELFTEST_NONE reason=alarm_wake
SLEEP_FLOW_MS = 10000   # sleeping picture (full refresh), 3 s window, PMIC off
MORNING_WINDOW_MS = 60000     # PowerManager::kMorningAwakeMs (scaled)
CRITICAL_DEBOUNCE_MS = 6000   # 3 battery samples, 2 s apart (scaled)
MARGIN_S = 10
DONE_MARKER = "PET_SELFTEST_DONE"
ALARM_BOOT_MARKER = "PET_SELFTEST_NONE reason=alarm_wake"


def _scaled(ms: int, pct: int) -> int:
    return max(1, ms * pct // 100)  # as SelfTestClock::Scaled


def plan(scenario: str) -> dict:
    """How long one run takes: boot, the script (``twait`` at its scale)
    and, when it powers off, the alarm and the boot after it.

    ``expect_s`` is the estimate up to the end marker ``until``;
    ``monitor_s`` the upper bound a runner without early stop should use
    (the whole script plus a margin)."""
    steps = [line.split() for line in scenario.splitlines()
             if line.strip() and not line.lstrip().startswith("#")]
    keys_after = [any(w[0] in ("tap", "hold", "combo") for w in steps[i + 1:])
                  for i in range(len(steps))]
    pct, alarm_s, t_ms = 100, 0, 0
    off_ms = None  # first power-off, from the script start
    for i, words in enumerate(steps):
        op, args = words[0], [int(w) for w in words[1:] if w.isdigit()]
        if op == "timescale":
            pct = args[0]
        elif op == "wait":
            t_ms += args[0]
        elif op == "twait":
            t_ms += _scaled(args[0], pct)
        elif op == "envelope":  # V4-08: the step lasts as long as the sound
            t_ms += args[0]
        elif op == "waitidle":
            t_ms += min(args[0], READY_EST_S * 1000)
        elif op == "tap":
            if words[1] == "pwr" and off_ms is None:
                off_ms = t_ms + 80 + SLEEP_FLOW_MS
            t_ms += 80 + (args[0] if args else 1000)
        elif op in ("hold", "combo"):
            t_ms += args[0] + (args[1] if len(args) > 1 else 1000)
        elif op in ("pet", "morning"):
            t_ms += TOGGLE_S * 1000
            # A morning nobody touches ends in a power-off after its window.
            if op == "morning" and not keys_after[i] and off_ms is None:
                off_ms = t_ms + _scaled(MORNING_WINDOW_MS, pct) + SLEEP_FLOW_MS
        elif op == "alarm":
            alarm_s = args[0]
        elif op == "battery" and len(words) == 3 and words[2] == "0" and args[0] < 8:
            if off_ms is None:  # critical on battery: safe shutdown
                off_ms = t_ms + _scaled(CRITICAL_DEBOUNCE_MS, pct) + SLEEP_FLOW_MS
    script_s = -(-t_ms // 1000)
    power_off = off_ms is not None
    monitor_s = BOOT_TO_SCRIPT_S + script_s + MARGIN_S
    if power_off:
        monitor_s += alarm_s + ALARM_BOOT_S
        expect_s = BOOT_TO_SCRIPT_S + -(-off_ms // 1000) + alarm_s + ALARM_BOOT_S
    else:
        expect_s = BOOT_TO_SCRIPT_S + script_s
    return {"timescale": pct, "script_s": script_s, "power_off": power_off,
            "alarm_s": alarm_s if power_off else 0, "expect_s": expect_s,
            "monitor_s": monitor_s,
            "until": ALARM_BOOT_MARKER if power_off else DONE_MARKER}


def read_log(path: Path) -> str:
    """Decode a trial log; Windows PowerShell 5 Tee-Object writes UTF-16."""
    raw = path.read_bytes()
    if raw.startswith((b"\xff\xfe", b"\xfe\xff")):
        return raw.decode("utf-16", errors="replace")
    return raw.decode("utf-8-sig", errors="replace")


CRASH_TEXT = re.compile(r"Guru Meditation|abort\(\) was called|Task watchdog got triggered")


def _ledger_restored(body: str) -> int:
    """1 when a power-off's durable state is exactly what the next boot loaded:
    same ledger generation, and the feed count of the last commit before it."""
    for off in re.finditer(r"PET_SLEEP_POWEROFF [^\n]*ledger_gen=(\d+)", body):
        gen = int(off.group(1))
        ready = re.search(r"PET_LEDGER_READY ok=1 [^\n]*? gen=(\d+) fed=(\d+)",
                          body[off.end():])
        fed = re.findall(r"PET_EVENT_COMMITTED [^\n]*fed=(\d+)", body[:off.start()])
        if (gen and ready and fed and int(ready.group(1)) == gen
                and int(ready.group(2)) == int(fed[-1])):
            return 1
    return 0


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
        "ledger_restored": _ledger_restored(body),
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
        # Test-only time scale steps (not the reset when the script ends).
        "timescale": [int(p) for p in
                      re.findall(r"PET_SELFTEST_TIMESCALE pct=(\d+)(?!\d| reason=end)", log)],
        "idle_ms": [int(ms) for ms in re.findall(r"PET_SELFTEST_IDLE ready=1 ms=(\d+)", log)],
        "failed_refreshes": sum(1 for _, ok, _, _ in epd if ok != "1"),
        "summaries": len(summaries),
        "body": body,
        "panic": panic,
        "watchdog": wdt,
        # A durable pet commit that failed in NVS: never expected in a trial.
        "persist_failures": len(re.findall(r"PET_LEDGER_PERSIST_FAILED", log)),
    }


def evaluate(scenario: str, log: str) -> dict:
    exp = expectations(scenario)
    obs = _observe(log)
    checks: dict[str, bool] = {}
    for name, want in exp.items():
        if name in ("home_select", "prompt_seq", "phrase_seq"):
            checks[name] = obs[name] == want
        elif name in ("commits", "selftest_done", "ledger_restored"):
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
        elif name == "timescale":
            scales = obs["timescale"]
            checks[name] = (all(p == 100 for p in scales) if int(want) == 100
                            else bool(scales) and scales[-1] == int(want))
        else:
            checks[name] = False  # unknown expectation is a scenario bug
    # No monitor SUMMARY means the log is incomplete: fail closed.
    checks["summary"] = obs["summaries"] > 0
    checks["panic"] = obs["panic"] == 0
    checks["watchdog"] = obs["watchdog"] == 0
    checks["ledger_persist"] = obs["persist_failures"] == 0
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
    p = sub.add_parser("plan", help="monitor seconds and end marker per scenario")
    p.add_argument("scenarios", type=Path, nargs="+")
    p.add_argument("--json", action="store_true")
    for cmd in (s, r, p):
        cmd.add_argument("--timescale", type=int, default=None,
                         help="run a time-scaled scenario at this %% (100 = real speed)")
    args = parser.parse_args()

    if args.cmd == "plan":
        rows = {path.stem: plan(apply_timescale(path.read_text(encoding="ascii"),
                                                args.timescale))
                for path in args.scenarios}
        if args.json:
            print(json.dumps(rows, indent=1))
            return 0
        for name, row in rows.items():
            print(f"{name:16s} timescale={row['timescale']:3d} script_s={row['script_s']:4d} "
                  f"expect_s={row['expect_s']:4d} monitor_s={row['monitor_s']:4d} "
                  f"until={row['until']}")
        print(f"total expect_s={sum(row['expect_s'] for row in rows.values())} "
              f"monitor_s={sum(row['monitor_s'] for row in rows.values())}")
        return 0
    scenario = apply_timescale(args.scenario.read_text(encoding="ascii"), args.timescale)
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
                                                        "status_refreshes", "timescale",
                                                        "idle_ms")}))
    print("RESULT:", "PASS" if report["passed"] else "FAIL")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
