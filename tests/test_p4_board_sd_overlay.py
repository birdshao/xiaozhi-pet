"""Waveshare 3.97 pinned-board story integration source contract.

The actual ESP32-S3 build separately compiles this with the pinned
upstream board. These checks ensure a future overlay change cannot quietly
remove the real SD mount / display fallback or change official pins.
"""
from pathlib import Path
import re
import unittest

import json

from scripts.p1_integrate import (BOARD_WORKER, DISPLAY_IMPL, patch_board, patch_board_config,
                                  patch_display_h)

ROOT=Path(__file__).resolve().parents[1]
SD=(ROOT/"firmware/components/kid_app/src/waveshare_story_sd.cc").read_text(encoding="utf-8")

class WaveshareSdOverlayTests(unittest.TestCase):
    def test_official_waveshare_pin_map_and_nonformatting(self):
        expected={"kClk":16,"kCmd":17,"kD0":15,"kD1":7,"kD2":8,"kD3":18}
        for name, gpio in expected.items():
            self.assertRegex(SD, rf"constexpr int {name} = {gpio};")
        self.assertIn("slot.width = 4", SD)
        self.assertIn("format_if_mount_failed = false", SD)
        self.assertIn("SDMMC_FREQ_DEFAULT", SD)
        self.assertIn("esp_vfs_fat_sdmmc_mount",SD)
        self.assertNotIn("fopen(", SD)
        self.assertNotIn("sdmmc_card_erase", SD)

    def test_only_reader_frame_uses_sd_and_fallback_remains(self):
        self.assertIn("story_sd_.Initialize",BOARD_WORKER)
        self.assertIn("story_sd_.Ready()",BOARD_WORKER)
        self.assertIn("scene.kind == kid_app::SceneKind::Reader",BOARD_WORKER)
        self.assertIn("okay = DrawBookLocked(scene, &mode);",BOARD_WORKER)
        # Missing SD / out-of-range page (V4-05: also the end picture) still
        # falls back to the pet scene (V2: full or partial chosen by the
        # refresh planner).
        fallback = BOARD_WORKER[BOARD_WORKER.index("okay = DrawBookLocked(scene, &mode);"):]
        fallback = fallback[fallback.index("if (!okay) {"):]
        self.assertIn(": display_->ShowPetScene(scene);", fallback[:400])
        self.assertIn("ShowPetScene",DISPLAY_IMPL)
        self.assertIn("reader.ReadFrame(page, *frame)",DISPLAY_IMPL)
        self.assertIn("EPD_DisplayPartBaseImage()",DISPLAY_IMPL)
        self.assertIn("lvgl_port_lock(5000)",DISPLAY_IMPL)
        self.assertIn("pet_preview_enabled_",DISPLAY_IMPL)

    def test_upstream_display_header_overlay(self):
        source=(
            '#include "lcd_display.h"\n'
            "    void EPD_Sleep();\n"
            "    uint8_t* buffer = NULL;\n"
        )
        patched=patch_display_h(source)
        self.assertIn("ShowStoryPage(kid_app::StoryPackReader& reader",patched)
        self.assertIn('#include "kid_app/story_pack_reader.h"',patched)

    def test_board_build_enables_fat_long_file_names(self):
        # 2026-11-05: the card mounted but every pack ("reader_test.kpb",
        # "forest_friends.kpb") failed to open: FatFs defaulted to 8.3 names.
        upstream = ('{\n    "manufacturer": "waveshare",\n    "type": "esp32-s3-epaper-3.97",\n'
                    '    "target": "esp32s3",\n    "builds": [\n        {\n'
                    '            "name": "esp32-s3-epaper-3.97",\n            "sdkconfig_append": [\n'
                    '                "CONFIG_USE_WECHAT_MESSAGE_STYLE=n"\n            ]\n        }\n    ]\n}\n')
        patched = json.loads(patch_board_config(upstream))
        append = patched["builds"][0]["sdkconfig_append"]
        self.assertEqual(append[0], "CONFIG_USE_WECHAT_MESSAGE_STYLE=n")  # upstream kept first
        self.assertIn("CONFIG_FATFS_LFN_HEAP=y", append)
        self.assertIn("CONFIG_FATFS_MAX_LFN=255", append)
        self.assertIn('CONFIG_OTA_URL="http://192.168.1.110:8780/xiaozhi/ota/"', append)
        self.assertIn('CONFIG_KID_PET_SERVER_ORIGIN="https://pet.birdshao.cn:8443"', append)
        self.assertEqual(patched["builds"][0]["name"], "esp32-s3-epaper-3.97")
        self.assertTrue(patch_board_config(upstream).endswith("}\n"))
        # Upstream starting to set it itself must be noticed, not silently doubled.
        twice = json.dumps({"builds": [{"name": "x", "sdkconfig_append": ["CONFIG_FATFS_LFN_HEAP=y"]}]})
        with self.assertRaises(RuntimeError):
            patch_board_config(twice)
        with self.assertRaises(RuntimeError):
            patch_board_config(json.dumps({"builds": []}))
        # The integration actually patches the board config file.
        src = (ROOT / "scripts/p1_integrate.py").read_text(encoding="utf-8")
        self.assertIn('board_config: patch_board_config(board_config.read_text(encoding="utf-8"))', src)

    def test_main_component_lists_sd_deps_and_source(self):
        cmake=(ROOT/"firmware/components/kid_app/CMakeLists.txt").read_text()
        for name in ("waveshare_story_sd.cc","fatfs","sdmmc","esp_driver_sdmmc"):
            self.assertIn(name,cmake)

if __name__=="__main__":
    unittest.main()
