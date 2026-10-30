"""Host-only checks for the optional, explicitly non-child KPB1 smoke fixture."""
from __future__ import annotations
import hashlib
from pathlib import Path
import struct
import tempfile
import unittest
import wave

from tools.make_story_sd_fixture import build
from tools.story_pack import HEADER, RECORD, FRAME_BYTES


class SdSmokeFixtureTests(unittest.TestCase):
    def test_distinct_pages_and_24k_pcm_for_real_tf_card(self):
        with tempfile.TemporaryDirectory() as folder:
            target=Path(folder)
            report=build(target)
            binary=target/"books"/"forest_friends.kpb"
            payload=binary.read_bytes()
            self.assertEqual(hashlib.sha256(payload).hexdigest(),report["sha256"])
            self.assertEqual(HEADER.unpack_from(payload)[4],2)
            a=RECORD.unpack_from(payload,32)
            b=RECORD.unpack_from(payload,32+64)
            self.assertEqual(a[0],1)
            self.assertEqual(b[0],2)
            self.assertEqual(a[3],FRAME_BYTES)
            self.assertEqual(b[3],FRAME_BYTES)
            self.assertEqual(a[8],24000)
            self.assertEqual(b[8],24000)
            self.assertEqual(a[7],24000) # 500 ms, mono signed 16-bit
            self.assertEqual(b[7],24000)
            self.assertNotEqual(payload[a[2]:a[2]+a[3]],
                                payload[b[2]:b[2]+b[3]])
            self.assertNotEqual(payload[a[5]:a[5]+a[6]],
                                payload[b[5]:b[5]+b[6]])
            self.assertIn("HARDWARE SMOKE TEST ONLY",
                          (target/"README-SMOKE-ONLY.txt").read_text())

    def test_reproducible_bytes_and_consistent_readme(self):
        with tempfile.TemporaryDirectory() as folder:
            out=Path(folder)
            a=build(out)
            b=build(out)
            self.assertEqual(a["sha256"],b["sha256"])
            self.assertIn(a["sha256"],(out/"README-SMOKE-ONLY.txt").read_text())


if __name__=="__main__":
    unittest.main()
