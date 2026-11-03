"""Host-side self-test report: expectations from a scenario vs a serial log."""
import unittest

from tools.selftest_report import evaluate, expectations, sector_bytes

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
