"""Host-only checks for the locked firmware source: no ESP-IDF or device needed."""
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))
import p0_check  # noqa: E402


class ManifestTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.manifest = json.loads(p0_check.MANIFEST.read_text(encoding="utf-8"))

    def test_pinned_manifest_is_valid(self):
        p0_check.validate_manifest(self.manifest)

    def test_sha_must_be_full_length(self):
        changed = json.loads(json.dumps(self.manifest))
        changed["firmware"]["commit"] = "deadbeef"
        with self.assertRaisesRegex(ValueError, "40-character"):
            p0_check.validate_manifest(changed)

    def test_board_must_match_hardware(self):
        changed = json.loads(json.dumps(self.manifest))
        changed["board"]["target"] = "esp32"
        with self.assertRaisesRegex(ValueError, "board identity"):
            p0_check.validate_manifest(changed)

    def test_source_board_files_and_commit(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / ".git").mkdir()
            board = root / "main/boards/waveshare/esp32-s3-epaper-3.97"
            board.mkdir(parents=True)
            names = [
                "config.json", "config.h", "custom_lcd_display.h",
                "custom_lcd_display.cc", "waveshare-s3-epaper-3.97.cc",
            ]
            for name in names:
                (board / name).write_text("", encoding="utf-8")
            (board / "config.json").write_text(
                json.dumps({"target": "esp32s3", "builds": [{"name": "esp32-s3-epaper-3.97"}]}),
                encoding="utf-8",
            )
            (board / "config.h").write_text(
                "#define EXAMPLE_LCD_WIDTH   800\n#define EXAMPLE_LCD_HEIGHT  480\n",
                encoding="utf-8",
            )
            with patch.object(
                p0_check.subprocess, "check_output",
                return_value=self.manifest["firmware"]["commit"] + "\n",
            ):
                result = p0_check.inspect_source(root, self.manifest)
            self.assertEqual(result["result"], "source_static_checks_passed")

    def test_missing_board_file_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / ".git").mkdir()
            with patch.object(
                p0_check.subprocess, "check_output",
                return_value=self.manifest["firmware"]["commit"],
            ):
                with self.assertRaisesRegex(ValueError, "Missing board files"):
                    p0_check.inspect_source(root, self.manifest)


if __name__ == "__main__":
    unittest.main()
