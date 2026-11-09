"""Contract for the build-time wake word switch (你好团团 by default, 你好小智 on request).

Offline by default. When UPSTREAM_SRC points at the patched upstream checkout (the CI
step after the build does this) the partition table and the MultiNet7 model size are
checked against the real files too.
"""
import json
import os
import re
import subprocess
import sys
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from scripts import p1_integrate as p1  # noqa: E402

UPSTREAM_BOARD_CONFIG = json.dumps({
    "manufacturer": "waveshare", "type": "esp32-s3-epaper-3.97", "target": "esp32s3",
    "builds": [{"name": "esp32-s3-epaper-3.97",
                "sdkconfig_append": ["CONFIG_USE_WECHAT_MESSAGE_STYLE=n"]}]}, indent=4)

# Measured numbers (see docs/WAKE_WORD.md). The "assets" partition holds srmodels.bin;
# this partition table has no separate srmodels partition.
ASSETS_PARTITION_KB = 8192      # upstream partitions/v2/16m.csv: assets, data, spiffs, 8M
ASSETS_USED_NOW_KB = 2880       # boot log, WakeNet build (fonts, sounds, wn9 model, ...)
WN9_KB = 289                    # esp-sr model/wakenet_model/wn9_nihaoxiaozhi_tts
MN7_CN_KB = 2617                # esp-sr model/multinet_model/mn7_cn
MIN_HEADROOM_KB = 1024          # keep at least 1 MiB free for content growth


def append_for(choice=None):
    patched = json.loads(p1.patch_board_config(UPSTREAM_BOARD_CONFIG, choice))
    return patched["builds"][0]["sdkconfig_append"]


def assignments(lines):
    out = {}
    for line in lines:
        key, _, value = line.partition("=")
        out[key] = value
    return out


class WakeWordConfigContract(unittest.TestCase):
    def setUp(self):
        env = mock.patch.dict(os.environ)
        env.start()
        self.addCleanup(env.stop)
        os.environ.pop(p1.WAKE_WORD_ENV, None)

    def test_default_is_tuantuan_multinet(self):
        self.assertEqual(p1.WAKE_WORD_DEFAULT, "tuantuan")
        a = assignments(append_for())
        self.assertEqual(a["CONFIG_USE_CUSTOM_WAKE_WORD"], "y")
        self.assertEqual(a["CONFIG_USE_AFE_WAKE_WORD"], "n")
        self.assertEqual(a["CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS"], "n")
        self.assertEqual(a["CONFIG_SR_MN_CN_MULTINET7_QUANT"], "y")
        self.assertEqual(a["CONFIG_CUSTOM_WAKE_WORD"], '"ni hao tuan tuan"')
        self.assertEqual(a["CONFIG_CUSTOM_WAKE_WORD_DISPLAY"], '"你好团团"')
        self.assertEqual(a["CONFIG_CUSTOM_WAKE_WORD_THRESHOLD"], "20")

    def test_pinyin_is_plain_lowercase_syllables(self):
        # esp-sr: pinyin only, space separated, no digits / tones / Latin mixing.
        self.assertRegex(p1.WAKE_WORD_PINYIN, r"^[a-z]+( [a-z]+)*$")
        self.assertGreaterEqual(len(p1.WAKE_WORD_PINYIN.split()), 3)  # >= 3 syllables advised
        self.assertTrue
        self.assertTrue(1 <= p1.WAKE_WORD_THRESHOLD <= 99)

    def test_pet_settings_and_upstream_line_survive(self):
        lines = append_for()
        self.assertEqual(lines[0], "CONFIG_USE_WECHAT_MESSAGE_STYLE=n")
        a = assignments(lines)
        self.assertEqual(a["CONFIG_FATFS_LFN_HEAP"], "y")
        self.assertIn("CONFIG_OTA_URL", a)
        self.assertIn("CONFIG_KID_PET_SERVER_ORIGIN", a)

    def test_no_duplicate_keys(self):
        keys = [line.split("=", 1)[0] for line in append_for()]
        self.assertEqual(len(keys), len(set(keys)))

    def test_one_step_back_to_xiaozhi(self):
        for how in ("arg", "env"):
            with self.subTest(how=how):
                if how == "env":
                    os.environ[p1.WAKE_WORD_ENV] = "xiaozhi"
                    lines = append_for()
                else:
                    lines = append_for("xiaozhi")
                keys = {line.split("=", 1)[0] for line in lines}
                # Nothing wake-word related is overridden: upstream WakeNet default wins.
                self.assertFalse({k for k in keys if "WAKE_WORD" in k or "CONFIG_SR_" in k})
                self.assertIn("CONFIG_FATFS_LFN_HEAP", keys)  # the rest still applies

    def test_argument_beats_env_and_bad_value_is_rejected(self):
        os.environ[p1.WAKE_WORD_ENV] = "xiaozhi"
        self.assertEqual(p1.resolve_wake_word("tuantuan"), "tuantuan")
        os.environ[p1.WAKE_WORD_ENV] = "  TuanTuan "
        self.assertEqual(p1.resolve_wake_word(), "tuantuan")
        os.environ[p1.WAKE_WORD_ENV] = ""  # empty (unset CI input) means default
        self.assertEqual(p1.resolve_wake_word(), p1.WAKE_WORD_DEFAULT)
        with self.assertRaises(RuntimeError):
            p1.resolve_wake_word("nihao")

    def test_upstream_already_setting_a_wake_key_is_noticed(self):
        cfg = json.loads(UPSTREAM_BOARD_CONFIG)
        cfg["builds"][0]["sdkconfig_append"].append("CONFIG_USE_CUSTOM_WAKE_WORD=n")
        with self.assertRaises(RuntimeError):
            p1.patch_board_config(json.dumps(cfg))

    def test_cli_exposes_the_switch(self):
        out = subprocess.run([sys.executable, str(ROOT / "scripts/p1_integrate.py"), "--help"],
                             capture_output=True, text=True, encoding="utf-8")
        self.assertIn("--wake-word", out.stdout)
        self.assertIn("xiaozhi", out.stdout)


