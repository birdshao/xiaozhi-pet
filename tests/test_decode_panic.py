"""Panic log parsing of tools/decode_panic.py (the toolchain call is not run)."""
import unittest

from tools.decode_panic import flash_mapped, parse_panic

# From the 2026-11-07 board run (v4_book_reader, first run).
PANIC = """I (12697) MODEL_LOADER: Successfully load srmodels
Guru Meditation Error: Core  0 panic'ed (Cache error). 
Dbus write to cache rejected, error address: 0x3cd9a064

Core  0 register dump:
PC      : 0x420ffbb7  PS      : 0x00060434  A0      : 0x821033a3  A1      : 0x3fcce560

Backtrace: 0x420ffbb4:0x3fcce560 0x421033a0:0x3fcce580 0x420f8220:0x3fcce690

ELF file SHA256: cf58a2d00
"""


class DecodePanicTests(unittest.TestCase):
    def test_parse_panic(self):
        found = parse_panic(PANIC)
        self.assertEqual(found["core"], 0)
        self.assertEqual(found["reason"], "Cache error")
        self.assertEqual(found["error_address"], "0x3cd9a064")
        self.assertEqual(found["pc"], "0x420ffbb7")
        self.assertEqual(found["elf_sha_prefix"], "cf58a2d00")
        self.assertEqual(found["backtrace"], ["0x420ffbb4", "0x421033a0", "0x420f8220"])

    def test_log_without_a_panic_finds_nothing(self):
        found = parse_panic("I (1) boot: fine\n")
        self.assertIsNone(found["reason"])
        self.assertEqual(found["backtrace"], [])

    def test_flash_window(self):
        self.assertTrue(flash_mapped("0x3cd9a064"))
        self.assertFalse(flash_mapped("0x3fca8254"))  # internal RAM
        self.assertFalse(flash_mapped("0x420ffbb7"))  # code


if __name__ == "__main__":
    unittest.main()
