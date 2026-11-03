"""Verify P1's narrow board overlay supports full scene rendering.

Does not need the ESP-IDF SDK; test patches on fixture snippets.
"""
import unittest
from scripts.p1_integrate import patch_display_h, patch_display_cc, patch_board

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


    def test_device_pet_runtime_board_hooks(self):
        original = ('#include <stdio.h>\n'
                    'class Board {\n'
                    '    void InitializeButtons() {\n'
                    '        boot_button_.OnClick([this]() {\n'
                    '            app.ToggleChatState();\n'
                    '        });\n'
                    '        pwr_button_.OnClick([this]() {\n'
                    '        });\n'
                    '    }\n'
                    '};\n')
        patched = patch_board(original)
        self.assertIn('FreertosPetSceneMailbox pet_mailbox_', patched)
        self.assertIn('FreertosPetService pet_service_', patched)
        self.assertIn('FreertosPetDisplayWorker pet_display_worker_', patched)
        self.assertIn('ShowPetScene(scene)', patched)
        self.assertIn('PET_FIRMWARE_BOOT_MARKER phase=board_init', patched)
        self.assertIn('PET_FIRMWARE_BOOT_MARKER phase=delay5s', patched)
        self.assertIn('PET_FIRMWARE_BOOT_MARKER phase=delay12s', patched)
        self.assertEqual(patched.count('PET_FIRMWARE_BOOT_MARKER'), 3)
        self.assertIn('vTaskDelete(nullptr);', patched)
        self.assertIn('pet_diag_started != pdPASS', patched)
        self.assertIn('PET_RUNTIME_ON', patched)
        self.assertIn('PET_RUNTIME_OFF', patched)
        self.assertIn('PetInputRouter::Decide(', patched)
        self.assertIn('PetClick::Single', patched)
        self.assertIn('PetClick::Double', patched)
        self.assertIn('PetClick::Triple', patched)
        self.assertIn('PostLocal(decision.action)', patched)
        self.assertIn('pet_event_mode_.load()', patched)
        self.assertIn('pet_screen_mutex_', patched)
        self.assertIn('app.ToggleChatState();', patched)
        self.assertIn('pwr_button_.OnClick', patched)
        self.assertEqual(patched.count('OnLongPress'), 1)
        self.assertEqual(patched.count('OnDoubleClick'), 1)

    def _board(self):
        return patch_board('#include <stdio.h>\n'
                           'class Board {\n'
                           '    void InitializeButtons() {\n'
                           '        boot_button_.OnClick([this]() {\n'
                           '            app.ToggleChatState();\n'
                           '        });\n'
                           '        pwr_button_.OnClick([this]() {\n'
                           '        });\n'
                           '    }\n'
                           '};\n')

    def test_three_way_dial_feeds_single_input_task(self):
        patched = self._board()
        # Schematic labels Button_Up GP4 / Button_Down GP6, but the
        # 2026-11-02 V1 trial showed the child-facing direction is the
        # opposite: physical up = GPIO6, physical down = GPIO4.
        self.assertIn('Button dial_up_{GPIO_NUM_6}', patched)
        self.assertIn('Button dial_press_{GPIO_NUM_5}', patched)
        self.assertIn('Button dial_down_{GPIO_NUM_4}', patched)
        self.assertIn('#include "kid_app/input_hub.h"', patched)
        self.assertIn('kid_app::InputHub input_hub_', patched)
        for key in ('DialUp', 'DialPress', 'DialDown', 'Boot', 'Power'):
            self.assertIn(f'PostKeyEdge(kid_app::InputKey::{key}, true)', patched)
            self.assertIn(f'PostKeyEdge(kid_app::InputKey::{key}, false)', patched)
        # Callbacks only enqueue; InputHub is owned by one task.
        self.assertIn('xQueueSend(pet_input_queue_, &edge, 0)', patched)
        self.assertIn('"pet_input"', patched)
        self.assertIn('input_hub_.Tick(NowMs())', patched)
        self.assertIn('PetInputRouter::DecideDial(', patched)
        self.assertIn('PET_INPUT kind=%s', patched)
        self.assertIn('PET_INPUT_TASK_FAILED', patched)
        # BOOT + dial press is the parent combo: legacy iot_button BOOT
        # handlers must not toggle chat/Pet mode while the dial is held.
        self.assertIn('static bool DialPressHeld()', patched)
        self.assertIn('if (DialPressHeld()) return;', patched)
        self.assertIn('if (!DialPressHeld()) RequestPetModeToggle();', patched)
        self.assertEqual(patched.count('!pet_mode_.load() || DialPressHeld()'), 2)
        # Voice sessions gate dial actions; dial use keeps the device awake.
        self.assertIn('input_hub_.SetVoiceActive(', patched)
        self.assertIn('kDeviceStateListening', patched)
        self.assertIn('power_save_timer_->WakeUp()', patched)
        # No new click-count gestures for children.
        self.assertEqual(patched.count('OnLongPress'), 1)
        self.assertEqual(patched.count('OnDoubleClick'), 1)
        self.assertEqual(patched.count('OnMultipleClick'), 1)

    def test_pwr_does_not_power_off_in_pet_mode(self):
        patched = self._board()
        start = patched.index('pwr_button_.OnClick([this]() {')
        body = patched[start:start + 400]
        self.assertIn('if (pet_mode_.load())', body)
        self.assertIn('PET_SLEEP_REQUEST_PENDING', body)
        self.assertIn('return;', body)

    def test_read_only_hardware_diagnostics(self):
        patched = self._board()
        self.assertIn('RunHardwareDiag()', patched)
        for addr in ('0x18', '0x34', '0x51', '0x6A', '0x6B', '0x70'):
            self.assertIn(addr, patched)
        self.assertIn('i2c_master_probe(i2c_bus_', patched)
        for marker in ('PET_DIAG_DIAL', 'PET_DIAG_I2C', 'PET_DIAG_IMU',
                       'PET_DIAG_ENV', 'PET_DIAG_RTC', 'PET_DIAG_PMIC'):
            self.assertIn(marker, patched)
        # Diagnostics must never write RTC time/alarm or PMIC registers.
        diag = patched[patched.index('void RunHardwareDiag()'):]
        diag = diag[:diag.index('\n    }\n')]
        self.assertNotIn('WriteReg', diag)
        self.assertNotIn('PowerOff', diag)
        self.assertIn('i2c_master_bus_rm_device', patched)
    def test_v1_round3_measurement_loop(self):
        patched = self._board()
        self.assertIn('#include "kid_app/rtc_clock.h"', patched)
        self.assertIn('#include "kid_app/motion_stats.h"', patched)
        self.assertIn('RunDiagLoop()', patched)
        # QMI8658: little-endian auto-increment, +/-4 g at 62.5 Hz, accel only.
        for cfg in ('{0x02, 0x40}', '{0x03, 0x17}', '{0x08, 0x01}'):
            self.assertIn(cfg, patched)
        for marker in ('PET_DIAG_ACC', 'PET_DIAG_HEARTBEAT', 'PET_DIAG_RTC_SYNC',
                       'PET_DIAG_WAKE', 'PET_DIAG_ALARM_TEST', 'PET_DIAG_EPD'):
            self.assertIn(marker, patched)
        # RTC is only written from a plausible system clock (server time).
        sync = patched[patched.index('void SyncRtcFromSystemTime()'):]
        sync = sync[:sync.index('\n    }\n')]
        self.assertIn('PlausibleWallClock', sync)
        # Timed power-on test: Pet mode + parent combo only, RTC must be set.
        self.assertIn('event.kind == kid_app::InputKind::ParentCombo && pet_mode_.load()',
                      patched)
        arm = patched[patched.index('void ArmRtcWakeTest()'):]
        arm = arm[:arm.index('\n    }\n')]
        self.assertIn('refused=rtc_not_set', arm)
        self.assertIn('pmic_->PowerOff()', arm)
        # A boot after the test clears and disables the alarm again, and
        # does so at board init: a held RTC INT on the PMIC power-on pin
        # would otherwise read as a long PWR press and cut power.
        board_init = patched.index('PET_FIRMWARE_BOOT_MARKER phase=board_init')
        self.assertIn('ClearRtcAlarm();', patched[board_init:board_init + 400])
        self.assertNotIn('self->ClearRtcAlarm()', patched)
        # Newlib-nano printf has no 64-bit integers.
        self.assertNotIn('%lld', patched)
        # Let the accelerometer produce a first sample before reading.
        self.assertIn('vTaskDelay(pdMS_TO_TICKS(50));  // first accel sample', patched)
        # PWR edges are counted even if no InputHub event follows.
        self.assertIn('pwr_edges_', patched)
        self.assertIn('sleep_requests_', patched)

    def test_round3_hardware_diag_stays_read_only(self):
        patched = self._board()
        diag = patched[patched.index('void RunHardwareDiag()'):]
        diag = diag[:diag.index('\n    }\n')]
        self.assertNotIn('PowerOff', diag)
        self.assertNotIn('EncodeTime', diag)
        self.assertNotIn('EncodeAlarm', diag)

    def test_diag_waits_fit_tick(self):
        patched = self._board()
        diag = patched[patched.index('void RunHardwareDiag()'):]
        diag = diag[:diag.index('\n    }\n')]
        # 100 Hz FreeRTOS tick: waits must stay >= SHTC3 wake/measure times.
        self.assertNotIn('pdMS_TO_TICKS(1)', diag)
        self.assertIn('measure, sizeof(measure), env, sizeof(env), 40)', diag)

if __name__ == "__main__":
    unittest.main()
