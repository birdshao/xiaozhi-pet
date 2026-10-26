"""Verify P1's narrow board overlay supports full scene rendering.

Does not need the ESP-IDF SDK; test patches on fixture snippets.
"""
import unittest
from scripts.p1_integrate import patch_display_h, patch_display_cc

class DisplayOverlayTest(unittest.TestCase):
    def test_scene_display_header(self):
        original = (
            '#include "lcd_display.h"\n'
            'class Dummy {\n'
            '    void EPD_Sleep();\n'
            '    uint8_t* buffer = NULL;\n'
            '};\n'
        )
        patched = patch_display_h(original)
        self.assertIn("bool ShowPetScene(const kid_app::SceneModel& scene);", patched)
        self.assertIn("bool ShowPetPreview(kid_app::Emotion emotion);", patched)
        self.assertEqual(patched.count("bool ShowPetScene"), 1)

    def test_preview_forwards_to_shared_renderer(self):
        source = '#include <vector>\nvoid Flush() {\n    driver->EPD_Clear();\n}\n'
        patched = patch_display_cc(source)
        self.assertIn("return ShowPetScene({kid_app::SceneKind::Home, emotion, 1});", patched)
        self.assertIn("bool CustomEpdDisplay::ShowPetScene(const kid_app::SceneModel& scene)", patched)
        self.assertIn("kid_app::SceneComposer::Render(scene, *frame)", patched)
        self.assertIn("heap_caps_malloc", patched)
        self.assertIn("heap_caps_free(memory)", patched)
        self.assertIn("pet_preview_enabled_", patched)

if __name__ == "__main__":
    unittest.main()
