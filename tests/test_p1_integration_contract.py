"""Verify P1's narrow board overlay supports full scene rendering.

Does not need the ESP-IDF SDK; test patches on fixture snippets.
"""
import unittest
from pathlib import Path
from scripts.p1_integrate import patch_display_h, patch_display_cc, patch_board
from scripts.p1_integrate import patch_display_voice_h

ROOT = Path(__file__).resolve().parent.parent

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
        self.assertEqual(patched.count("bool ShowPetScene("), 1)

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
        self.assertIn('PET_PWR_CLICK_PET_MODE', body)
        self.assertIn('return;', body)
        # The sleep flow, not a pending marker, now owns SleepRequest.
        self.assertNotIn('PET_SLEEP_REQUEST_PENDING', patched)

    def test_v2_sleep_flow_wiring(self):
        patched = self._board()
        self.assertIn('#include "kid_app/sleep_flow.h"', patched)
        self.assertIn('kid_app::SleepFlow sleep_flow_', patched)
        handle = patched[patched.index('void HandleInputEvent('):]
        handle = handle[:handle.index('\n    }\n')]
        # While going to sleep, inputs only cancel (checked before anything
        # else, including the parent combo), and PWR starts the flow.
        self.assertLess(handle.index('sleep_flow_.OnInput('), handle.index('ArmRtcWakeTest()'))
        self.assertIn('sleep_flow_.Request(NowMs())', handle)
        # Home rail "Sleep" item also starts it.
        rail = patched[patched.index('PetInputDecision HandleHomeRailLocked('):]
        rail = rail[:rail.index('\n    }\n')]
        self.assertIn('kid_app::HomeItem::Sleep', rail)
        self.assertIn('*begin_sleep = true', rail)
        loop = patched[patched.index('void InputTaskLoop()'):]
        loop = loop[:loop.index('\n    }\n')]
        self.assertIn('sleep_flow_.OnSceneShown(', loop)
        self.assertIn('sleep_flow_.Tick(NowMs())', loop)
        # The EPD worker reports when the sleep scene is on the panel.
        self.assertIn('kid_app::SceneKind::Sleep', patched)
        self.assertIn('PET_SLEEP_SHOWN', patched)
        # Pet commits do not overwrite the sleep scene.
        self.assertIn('effects.repaint && !self->sleep_active_.load()', patched)
        off = patched[patched.index('void PowerOffForSleep()'):]
        off = off[:off.index('\n    }\n')]
        self.assertLess(off.index('pet_service_.Settled()'), off.index('pmic_->PowerOff()'))
        self.assertLess(off.index('CancelLocalStoryPcm()'), off.index('pmic_->PowerOff()'))
        self.assertIn('ArmRtcAlarmIn(', off)
        self.assertIn('PET_SLEEP_POWEROFF_RETURNED', off)
        self.assertIn('sleep_flow_.PowerOffReturned()', off)
        for marker in ('PET_SLEEP_REQUESTED', 'PET_SLEEP_CANCELLED', 'PET_SLEEP_POWEROFF'):
            self.assertIn(marker, patched)

    def test_gestures_and_rhythm_wiring(self):
        patched = self._board()
        for header in ('gesture_detector.h', 'rhythm_scheduler.h'):
            self.assertIn(f'#include "kid_app/{header}"', patched)
        diag = patched[patched.index('void RunDiagLoop()'):]
        diag = diag[:diag.index('\n    }\n')]
        # IMU windows feed the detector only while Pet mode is awake.
        self.assertIn('gesture_detector_.OnWindow(s, now)', diag)
        self.assertIn('pet_mode_.load() && !sleep_active_.load()', diag)
        self.assertIn('UpdateRhythm()', diag)
        gesture = patched[patched.index('void HandleGesture('):]
        gesture = gesture[:gesture.index('\n    }\n')]
        self.assertIn('kDeviceStateIdle', gesture)
        # ADR-011: only pickup is a durable action; shakes are reactions.
        self.assertIn('PostLocal(kid_app::PetAction::Pickup)', gesture)
        self.assertEqual(gesture.count('PostLocal('), 1)
        self.assertIn('kid_app::Animation::Giggle', gesture)
        self.assertIn('kid_app::Animation::Dizzy', gesture)
        self.assertIn('face_down_.exchange(false)', gesture)
        self.assertIn('HandleGesture(static_cast<kid_app::Gesture>(step.ms), "selftest")', patched)
        # ADR-010: sleep arms the next morning unless a self-test alarm is set.
        off = patched[patched.index('void PowerOffForSleep()'):]
        off = off[:off.index('\n    }\n')]
        self.assertLess(off.index('sleep_alarm_s_.exchange(0)'), off.index('SecondsUntilWake()'))
        self.assertIn('ArmRtcAlarmIn(wake_s)', off)
        # Quiet hours lower the prompts.
        self.assertIn('quiet_hours_.load() ?', patched)

    def test_animations_and_deferred_cleanup(self):
        patched = self._board()
        self.assertIn('#include "kid_app/pet_animation.h"', patched)
        self.assertIn('pet_display_worker_.Start(&PetRender, this, &PetIdle)', patched)
        # Gate scenes and director frames share one drawing path.
        render = patched[patched.index('static bool PetRender('):]
        render = render[:render.index('\n    }\n')]
        self.assertIn('DrawPetFrameLocked(scene, "gate")', render)
        idle = patched[patched.index('static void PetIdle('):]
        idle = idle[:idle.index('\n    }\n')]
        self.assertIn('DrawPetFrameLocked(step.scene', idle)
        # Blink/cleanup only on a quiet, awake home screen; cleanup is full.
        self.assertIn('!self->sleep_active_.load()', idle)
        self.assertIn('kDeviceStateIdle', idle)
        self.assertLess(idle.index('refresh_planner_.ForceFull()'),
                        idle.index('DrawPetFrameLocked(step.scene'))
        # The director lock is never held while drawing.
        self.assertLess(idle.index('xSemaphoreTake(self->pet_screen_mutex_'),
                        idle.index('DrawPetFrameLocked(step.scene'))
        # Triggers: feed -> hearts, hello -> wave, sleep -> Zzz one by one.
        self.assertIn('RequestAnimation(kid_app::Animation::Feed, scene)', patched)
        self.assertIn('RequestAnimation(kid_app::Animation::Greet, scene)', patched)
        self.assertIn('scene.pose = kid_app::ScenePose::Zzz1', patched)
        self.assertIn('RequestAnimation(kid_app::Animation::SleepIn, scene)', patched)
        # The cancel window starts only after the final sleep picture.
        self.assertIn('scene.pose == kid_app::ScenePose::Still', patched)

    def test_v2_sleep_flow_review_fixes(self):
        patched = self._board()
        # Each sleep request is a distinct frame, so the refresh gate never
        # skips a second request as "already shown".
        self.assertIn('scene.book_page = ++sleep_seq_', patched)
        # No story narration while going to sleep.
        for fn in ('static bool StoryAudioBegin(', 'static bool StoryAudioPush('):
            body = patched[patched.index(fn):]
            body = body[:body.index('\n    }\n')]
            self.assertIn('self->sleep_active_.load()', body)
        self.assertIn('if (okay && !sleep_active_.load()) story_pcm_.RequestPage(', patched)
        # Leaving Pet mode aborts the flow; it never powers off from XiaoZhi.
        loop = patched[patched.index('void InputTaskLoop()'):]
        loop = loop[:loop.index('\n    }\n')]
        self.assertLess(loop.index('!pet_mode_.load() && sleep_flow_.Cancel()'),
                        loop.index('sleep_flow_.Tick(NowMs())'))
        # After a returned power-off: drop presses queued while blocked and
        # keep the boot's wake reason.
        off = patched[patched.index('void PowerOffForSleep()'):]
        off = off[:off.index('\n    }\n')]
        self.assertIn('xQueueReset(pet_input_queue_)', off)
        self.assertIn('input_hub_ = kid_app::InputHub{}', off)
        self.assertIn('ClearRtcAlarm(false)', off)
        self.assertIn('std::atomic<int> woke_by_alarm_', patched)

    def test_alarm_wake_note_survives_usb_chip_reset(self):
        # V2-09 trial: after the alarm power-on, the host reopening USB
        # caused rst:0x15 (USB_UART_CHIP_RESET); the second boot no longer
        # saw AF and re-ran the scenario. Keep a short-lived note in
        # RTC no-init memory (kept across chip resets, lost on power loss).
        patched = self._board()
        self.assertIn('RTC_NOINIT_ATTR static uint32_t g_pet_alarm_wake_magic', patched)
        self.assertIn('RTC_NOINIT_ATTR static uint32_t g_pet_alarm_wake_epoch', patched)
        self.assertIn('#include "esp_attr.h"', patched)
        note = patched[patched.index('bool NoteAlarmWake('):]
        note = note[:note.index('\n    }\n')]
        self.assertIn('esp_reset_reason() == ESP_RST_POWERON', note)
        self.assertIn('kAlarmWakeNoteSeconds', note)
        self.assertIn('static constexpr uint32_t kAlarmWakeNoteSeconds = 60;', patched)
        clear = patched[patched.index('void ClearRtcAlarm('):]
        clear = clear[:clear.index('\n    }\n')]
        self.assertIn('NoteAlarmWake(alarm_flag', clear)

    def test_v2_option_prompts_wiring(self):
        patched = self._board()
        self.assertIn('#include "kid_app/prompt_scheduler.h"', patched)
        self.assertIn('#include "kid_app/prompt_clips.h"', patched)
        self.assertIn('kid_app::PromptScheduler prompt_scheduler_', patched)
        rail = patched[patched.index('PetInputDecision HandleHomeRailLocked('):]
        rail = rail[:rail.index('\n    }\n')]
        self.assertIn('prompt_scheduler_.OnSelect(index, NowMs())', rail)
        # A press acts at once: the pending/ongoing name prompt stops.
        press = rail[rail.index('InputKind::DialPress) return {};'):]
        self.assertLess(press.index('StopPrompt()'), press.index('DecideHome('))
        loop = patched[patched.index('void InputTaskLoop()'):]
        loop = loop[:loop.index('\n    }\n')]
        self.assertIn('prompt_scheduler_.SetEnabled(', loop)
        self.assertIn('kid_app::PetInputRouter::IsHomeRail(', loop)
        self.assertIn('prompt_scheduler_.Tick(NowMs(), &prompt)', loop)
        # Going to sleep silences prompts too.
        sleep = patched[patched.index('case kid_app::SleepAction::ShowSleepScene:'):]
        self.assertIn('StopPrompt()', sleep[:600])

    def test_v2_prompt_player_uses_the_single_audio_path(self):
        patched = self._board()
        player = patched[patched.index('void PromptTaskLoop()'):]
        player = player[:player.index('\n    }\n')]
        self.assertIn('LoadPromptClip(', player)
        self.assertIn('BeginLocalStoryPcm(session)', player)
        self.assertIn('TryQueueLocalStoryPcm(session, chunk, n)', player)
        self.assertIn('clip.Copy(offset, chunk, 480)', player)
        # Self-test "volume" scales prompt PCM in RAM, before it is queued;
        # the XiaoZhi codec volume (persisted in NVS) is never changed.
        self.assertLess(player.index('ScalePcm(chunk, n, PromptVolumePct())'),
                        player.index('TryQueueLocalStoryPcm(session, chunk, n)'))
        self.assertNotIn('SetOutputVolume', patched)
        self.assertIn('case kid_app::SelfTestOp::Volume:', patched)
        # Newest request wins: older prompt audio is invalidated first and
        # a waiting request interrupts the current one.
        self.assertLess(player.index('CancelLocalStoryPcm()'), player.index('BeginLocalStoryPcm('))
        self.assertIn('uxQueueMessagesWaiting(prompt_queue_)', player)
        for marker in ('PET_PROMPT_PLAY', 'PET_PROMPT_QUEUED', 'PET_PROMPT_INTERRUPTED',
                       'PET_PROMPT_FAILED'):
            self.assertIn(marker, player)
        self.assertIn('xQueueOverwrite(prompt_queue_', patched)
        self.assertIn('"pet_prompt"', patched)
        # Prompt sessions never collide with story-page tokens.
        self.assertIn('0x50524F4D', player)
        # Same guard as story narration (pet mode, not asleep, idle) before
        # starting and while pushing, so a voice session is never talked over.
        allowed = patched[patched.index('bool PromptAllowed()'):]
        allowed = allowed[:allowed.index('\n    }\n')]
        for cond in ('pet_mode_.load()', '!sleep_active_.load()', 'kDeviceStateIdle'):
            self.assertIn(cond, allowed)
        self.assertLess(player.index('PromptAllowed()'), player.index('BeginLocalStoryPcm('))
        push = player[player.index('while (offset < clip.samples)'):]
        self.assertIn('PromptAllowed()', push)
        self.assertIn('stalls > 50', push)
        self.assertIn('reason=state', player)
        self.assertIn('"pet_prompt", 6144', patched)

    def test_selftest_alarm_and_no_rerun_after_alarm_wake(self):
        patched = self._board()
        run = patched[patched.index('void MaybeRunSelfTest()'):]
        run = run[:run.index('\n    }\n')]
        self.assertIn('woke_by_alarm_.load() == 1', run)
        self.assertIn('reason=alarm_wake', run)
        self.assertIn('kid_app::SelfTestOp::Alarm', run)
        self.assertIn('sleep_alarm_s_.store(', run)

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
        # V2-09: the RTC-not-set refusal lives in the shared alarm helper.
        self.assertIn('if (!ArmRtcAlarmIn(90))', arm)
        self.assertIn('pmic_->PowerOff()', arm)
        helper = patched[patched.index('bool ArmRtcAlarmIn('):]
        helper = helper[:helper.index('\n    }\n')]
        self.assertIn('refused=rtc_not_set', helper)
        self.assertIn('PlausibleWallClock', helper)
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

    def test_v2_partial_refresh_display_path(self):
        header = patch_display_h('#include "lcd_display.h"\nclass Dummy {\n'
                                 '    void EPD_Sleep();\n    uint8_t* buffer = NULL;\n};\n')
        self.assertIn('bool ShowPetSceneFast(const kid_app::SceneModel& scene);', header)
        impl = patch_display_cc('#include <vector>\nvoid Flush() {\n    driver->EPD_Clear();\n}\n')
        fast = impl[impl.index('bool CustomEpdDisplay::ShowPetSceneFast'):]
        fast = fast[:fast.index('\n}\n')]
        # New frame into RAM 0x24, partial waveform, then the same frame into
        # RAM 0x26 so the next partial diffs against what is on the panel.
        self.assertLess(fast.index('EPD_SendCommand(0x24)'), fast.index('EPD_TurnOnDisplayPart()'))
        self.assertLess(fast.index('EPD_TurnOnDisplayPart()'), fast.index('EPD_SendCommand(0x26)'))
        self.assertIn('lvgl_port_lock(5000)', fast)
        self.assertIn('heap_caps_free(memory)', fast)

    def test_v2_home_rail_board_wiring(self):
        patched = self._board()
        self.assertIn('#include "kid_app/home_menu.h"', patched)
        self.assertIn('#include "kid_app/refresh_planner.h"', patched)
        self.assertIn('kid_app::HomeMenu home_menu_', patched)
        self.assertIn('kid_app::RefreshPlanner refresh_planner_', patched)
        self.assertIn('PetInputRouter::IsHomeRail(', patched)
        self.assertIn('PetInputRouter::DecideHome(', patched)
        self.assertIn('home_menu_.Move(', patched)
        self.assertIn('PET_HOME_SELECT', patched)
        # Selection survives the packed atomic scene snapshot.
        self.assertIn('scene.home_selection', patched)
        # Partial refreshes need a short pacing interval.
        self.assertIn('pet_display_worker_{pet_mailbox_, 500}', patched)
        self.assertNotIn('pet_display_worker_{pet_mailbox_, 5000}', patched)
        self.assertIn('refresh_planner_.Plan(scene)', patched)
        self.assertIn('ShowPetSceneFast(scene)', patched)
        self.assertIn('refresh_planner_.ForceFull()', patched)
        self.assertIn('kid_app::PackScene(scene)', patched)
        self.assertIn('kid_app::UnpackScene(bits)', patched)

    def test_v2_scene_state_is_published_under_one_lock(self):
        patched = self._board()
        commit = patched[patched.index('static void PetCommitted('):]
        commit = commit[:commit.index('\n    }\n')]
        locked = commit[commit.index('SceneStateLock lock(self->scene_state_mutex_)'):]
        # Scene stored and published before the mode becomes visible.
        self.assertLess(locked.index('last_pet_scene_.store('), locked.index('pet_event_mode_.store('))
        self.assertLess(locked.index('pet_mailbox_.Publish('), locked.index('pet_event_mode_.store('))
        self.assertEqual(commit.count('pet_event_mode_.store('), 1)
        handle = patched[patched.index('void HandleInputEvent('):]
        handle = handle[:handle.index('\n    }\n')]
        self.assertIn('SceneStateLock lock(scene_state_mutex_)', handle)
        # Durable action posted after the lock scope closes.
        self.assertGreater(handle.index('PostLocal(decision.action)'),
                           handle.index('SceneStateLock lock(scene_state_mutex_)'))
        self.assertNotIn('compare_exchange', patched)

    def test_selftest_script_runner(self):
        patched = self._board()
        self.assertIn('#include "kid_app/selftest_script.h"', patched)
        self.assertIn('#include "esp_ota_ops.h"', patched)
        run = patched[patched.index('void MaybeRunSelfTest()'):]
        run = run[:run.index('\n    }\n')]
        # Only ever from the running ota_1 slot's last 4 KiB sector.
        self.assertIn('esp_ota_get_running_partition()', run)
        self.assertIn('ESP_PARTITION_SUBTYPE_APP_OTA_1', run)
        self.assertIn('running->size - kSelfTestSectorBytes', run)
        self.assertIn('ParseSector(', run)
        # Injected presses go through the same raw-edge queue as real keys.
        self.assertIn('PostKeyEdge(step.key, true)', run)
        self.assertIn('PostKeyEdge(step.key, false)', run)
        self.assertIn('RequestPetModeToggle()', run)
        for marker in ('PET_SELFTEST_NONE', 'PET_SELFTEST_START', 'PET_SELFTEST_STEP',
                       'PET_SELFTEST_DONE'):
            self.assertIn(marker, run)
        # Never writes flash itself.
        self.assertNotIn('esp_partition_write', patched)
        self.assertNotIn('esp_partition_erase', patched)
        self.assertIn('"pet_selftest"', patched)

    def test_v2_border_waveform_per_refresh_mode(self):
        impl = patch_display_cc('#include <vector>\nvoid Flush() {\n    driver->EPD_Clear();\n}\n')
        fast = impl[impl.index('bool CustomEpdDisplay::ShowPetSceneFast'):]
        fast = fast[:fast.index('\n}\n')]
        self.assertLess(fast.index('EPD_SendData(0x80)'), fast.index('EPD_SendCommand(0x24)'))
        self.assertEqual(impl.count('EPD_SendData(0x01);\n            EPD_DisplayPartBaseImage();'), 2)

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

    def test_v6_display_counts_recognised_sentences(self):
        header = patch_display_voice_h(
            '#include <driver/gpio.h>\n#include "lcd_display.h"\n'
            'class CustomEpdDisplay : public LcdDisplay {\npublic:\n'
            '    void EPD_DrawColorPixel(uint16_t x, uint16_t y, uint8_t color);\n};\n')
        self.assertIn('#include <atomic>', header)
        hook = header[header.index('void SetChatMessage('):]
        hook = hook[:hook.index('\n    }\n')]
        # Counts "user" (stt) messages only and always forwards to LVGL; the
        # sentence text itself is never stored or logged by the pet.
        self.assertIn('std::strcmp(role, "user") == 0', hook)
        self.assertIn('user_utterances_.fetch_add(1)', hook)
        self.assertIn('LcdDisplay::SetChatMessage(role, content);', hook)
        self.assertNotIn('ESP_LOG', hook)
        self.assertIn('uint32_t UserUtterances() const', header)
        # run() applies it on top of the pet display header.
        source = (ROOT / 'scripts' / 'p1_integrate.py').read_text(encoding='utf-8')
        self.assertIn('patch_display_voice_h(patch_display_h(', source)

    def test_v6_voice_faces_wiring(self):
        patched = self._board()
        self.assertIn('#include "kid_app/voice_face.h"', patched)
        update = patched[patched.index('void UpdateVoiceFace('):]
        update = update[:update.index('\n    }\n')]
        # XiaoZhi states -> link; "thinking" from recognised sentences.
        self.assertIn('kDeviceStateConnecting || state == kDeviceStateListening', update)
        self.assertIn('state == kDeviceStateSpeaking', update)
        self.assertIn('display_->UserUtterances()', update)
        self.assertIn('voice_tracker_.Update(link, heard, NowMs(), &face)', update)
        # Off outside Pet mode / while going to sleep; display_ only read in Pet mode.
        self.assertIn('if (!pet || sleep_active_.load()) link = kid_app::VoiceLink::Off;', update)
        self.assertIn('(pet ? display_->UserUtterances() : 0)', update)
        # UI-only overlay: published under the scene lock from the durable
        # picture, never a durable Pet action or NVS write.
        locked = update[update.index('SceneStateLock lock(scene_state_mutex_)'):]
        self.assertIn('voice_face_.store(', locked)
        self.assertIn('pet_mailbox_.Publish(WithVoiceFace(UnpackPetScene(last_pet_scene_.load())))',
                      locked)
        self.assertNotIn('PostLocal', update)
        self.assertNotIn('last_pet_scene_.store', update)
        self.assertIn('PET_VOICE_FACE face=%s', update)
        loop = patched[patched.index('void InputTaskLoop()'):]
        loop = loop[:loop.index('\n    }\n')]
        self.assertIn('UpdateVoiceFace(state);', loop)
        # Committed scenes keep the face while a voice session runs.
        commit = patched[patched.index('static void PetCommitted('):]
        commit = commit[:commit.index('\n    }\n')]
        self.assertIn('pet_mailbox_.Publish(self->WithVoiceFace(scene))', commit)
        self.assertIn('last_pet_scene_.store(PackPetScene(scene))', commit)
        # No idle blink or cleanup on a voice face.
        idle = patched[patched.index('static void PetIdle('):]
        idle = idle[:idle.index('\n    }\n')]
        self.assertIn('self->voice_face_.load() == 0', idle)

    def test_v6_boot_talks_on_the_pet_home_screen(self):
        patched = self._board()
        click = patched[patched.index('boot_button_.OnClick('):]
        click = click[:click.index('\n        });\n')]
        pet = click[click.index('if (pet_mode_.load()) {'):]
        # Home rail: BOOT starts/stops XiaoZhi (not while going to sleep);
        # quiet time and the reader keep their single-click routing.
        self.assertIn('PetInputRouter::IsHomeRail(pet_mode)', pet)
        self.assertIn('if (!sleep_active_.load()) app.ToggleChatState();', pet)
        self.assertLess(pet.index('app.ToggleChatState();'), pet.index('PetInputRouter::Decide('))
        self.assertIn('if (DialPressHeld()) return;', click)
        self.assertLess(click.index('if (DialPressHeld()) return;'), click.index('ToggleChatState'))

    def test_v6_selftest_voice_step(self):
        patched = self._board()
        run = patched[patched.index('void MaybeRunSelfTest()'):]
        run = run[:run.index('\n    }\n')]
        voice = run[run.index('case kid_app::SelfTestOp::Voice:'):]
        voice = voice[:voice.index('break;')]
        self.assertIn('selftest_heard_.fetch_add(1)', voice)
        self.assertIn('selftest_voice_.store(', voice)
        self.assertIn('PET_SELFTEST_VOICE phase=%s', voice)
        # Forced phases go through the same tracker as XiaoZhi's states.
        update = patched[patched.index('void UpdateVoiceFace('):]
        update = update[:update.index('\n    }\n')]
        self.assertIn('selftest_voice_.load()', update)
        self.assertIn('selftest_heard_.load()', update)
        scenario = (ROOT / 'tools' / 'selftest' / 'v6_voice_faces.pettest').read_text(
            encoding='ascii')
        self.assertTrue(scenario.startswith('PETSELFTEST1\n'))
        for phase in ('voice listen', 'voice think', 'voice speak', 'voice off'):
            self.assertIn(phase + '\n', scenario)
        self.assertIn('# expect partial_refreshes 4', scenario)

if __name__ == "__main__":
    unittest.main()
