"""Waveshare 3.97 pinned-board story integration source contract.

The actual ESP32-S3 build separately compiles this with the pinned
upstream board. These checks ensure a future overlay change cannot quietly
remove the real SD mount / display fallback or change official pins.
"""
from pathlib import Path
import re
import unittest

from scripts.p1_integrate import BOARD_WORKER, DISPLAY_IMPL, patch_board, patch_display_h

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
        self.assertIn("ShowStoryPage(",BOARD_WORKER)
        self.assertIn("if (!okay) okay = self->display_->ShowPetScene(scene)",BOARD_WORKER)
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

    def test_main_component_lists_sd_deps_and_source(self):
        cmake=(ROOT/"firmware/components/kid_app/CMakeLists.txt").read_text()
        for name in ("waveshare_story_sd.cc","fatfs","sdmmc","esp_driver_sdmmc"):
            self.assertIn(name,cmake)

if __name__=="__main__":
    unittest.main()