class ServerWakeupWords(unittest.TestCase):
    def test_child_config_lists_both_wake_texts(self):
        # The board sends the wake word text as the first utterance; an unlisted text
        # would reach the LLM instead of being swallowed (enable_greeting: false).
        text = (ROOT / "deploy/xiaozhi/child.config.yaml").read_text(encoding="utf-8")
        block = re.search(r"^wakeup_words:\n((?:  - .*\n)+)", text, re.M).group(1)
        words = re.findall(r'"([^"]+)"', block)
        self.assertIn(p1.WAKE_WORD_DISPLAY, words)
        self.assertIn("你好小智", words)


class AssetsPartitionBudget(unittest.TestCase):
    """MultiNet7 replaces WakeNet9 inside srmodels.bin, which lives in the assets partition."""

    def test_budget_arithmetic(self):
        after = ASSETS_USED_NOW_KB - WN9_KB + MN7_CN_KB
        self.assertLess(after, ASSETS_PARTITION_KB)
        self.assertGreaterEqual(ASSETS_PARTITION_KB - after, MIN_HEADROOM_KB)

    def test_even_keeping_wakenet_it_fits(self):  # worst case, if both were packed
        self.assertLess(ASSETS_USED_NOW_KB + MN7_CN_KB, ASSETS_PARTITION_KB - MIN_HEADROOM_KB)

    @unittest.skipUnless(os.environ.get("UPSTREAM_SRC"), "needs UPSTREAM_SRC (CI)")
    def test_real_partition_table(self):
        src = Path(os.environ["UPSTREAM_SRC"])
        defaults = (src / "sdkconfig.defaults").read_text(encoding="utf-8")
        table = re.search(r'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="([^"]+)"', defaults).group(1)
        csv = (src / table).read_text(encoding="utf-8")
        row = next(line for line in csv.splitlines() if line.startswith("assets"))
        size = row.split(",")[4].strip()
        kb = int(size[:-1]) * 1024 if size.endswith("M") else int(size, 0) // 1024
        self.assertGreaterEqual(kb, ASSETS_PARTITION_KB)

    @unittest.skipUnless(os.environ.get("UPSTREAM_SRC"), "needs UPSTREAM_SRC (CI)")
    def test_real_multinet_model_size(self):
        src = Path(os.environ["UPSTREAM_SRC"])
        model = src / "managed_components/espressif__esp-sr/model/multinet_model/mn7_cn"
        if not model.is_dir():
            self.skipTest("esp-sr not fetched yet")
        kb = sum(f.stat().st_size for f in model.rglob("*") if f.is_file()) / 1024
        self.assertLess(kb, ASSETS_PARTITION_KB - ASSETS_USED_NOW_KB + WN9_KB - MIN_HEADROOM_KB)

    @unittest.skipUnless(os.environ.get("UPSTREAM_SRC"), "needs UPSTREAM_SRC (CI)")
    def test_patched_checkout_carries_the_wake_config(self):
        src = Path(os.environ["UPSTREAM_SRC"])
        cfg = json.loads((src / p1.BOARD_REL / "config.json").read_text(encoding="utf-8"))
        a = assignments(cfg["builds"][0]["sdkconfig_append"])
        want = os.environ.get(p1.WAKE_WORD_ENV) or p1.WAKE_WORD_DEFAULT
        self.assertEqual(a.get("CONFIG_USE_CUSTOM_WAKE_WORD") == "y", want == "tuantuan")


if __name__ == "__main__":
    unittest.main()
