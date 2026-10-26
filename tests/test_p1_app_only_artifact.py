#!/usr/bin/env python3
"""No hardware is touched by these build artifact contract tests."""
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from p1_app_only_artifact import NAME, SLOT_BYTES, metadata, package


def valid_image(length=100_001):
    image = bytearray(b"\xff" * length)
    image[:2] = b"\xe9\x04"
    struct.pack_into("<I", image, 32, 0xABCD5432)
    image[48:53] = b"2.5.2"
    image[80:87] = b"xiaozhi"
    return bytes(image)


class AppOnlyArtifactTests(unittest.TestCase):
    def test_valid_app_metadata(self):
        result = metadata(valid_image())
        self.assertEqual(result["image_size"], 100_001)
        self.assertEqual(result["app_version"], "2.5.2")

    def test_rejects_merged_image_without_app_descriptor(self):
        corrupted = bytearray(valid_image())
        corrupted[32:36] = b"\xff" * 4
        with self.assertRaisesRegex(ValueError, "merged image"):
            metadata(bytes(corrupted))

    def test_rejects_oversize(self):
        with self.assertRaisesRegex(ValueError, "OTA slot"):
            metadata(valid_image(SLOT_BYTES + 1))

    def test_rejects_bad_header(self):
        corrupted = bytearray(valid_image())
        corrupted[0] = 0
        with self.assertRaisesRegex(ValueError, "application image"):
            metadata(bytes(corrupted))

    def test_output_manifest_and_app_only_bytes(self):
        with tempfile.TemporaryDirectory() as dirname:
            root = Path(dirname)
            src = root / "source.bin"
            src.write_bytes(valid_image())
            result = package(src, root / "out", "a" * 40, "b" * 40, "12345")
            target = root / "out" / NAME
            manifest = json.loads((root / "out" / "manifest.json").read_text())
            self.assertEqual(target.read_bytes(), src.read_bytes())
            self.assertEqual(result, manifest)
            self.assertEqual(manifest["image_sha256"], hashlib.sha256(src.read_bytes()).hexdigest())
            self.assertEqual(manifest["maximum_single_ota_partition_bytes"], SLOT_BYTES)
            self.assertEqual(manifest["image_kind"], "application_only")
            self.assertFalse(manifest["safe_to_flash_without_checks"])
            for key in ("contains_bootloader", "contains_partition_table",
                        "contains_nvs", "contains_assets"):
                self.assertFalse(manifest[key])

    def test_rejects_unpinned_provenance(self):
        with tempfile.TemporaryDirectory() as dirname:
            root = Path(dirname)
            source = root / "app.bin"
            source.write_bytes(valid_image())
            with self.assertRaisesRegex(ValueError, "git commit SHA"):
                package(source, root / "out", "main", "b" * 40, "1234")


if __name__ == "__main__":
    unittest.main()
