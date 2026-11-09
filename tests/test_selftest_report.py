"""Host-side self-test report: expectations from a scenario vs a serial log."""
import contextlib
import io
import re
import tempfile
import unittest
from pathlib import Path

from tools.selftest_report import (MAX_LINE, apply_timescale, evaluate, expectations,
                                   long_lines, main, plan, read_log, run_order, sector_bytes)

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

    def test_failed_ledger_commit_always_fails(self):
        self.assertNotIn("ledger_persist", evaluate(SCENARIO, GOOD_LOG)["failed"])
        # Even before "mark begin" (e.g. the normalization's StopStory).
        failed = ("W (9000) kid_pet_atomic: PET_LEDGER_PERSIST_FAILED "
                  "err=ESP_ERR_NVS_NOT_ENOUGH_SPACE kind=1 attempt=1 dropped=0\n")
        report = evaluate(SCENARIO, failed + GOOD_LOG)
        self.assertFalse(report["passed"])
        self.assertIn("ledger_persist", report["failed"])
        self.assertEqual(report["observed"]["persist_failures"], 1)

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

    def test_status_band_refreshes_are_counted_apart(self):
        # 2026-11-04: v2_home_rail saw 19 partials, one being the status
        # band's once-a-minute refresh; scenario counts must not depend on
        # the wall clock, timing limits still cover every refresh.
        status = ("[ 73s] I (73232) b: PET_DIAG_EPD kind=0 page=1 sel=1 mode=partial ok=1 "
                  "ms=1800 n=9 pose=0 src=status rail=4 pet=0\n")
        lines = GOOD_LOG.splitlines()
        log = "\n".join(lines[:4] + [status] + lines[4:])
        report = evaluate(SCENARIO.replace("# expect commits 2\n",
                                           "# expect commits 2\n# expect partial_refreshes 1\n"
                                           "# expect full_refreshes 1\n"), log)
        self.assertTrue(report["passed"], report)
        self.assertEqual(report["observed"]["partial_ms"], [640, 1800])
        # V8: a home-look refresh (needs drift with the clock) is counted the same way.
        look_log = log.replace("src=status", "src=look")
        look_report = evaluate(SCENARIO.replace("# expect commits 2\n",
                                                "# expect commits 2\n# expect partial_refreshes 1\n"),
                               look_log)
        self.assertTrue(look_report["passed"], look_report)
        self.assertEqual(look_report["observed"]["status_refreshes"], 1)
        too_slow = log.replace("ms=1800", "ms=2600")
        self.assertIn("max_partial_ms", evaluate(SCENARIO, too_slow)["failed"])

    def test_marker_expectations(self):
        scenario = ("PETSELFTEST1\n# expect min:PET_SLEEP_SHOWN 2\n"
                    "# expect min:woke_by_alarm=1 1\n"
                    "# expect none:PET_SLEEP_POWEROFF_RETURNED 0\nend\n")
        log = ("I: PET_SELFTEST_MARK name=begin\nI: PET_SLEEP_SHOWN\nI: PET_SLEEP_SHOWN\n"
               "I: PET_DIAG_HEARTBEAT woke_by_alarm=1 rtc_synced=1\n"
               "SUMMARY lines=3 panic=0 wdt=0\n")
        self.assertTrue(evaluate(scenario, log)["passed"])
        self.assertEqual(evaluate(scenario, log.replace("I: PET_SLEEP_SHOWN\n", "", 1))["failed"],
                         ["min:PET_SLEEP_SHOWN"])
        bad = log + "W: PET_SLEEP_POWEROFF_RETURNED\n"
        self.assertEqual(evaluate(scenario, bad)["failed"], ["none:PET_SLEEP_POWEROFF_RETURNED"])
        # Markers before "mark begin" are warm-up and do not count.
        early = "I: PET_SLEEP_SHOWN\nI: PET_SLEEP_SHOWN\n" + log.replace("I: PET_SLEEP_SHOWN\n", "")
        self.assertIn("min:PET_SLEEP_SHOWN", evaluate(scenario, early)["failed"])

    def test_prompt_sequence(self):
        scenario = "PETSELFTEST1\n# expect prompt_seq 2,4\nend\n"
        log = ("I: PET_SELFTEST_MARK name=begin\n"
               "I: PET_PROMPT_PLAY index=2 token=1 samples=7000\n"
               "I: PET_PROMPT_PLAY index=4 token=2 samples=9000\n"
               "SUMMARY lines=3 panic=0 wdt=0\n")
        report = evaluate(scenario, log)
        self.assertTrue(report["passed"], report)
        self.assertEqual(report["observed"]["prompt_seq"], "2,4")
        self.assertIn("prompt_seq", evaluate(scenario, log.replace("index=4", "index=3"))["failed"])

    def test_phrase_sequence(self):
        scenario = "PETSELFTEST1\n# expect phrase_seq 211,272\nend\n"
        log = ("I: PET_PHRASE_PLAY id=999 ctx=greeting src=chime token=1 samples=9\n"
               "I: PET_SELFTEST_MARK name=begin\n"
               "I: PET_PROMPT_PLAY index=2 token=1 samples=7000 src=embedded\n"
               "I: PET_PHRASE_PLAY id=211 ctx=greeting src=chime token=2 samples=9120\n"
               "I: PET_PHRASE_PLAY id=272 ctx=giggle src=pack token=3 samples=24000\n"
               "SUMMARY lines=5 panic=0 wdt=0\n")
        report = evaluate(scenario, log)
        self.assertTrue(report["passed"], report)
        self.assertEqual(report["observed"]["phrase_seq"], "211,272")
        self.assertEqual(report["observed"]["prompt_seq"], "2")
        self.assertIn("phrase_seq", evaluate(scenario, log.replace("id=272", "id=271"))["failed"])

    def test_ledger_restored_across_a_power_off(self):
        scenario = "PETSELFTEST1\n# expect ledger_restored 1\n# expect commits 1\nend\n"
        log = ("I: PET_LEDGER_READY ok=1 migration=migrated failure=none flags=0x03 "
               "gen=7 fed=4 mode=0 mood=0 today=0 feeds=0 temperament=1\n"
               "I: PET_SELFTEST_MARK name=begin\n"
               "I: PET_EVENT_COMMITTED id_class=local fed=5\n"
               "I: PET_SLEEP_POWEROFF settled=1 alarm_s=45 armed=1 wake=selftest battery=0 "
               "ledger_gen=8\n"
               "I: PET_FIRMWARE_BOOT_MARKER phase=board_init\n"
               "I: PET_LEDGER_READY ok=1 migration=already failure=none flags=0x03 "
               "gen=8 fed=5 mode=1 mood=1 today=20396 feeds=1 temperament=1\n"
               "SUMMARY lines=6 panic=0 wdt=0\n")
        report = evaluate(scenario, log)
        self.assertTrue(report["passed"], report)
        # A lost commit (older generation or feed count after the boot) fails.
        for bad in (log.replace("gen=8 fed=5", "gen=7 fed=5"),
                    log.replace("gen=8 fed=5", "gen=8 fed=4"),
                    log.replace("ledger_gen=8", "ledger_gen=0"),
                    log.replace("READY ok=1 migration=already", "READY ok=0 migration=failed")):
            self.assertIn("ledger_restored", evaluate(scenario, bad)["failed"])
        # Without a power-off and a later boot there is nothing to compare.
        head = log[:log.index("I: PET_SLEEP_POWEROFF")] + "SUMMARY lines=3 panic=0 wdt=0\n"
        self.assertIn("ledger_restored", evaluate(scenario, head)["failed"])

    def test_sector_is_padded_with_erased_bytes(self):
        data = sector_bytes(SCENARIO)
        self.assertEqual(len(data), 4096)
        self.assertTrue(data.startswith(b"PETSELFTEST1\n"))
        self.assertEqual(data[len(SCENARIO.encode()):], b"\xff" * (4096 - len(SCENARIO.encode())))
        with self.assertRaises(ValueError):
            sector_bytes("PETSELFTEST1\n" + "#" * 5000 + "\nend\n")
        with self.assertRaises(ValueError):
            sector_bytes("wait 1\nend\n")  # no header

    def test_committed_scenarios_that_power_off_arm_an_alarm_first(self):
        # An unattended trial that powers off without an RTC alarm leaves
        # the board off: the host can no longer restore the boot selector.
        scenarios = sorted((Path(__file__).resolve().parents[1] / "tools/selftest").glob("*.pettest"))
        self.assertTrue(scenarios)
        for path in scenarios:
            text = path.read_text(encoding="ascii")
            self.assertEqual(len(sector_bytes(text)), 4096, path.name)
            steps = [line.split() for line in text.splitlines()
                     if line.strip() and not line.lstrip().startswith("#")]
            self.assertEqual(steps[0], ["PETSELFTEST1"], path.name)
            self.assertEqual(steps[-1], ["end"], path.name)
            armed = False
            for words in steps:
                armed = armed or words[0] == "alarm"
                # V3-05: a critical battery level on battery (< 8 %) powers off.
                critical = (words[0] == "battery" and len(words) == 3 and words[2] == "0"
                            and int(words[1]) < 8)
                if words[0] == "morning" or words[:2] == ["tap", "pwr"] or critical:
                    self.assertTrue(armed, f"{path.name}: {' '.join(words)} before an alarm step")

    def test_committed_scenarios_use_known_ops_and_say_their_time_scale(self):
        # Mirrors SelfTestScript::Parse (test_selftest_script.cc covers it).
        known = {"wait", "pet", "tap", "hold", "combo", "mark", "alarm", "volume", "gesture",
                 "morning", "voice", "battery", "temp", "book", "game", "timescale", "twait",
                 "waitidle", "micgame", "envelope", "vcmd", "phase", "end"}
        root = Path(__file__).resolve().parents[1] / "tools/selftest"
        scaled = []
        for path in sorted(root.glob("*.pettest")):
            text = path.read_text(encoding="ascii")
            steps = [line.split() for line in text.splitlines()[1:]
                     if line.strip() and not line.lstrip().startswith("#")]
            self.assertLessEqual({w[0] for w in steps}, known, path.name)
            self.assertLessEqual(len(steps) - 1, 64, path.name)  # kMaxSteps, "end" aside
            # Every scenario starts as soon as XiaoZhi is ready, not after
            # a fixed boot wait.
            self.assertIn(["waitidle", "60000"], steps, path.name)
            self.assertNotIn(["wait", "20000"], steps, path.name)
            scales = [int(w[1]) for w in steps if w[0] == "timescale"]
            if scales:
                scaled.append(path.stem)
                self.assertEqual(len(scales), 1, path.name)
                self.assertEqual(expectations(text).get("timescale"), str(scales[0]), path.name)
                self.assertTrue(10 <= scales[0] < 100, path.name)
                # The real-speed path: same scenario, same expectations.
                real = apply_timescale(text, 100)
                self.assertIn("\ntimescale 100\n", real)
                self.assertEqual(expectations(real)["timescale"], "100")
                self.assertGreater(plan(real)["script_s"], plan(text)["script_s"], path.name)
            else:
                self.assertNotIn("twait", {w[0] for w in steps}, path.name)
                self.assertEqual(apply_timescale(text, 100), text)
            self.assertEqual(len(sector_bytes(apply_timescale(text, 100))), 4096, path.name)
        self.assertEqual(sorted(scaled),
                         ["v3_animations", "v3_battery", "v3_morning", "v3_parent_menu",
                          "v4_env_tips", "v4_mini_game"])

    def test_every_expect_line_of_the_env_tips_scenario_is_parsed(self):
        # "# expect <name> <value>": the name has no spaces. A line the
        # parser does not match would be silently ignored in the report.
        path = Path(__file__).resolve().parents[1] / "tools/selftest/v4_env_tips.pettest"
        text = path.read_text(encoding="ascii")
        lines = [l for l in text.splitlines() if re.match(r"\s*#\s*expect\b", l)]
        self.assertGreaterEqual(len(lines), 10)
        self.assertEqual(len(expectations(text)), len(lines), "duplicate or unparsed expect")

    def test_mic_games_scenario_injects_the_microphone(self):
        # The hardware trial feeds synthetic loudness numbers; the real tap
        # must stay off for the whole run and nothing may be recorded.
        text = (Path(__file__).resolve().parents[1] / "tools/selftest/v4_mic_games.pettest"
                ).read_text(encoding="ascii")
        want = expectations(text)
        self.assertEqual(want["none:src=audio"], "0")
        self.assertEqual(want["commits"], "0")
        steps = [line.split() for line in text.splitlines()
                 if line.strip() and not line.lstrip().startswith("#")]
        # Every game is started with a micgame step first, so the script owns the "microphone".
        starts = [i for i, w in enumerate(steps) if w[0] == "micgame"]
        self.assertEqual(len(starts), 3)
        for i in starts:
            self.assertEqual(steps[i + 1][:2], ["tap", "press"])
        kinds = {w[1] for w in steps if w[0] == "envelope"}
        self.assertEqual(kinds, {"clap", "speech", "noise", "blow"})
        self.assertNotIn("timescale", {w[0] for w in steps})  # real-time games
        self.assertGreater(plan(text)["monitor_s"], plan(text)["expect_s"])
        self.assertIn("none:src=audio", evaluate(text, GOOD_LOG)["checks"])

    def test_motion_games_scenario_plan_and_expectations(self):
        # V4-07: gesture steps cost no time themselves; the waits around them do.
        path = Path(__file__).resolve().parents[1] / "tools/selftest/v4_motion_games.pettest"
        text = path.read_text(encoding="ascii")
        steps = [line.split() for line in text.splitlines()
                 if line.strip() and not line.lstrip().startswith("#")]
        gestures = [w[1] for w in steps if w[0] == "gesture"]
        self.assertEqual(gestures.count("tiltright"), 5)  # 3 + 2 rolls to holes 6 and 5
        self.assertEqual(gestures.count("tiltleft"), 3)   # to hole 0
        self.assertEqual(gestures.count("facedown"), 4)
        self.assertEqual(gestures.count("faceup"), 4)
        p = plan(text)
        waits = sum(int(w[1]) for w in steps if w[0] == "wait")
        self.assertGreater(p["script_s"] * 1000, waits)
        self.assertFalse(p["power_off"])
        self.assertEqual(p["until"], "PET_SELFTEST_DONE")
        self.assertEqual(expectations(text)["commits"], "0")
        good = ("PET_SELFTEST_MARK name=begin\n" + "PET_GAME_MENU highlight=0\n" * 3
                + "PET_GAME_CHOICE game=ball\nPET_GAME_CHOICE game=peekaboo\n"
                + "PET_GAME_START src=motion kind=ball\nPET_GAME_START src=motion kind=peekaboo\n"
                + "PET_GAME_END mode=menu end=home\n"
                + "PET_MOTION_STEP\n" * 5 +"PET_MOTION_HOLE\n" * 3
                + "PET_MOTION_TOO_SOON\n" + "PET_MOTION_HIDE\n" * 4 + "PET_MOTION_FOUND\n" * 3
                + "PET_GAME_END mode=ball end=completed growth=1\n"
                + "PET_GAME_END mode=peekaboo end=completed growth=1\n"
                + "PET_SELFTEST_DONE\nSUMMARY panic=0 wdt=0\n")
        report_ = evaluate(text, good)
        failed = [k for k in report_["failed"] if k not in ("max_partial_ms",)]
        self.assertEqual(failed, [])
        # The last roll of each round is a HOLE, not a STEP: 5 steps, not 8.
        self.assertEqual(expectations(text)["min:PET_MOTION_STEP"], "5")
        self.assertIn("min:PET_MOTION_STEP",
                      evaluate(text, good.replace("PET_MOTION_STEP\n", "", 1))["failed"])
        # Unparseable expect lines are errors, never silently ignored.
        for bad in ("# expect min:PET_GAME_MENU highlight 3", "# expect commits"):
            with self.assertRaises(ValueError):
                expectations("PETSELFTEST1\n" + bad + "\nend\n")
        # A missed reward or a refused start fails the scenario.
        self.assertIn("min:PET_MOTION_FOUND",
                      evaluate(text, good.replace("PET_MOTION_FOUND\n", "", 1))["failed"])
        self.assertIn("none:PET_GAME_REFUSED",
                      evaluate(text, good + "PET_GAME_REFUSED reason=motion_off\n")["failed"])

    @staticmethod
    def _steps(name):
        path = Path(__file__).resolve().parents[1] / "tools/selftest" / name
        text = path.read_text(encoding="ascii")
        return text, [line.split() for line in text.splitlines()
                      if line.strip() and not line.lstrip().startswith("#")]

    def test_v8_scenarios_never_power_off_and_pin_the_day_phase(self):
        # V8 scenarios are unattended and carry no alarm step: none of them may
        # reach a sleep flow (tap pwr / SLEEP_REQUEST), and the ones whose
        # behaviour follows the time of day say which phase they mean.
        for name in ("v8_care_cards.pettest", "v8_album.pettest", "v8_play_games.pettest",
                     "v8_sound_routines.pettest", "v4_book_reader.pettest",
                     "v9_menu.pettest"):
            text, steps = self._steps(name)
            self.assertIn(["phase", "day"], steps, name)
            self.assertLess(steps.index(["phase", "day"]), steps.index(["pet"]), name)
            self.assertNotIn(["alarm"], [w[:1] for w in steps], name)
            self.assertFalse(any(w[:2] == ["tap", "pwr"] for w in steps), name)
            self.assertFalse(any(w[:2] == ["vcmd", "SLEEP_REQUEST"] for w in steps), name)
            p = plan(text)
            self.assertFalse(p["power_off"], name)
            self.assertEqual(p["until"], "PET_SELFTEST_DONE", name)
            self.assertTrue(expectations(text), name)
            self.assertLessEqual(len(steps) - 2, 64, name)  # header and "end"

    def test_scenarios_that_open_game_or_feed_pin_the_day_phase(self):
        # Game / Feed refuse in the quiet hours (Feed then is the V7 plain feed:
        # 2026-11-07 22:22 run, 241,242,242 and 5 commits), so they never
        # depend on the wall clock.
        for name in ("v2_home_rail", "v5_phrases", "v3_animations", "v7_ledger_persist",
                     "v4_mini_game", "v4_motion_games", "v4_mic_games", "v9_menu"):
            _, steps = self._steps(name + ".pettest")
            self.assertIn(["phase", "day"], steps, name)
            self.assertLess(steps.index(["phase", "day"]), steps.index(["pet"]), name)

    def test_power_off_scenarios_run_last_and_say_how_long_to_wait(self):
        root = Path(__file__).resolve().parents[1] / "tools/selftest"
        texts = {p.stem: p.read_text(encoding="ascii") for p in sorted(root.glob("*.pettest"))}
        order = run_order(texts)
        self.assertEqual(sorted(order), sorted(texts))
        off = [name for name in order if plan(texts[name])["power_off"]]
        self.assertTrue(off)
        self.assertEqual(order[len(order) - len(off):], off)  # all of them at the end
        self.assertIn("v2_sleep", off)
        self.assertEqual(plan(texts["v2_sleep"])["wake_guard_s"], 60)
        self.assertEqual(plan(texts["v4_book_missing"])["wake_guard_s"], 0)

    def test_v8_feed_is_three_presses_in_the_home_rail_scenarios(self):
        # Rail Feed -> care menu -> food picker -> the fish: the third press feeds
        # (2026-11-07 board logs: two presses stopped in the picker).
        for name in ("v2_home_rail.pettest", "v5_phrases.pettest"):
            _, steps = self._steps(name)
            at = max(i for i, w in enumerate(steps) if w[:2] == ["tap", "press"]
                     and steps[i - 1][:2] == ["tap", "press"] and steps[i - 2][:2] == ["tap", "press"])
            self.assertEqual([w[0] for w in steps[at + 1:at + 2]], ["wait"], name)
            self.assertGreaterEqual(int(steps[at + 1][1]), 9000, name)
        text, _ = self._steps("v2_home_rail.pettest")
        # 12 rail moves + 1 hearts + 1 happy face + 3 feed animation frames + 2 moves
        # after = 19 (2026-11-08 board log); 5 fulls.
        self.assertEqual(expectations(text)["partial_refreshes"], "19")
        self.assertEqual(expectations(text)["full_refreshes"], "5")
        self.assertEqual(expectations(text)["home_select"], "0,1,2,3,4,3,2,1,0,1,2,3,2,1")

    def test_apply_timescale(self):
        text = "PETSELFTEST1\n# expect timescale 25\ntimescale 25\ntwait 8000\nend\n"
        self.assertEqual(apply_timescale(text, None), text)
        self.assertEqual(apply_timescale(text, 50),
                         "PETSELFTEST1\n# expect timescale 50\ntimescale 50\ntwait 8000\nend\n")
        for bad in (0, 9, 101):
            with self.assertRaises(ValueError):
                apply_timescale(text, bad)
        real_time = "PETSELFTEST1\nwait 100\nend\n"
        self.assertEqual(apply_timescale(real_time, 100), real_time)
        with self.assertRaises(ValueError):
            apply_timescale(real_time, 25)  # nothing in it would follow a scale

    def test_timescale_expectation(self):
        scenario = "PETSELFTEST1\n# expect timescale 25\ntimescale 25\nend\n"
        scaled = GOOD_LOG.replace(
            "[ 12s]", "[ 12s] I (12010) b: PET_SELFTEST_TIMESCALE pct=25\n[ 12s]")
        scaled = scaled.replace(
            "[ 30s]", "[ 30s] I (30000) b: PET_SELFTEST_TIMESCALE pct=100 reason=end\n[ 30s]")
        report = evaluate(scenario, scaled)
        self.assertTrue(report["passed"], report["failed"])
        self.assertEqual(report["observed"]["timescale"], [25])  # the end reset is no step
        self.assertIn("timescale", evaluate(scenario, GOOD_LOG)["failed"])  # never applied
        self.assertIn("timescale",
                      evaluate(scenario, scaled.replace("pct=25", "pct=50"))["failed"])
        # Real speed: no step, or only 100 %.
        real = apply_timescale(scenario, 100)
        self.assertTrue(evaluate(real, GOOD_LOG)["passed"])
        self.assertTrue(evaluate(real, scaled.replace("pct=25", "pct=100"))["passed"])
        self.assertIn("timescale", evaluate(real, scaled)["failed"])

    def test_plan_estimates_monitor_time_and_end_marker(self):
        done = plan("PETSELFTEST1\nwaitidle 60000\npet\nwait 5000\ntap up 2000\nend\n")
        # 6 s ready estimate + 1 s toggle + 5 s + 2.08 s -> 15 s of script.
        self.assertEqual(done["script_s"], 15)
        self.assertFalse(done["power_off"])
        self.assertEqual(done["until"], "PET_SELFTEST_DONE")
        self.assertEqual(done["expect_s"], 13 + 15)
        self.assertEqual(done["monitor_s"], 13 + 15 + 10)
        # twait follows the scale; plain waits do not.
        scaled = "PETSELFTEST1\ntimescale 25\ntwait 40000\nwait 4000\nend\n"
        self.assertEqual(plan(scaled)["script_s"], 14)
        self.assertEqual(plan(apply_timescale(scaled, 100))["script_s"], 44)
        # Power-off scenarios end at the boot the alarm brings.
        off = plan("PETSELFTEST1\nalarm 30\ntap pwr 30000\nend\n")
        self.assertTrue(off["power_off"])
        self.assertEqual(off["until"], "PET_SELFTEST_NONE reason=alarm_wake")
        self.assertEqual(off["expect_s"], 13 + 11 + 30 + 15)
        self.assertEqual(off["monitor_s"], 13 + 31 + 10 + 30 + 15)
        # A morning with a dial touch after it stays awake; the last one sleeps.
        stays = "PETSELFTEST1\nalarm 30\nmorning\nwait 5000\ntap down 1000\nend\n"
        self.assertFalse(plan(stays)["power_off"])
        sleeps = stays.replace("end\n", "morning\ntimescale 50\ntwait 60000\nend\n")
        self.assertEqual(plan(sleeps)["until"], "PET_SELFTEST_NONE reason=alarm_wake")
        # V4-08: an injected sound takes as long as it plays; picking the game is free.
        mic = plan("PETSELFTEST1\nmicgame clap\nenvelope clap 700\nenvelope blow 5000\nend\n")
        self.assertEqual(mic["script_s"], 6)
        self.assertFalse(mic["power_off"])
        critical = "PETSELFTEST1\nalarm 30\nbattery 5 0\nwait 1000\nend\n"
        self.assertTrue(plan(critical)["power_off"])
        self.assertFalse(plan(critical.replace("battery 5 0", "battery 5 1"))["power_off"])
        # V6-02: a voice sleep request powers off unless a key press follows.
        voice = "PETSELFTEST1\nalarm 30\nvcmd SLEEP_REQUEST\nwait 1000\nend\n"
        self.assertTrue(plan(voice)["power_off"])
        cancelled = voice.replace("wait 1000", "tap press 1000")
        self.assertFalse(plan(cancelled)["power_off"])
        self.assertFalse(plan("PETSELFTEST1\nvcmd TELL_TIME\nwait 1000\nend\n")["power_off"])


