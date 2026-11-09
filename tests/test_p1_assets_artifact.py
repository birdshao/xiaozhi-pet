"""Assets partition artifact manifest and the official-assets rollback cutter (no hardware)."""
import hashlib
import json
import sys
import tempfile
import unittest
import unittest.mock
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from scripts import p1_assets_artifact as art  # noqa: E402
from scripts import restore_official_assets as restore  # noqa: E402


class AssetsArtifact(unittest.TestCase):
    def test_manifest_pairs_assets_with_app(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            (d / "a.bin").write_bytes(b"x" * 5000)
            (d / "m.json").write_text(json.dumps({"image_sha256": "ab" * 32, "image_filename": "app.bin"}))
            m = art.package(d / "a.bin", d / "m.json", d / "out", "tuantuan", "123")
            on_disk = json.loads((d / "out" / "assets-manifest.json").read_text())
            self.assertEqual(m, on_disk)
            self.assertEqual(m["flash_offset"], "0x800000")
            self.assertEqual(m["paired_app_sha256"], "ab" * 32)
            self.assertEqual(m["image_sha256"], hashlib.sha256(b"x" * 5000).hexdigest())
            self.assertEqual(m["official_assets_rollback"]["size"], 2_900_513)
            self.assertTrue(m["write_only_this_partition"])
            self.assertFalse(m["safe_to_flash_without_checks"])

    def test_oversize_and_bad_run_id_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            (d / "m.json").write_text(json.dumps({"image_sha256": "0" * 64, "image_filename": "a"}))
            (d / "big.bin").write_bytes(b"\0" * (art.ASSETS_PARTITION_BYTES + 1))
            with self.assertRaises(ValueError):
                art.package(d / "big.bin", d / "m.json", d / "o", "tuantuan", "1")
            (d / "ok.bin").write_bytes(b"1")
            with self.assertRaises(ValueError):
                art.package(d / "ok.bin", d / "m.json", d / "o", "tuantuan", "x")


class RestoreOfficialAssets(unittest.TestCase):
    def test_rejects_wrong_content_and_never_writes(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            image = d / "full.bin"
            image.write_bytes(b"\0" * (art.ASSETS_OFFSET + art.OFFICIAL_ASSETS_BYTES))
            with self.assertRaises(ValueError):
                restore.cut(image, d / "out.bin")
            self.assertFalse((d / "out.bin").exists())

    def test_cuts_when_sha_matches(self):
        payload = b"official"
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            image = d / "full.bin"
            image.write_bytes(b"\0" * art.ASSETS_OFFSET + payload)
            with unittest.mock.patch.object(restore, "OFFICIAL_ASSETS_BYTES", len(payload)), \
                 unittest.mock.patch.object(restore, "OFFICIAL_ASSETS_SHA256", hashlib.sha256(payload).hexdigest()):
                restore.cut(image, d / "out.bin")
            self.assertEqual((d / "out.bin").read_bytes(), payload)


if __name__ == "__main__":
    import unittest.mock  # noqa: F401
    unittest.main()
