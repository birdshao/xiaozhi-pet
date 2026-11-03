"""Host-side self-test report: expectations from a scenario vs a serial log."""
import tempfile
import unittest
from pathlib import Path

from tools.selftest_report import evaluate, expectations, read_log, sector_bytes

SCENARIO = """PETSELFTEST1
# expect home_select 0,1,2
# expect commits 2
# expect selftest_done 1
# expect max_partial_ms 2000
wait 100
tap up
end
"""

GOOD_LOG = """
[ 12s] I (12000) b: PET_SELFTEST_START steps=2 total_ms=1180
[ 20s] I (20000) b: PET_HOME_SELECT index=0
[ 21s] I (21000) b: PET_DIAG_EPD kind=0 page=1 sel=0 mode=partial ok=1 ms=640 n=2
[ 22s] I (22000) b: PET_HOME_SELECT index=1
[ 23s] I (23000) b: PET_HOME_SELECT index=2
[ 24s] I (24000) b: PET_DIAG_EPD kind=0 page=1 sel=2 mode=full ok=1 ms=4310 n=3
[ 25s] I (25000) b: PET_EVENT_COMMITTED id_class=local
[ 26s] I (26000) b: PET_EVENT_COMMITTED id_class=local
[ 30s] I (30000) b: PET_SELFTEST_DONE steps=2
SUMMARY lines=99 opens=1 ota0=0 ota1=1 panic=0 pet_boot=2 pet_event=2 pet_runtime_on=1 wdt=0
"""


class SelfTestReportTests(unittest.TestCase):
    def test_expectations_are_read_from_comments(self):
        exp = expectations(SCENARIO)
        self.assertEqual(exp["home_select"], "0,1,2")
        self.assertEqual(exp["commits"], "2")
        self.assertEqual(len(exp), 4)

    def test_passing_log(self):
        report = evaluate(SCENARIO, GOOD_LOG)
        self.assertTrue(report["passed"], report)
        self.assertEqual(report["observed"]["home_select"], "0,1,2")
        self.assertEqual(report["observed"]["partial_ms"], [640])
        self.assertEqual(report["observed"]["full_ms"], [4310])

    def test_failures_are_named(self):
        bad = GOOD_LOG.replace("index=1", "index=3").replace("ms=640", "ms=2600")
        bad = bad.replace("PET_SELFTEST_DONE", "PET_SELFTEST_ABORT")
        report = evaluate(SCENARIO, bad)
        self.assertFalse(report["passed"])
        self.assertEqual(sorted(report["failed"]), ["home_select", "max_partial_ms", "selftest_done"])

    def test_panic_or_watchdog_always_fails(self):
        report = evaluate(SCENARIO, GOOD_LOG.replace("panic=0", "panic=1"))
        self.assertIn("panic", report["failed"])
        report = evaluate(SCENARIO, GOOD_LOG.replace("wdt=0", "wdt=2"))
        self.assertIn("watchdog", report["failed"])

    def test_missing_summary_or_crash_text_fails_closed(self):
        no_summary = "\n".join(l for l in GOOD_LOG.splitlines() if not l.startswith("SUMMARY"))
        self.assertIn("summary", evaluate(SCENARIO, no_summary)["failed"])
        crashed = GOOD_LOG + "Guru Meditation Error: Core  0 panic'ed\n"
        self.assertIn("panic", evaluate(SCENARIO, crashed)["failed"])
        # Several monitor sessions: every SUMMARY counts.
        two = GOOD_LOG + "SUMMARY lines=3 panic=0 wdt=1\n"
        self.assertIn("watchdog", evaluate(SCENARIO, two)["failed"])

    def test_counting_starts_at_the_begin_mark(self):
        warmup = ("[ 15s] I (1) b: PET_HOME_SELECT index=4\n"
                  "[ 15s] I (1) b: PET_EVENT_COMMITTED id_class=local\n"
                  "[ 16s] I (1) b: PET_DIAG_EPD kind=1 page=1 sel=1 mode=full ok=1 ms=4300 n=1\n"
                  "[ 17s] I (1) b: PET_SELFTEST_MARK name=begin\n")
        lines = GOOD_LOG.splitlines()
        log = "\n".join(lines[:2] + [warmup] + lines[2:])
        report = evaluate(SCENARIO, log)
        self.assertTrue(report["passed"], report)
        self.assertEqual(report["observed"]["commits"], 2)

    def test_abort_fails_even_if_done_follows(self):
        log = GOOD_LOG + "W (1) b: PET_SELFTEST_ABORT reason=pet_toggle step=1\n"
        self.assertIn("selftest_done", evaluate(SCENARIO, log)["failed"])

    def test_log_encodings_from_windows_powershell(self):
        # Windows PowerShell 5 Tee-Object writes UTF-16LE with a BOM; the
        # 2026-11-03 V2 trial log looked empty until decoded correctly.
        for encoding in ("utf-16", "utf-8-sig", "utf-8"):
            with tempfile.TemporaryDirectory() as tmp:
                path = Path(tmp) / "trial.log"
                path.write_bytes(GOOD_LOG.encode(encoding))
                self.assertTrue(evaluate(SCENARIO, read_log(path))["passed"], encoding)

    def test_sector_is_padded_with_erased_bytes(self):
        data = sector_bytes(SCENARIO)
        self.assertEqual(len(data), 4096)
        self.assertTrue(data.startswith(b"PETSELFTEST1\n"))
        self.assertEqual(data[len(SCENARIO.encode()):], b"\xff" * (4096 - len(SCENARIO.encode())))
        with self.assertRaises(ValueError):
            sector_bytes("PETSELFTEST1\n" + "#" * 5000 + "\nend\n")
        with self.assertRaises(ValueError):
            sector_bytes("wait 1\nend\n")  # no header


if __name__ == "__main__":
    unittest.main()