if __name__ == "__main__":
    unittest.main()


REPO = Path(__file__).resolve().parents[1]
FIRMWARE_PARSER = REPO / "firmware/components/kid_app/src/selftest_script.cc"


class LineLengthTests(unittest.TestCase):
    def test_max_line_matches_firmware(self):
        m = re.search(r"kMaxLine\s*=\s*(\d+)\s*;", FIRMWARE_PARSER.read_text(encoding="utf-8"))
        self.assertIsNotNone(m)
        self.assertEqual(MAX_LINE, int(m.group(1)))

    def test_every_repo_scenario_fits_the_board_parser(self):
        paths = sorted((REPO / "tools/selftest").glob("*.pettest"))
        self.assertTrue(paths)
        for path in paths:
            with open(path, encoding="ascii", newline="") as handle:
                self.assertEqual(long_lines(handle.read()), [], path.name)

    def test_80_ok_81_rejected_comments_included(self):
        ok = "PETSELFTEST1\n# " + "x" * 78 + "\nend\n"
        bad = "PETSELFTEST1\n# " + "x" * 79 + "\nend\n"
        self.assertEqual(long_lines(ok), [])
        self.assertEqual(long_lines(bad), [(2, 81)])
        self.assertEqual(len(sector_bytes(ok)), 4096)
        with self.assertRaisesRegex(ValueError, "scenario:2"):
            sector_bytes(bad)

    def test_cr_and_trailing_blanks_count_like_the_firmware(self):
        # The firmware measures up to '\n' before it drops the '\r'.
        self.assertEqual(long_lines("# " + "x" * 78 + "\r\n"), [(1, 81)])
        self.assertEqual(long_lines("# " + "x" * 78 + " \n"), [(1, 81)])
        self.assertEqual(long_lines("# " + "x" * 77 + "\r\n"), [])

    def test_cli_sector_and_plan_name_file_and_line(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "long.pettest"
            path.write_bytes(("PETSELFTEST1\n# " + "x" * 90 + "\nend\n").encode())
            out = Path(tmp) / "out.sector"
            for argv in (["sector", str(path), str(out)], ["plan", str(path)],
                         ["order", str(path)]):
                with contextlib.redirect_stderr(io.StringIO()) as err:
                    self.assertEqual(main(argv), 2, argv)
                self.assertIn("long.pettest:2", err.getvalue())
            self.assertFalse(out.exists())
