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
        self.assertIn('struct GatedSceneMailbox : kid_app::FreertosPetSceneMailbox', patched)
        self.assertIn('GatedSceneMailbox pet_mailbox_', patched)
        self.assertIn('FreertosAtomicPetService pet_service_', patched)
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
        self.assertLess(handle.index('sleep_flow_.OnInput('), handle.index('OpenParentMenu()'))
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
        # Quiet hours lower the prompts (ceiling from the parent settings).
        self.assertIn('kid_app::LimitVolumePct(ParentSettingsNow(), prompt_volume_pct_.load(),',
                      patched)
        self.assertNotIn('rhythm_.', patched)

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
        play = patched[patched.index('void PlayBookClip('):]
        self.assertIn('if (sleep_active_.load()) return;', play[:play.index('\n    }\n')])
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
        # V2-08: option names come from the TF pack or the embedded clips.
        self.assertIn('sound_bank_.LoadOption(request.index, &clip)', player)
        self.assertIn('kid_app::SoundBank sound_bank_{&kid_app::LoadPromptClip}', patched)
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
        # PET_PROMPT_* / PET_PHRASE_* outcome lines share one format.
        self.assertIn('PET_PROMPT_PLAY index=%u', player)
        self.assertIn('kind = phrase ? "PHRASE" : "PROMPT"', player)
        for marker in ('PET_%s_QUEUED', 'PET_%s_INTERRUPTED', 'PET_%s_FAILED'):
            self.assertIn(marker, player)
        self.assertIn('xQueueOverwrite(prompt_queue_', patched)
        self.assertIn('"pet_prompt"', patched)
        # Prompt sessions never collide with story-page tokens.
        self.assertIn('0x50524F4D', player)
        # Same guard as story narration (pet mode, not asleep, idle) before
        # starting and while pushing, so a voice session is never talked over.
        allowed = patched[patched.index('bool PromptAllowed('):]
        allowed = allowed[:allowed.index('\n    }\n')]
        for cond in ('pet_mode_.load()', '!sleep_active_.load()', 'kDeviceStateIdle'):
            self.assertIn(cond, allowed)
        self.assertLess(player.index('PromptAllowed(bedtime)'),
                        player.index('BeginLocalStoryPcm('))
        push = player[player.index('while (offset < clip.samples)'):]
        self.assertIn('PromptAllowed(bedtime)', push)
        self.assertIn('stalls > 50', push)
        self.assertIn('reason=state', player)
        self.assertIn('"pet_prompt", 6144', patched)

    def test_v2_sound_pack_loaded_by_the_prompt_task(self):
        patched = self._board()
        for header in ('sound_bank.h', 'pet_phrases.h'):
            self.assertIn(f'#include "kid_app/{header}"', patched)
        # The SD card is mounted once by the story reader; the prompt task
        # opens the pack after that attempt, read-only, and only there.
        init = self._body(patched, 'bool InitializePetRuntime()')
        self.assertLess(init.index('story_sd_.Initialize('), init.index('sound_sd_.store('))
        self.assertIn('story_sd_.Mounted() ? kSoundSdMounted : kSoundSdAbsent', init)
        load = self._body(patched, 'void LoadSoundPack()')
        self.assertIn('sound_file_.Open("/sdcard/sounds", "default")', load)
        self.assertIn('sound_pack_.Open(sound_file_)', load)
        self.assertIn('sound_bank_.UsePack(&sound_pack_)', load)
        for state in ('"no_sd"', '"absent"', '"invalid"', '"ready"', 'PET_SOUND_PACK'):
            self.assertIn(state, load)
        player = self._body(patched, 'void PromptTaskLoop()')
        self.assertLess(player.index('LoadSoundPack()'), player.index('xQueueReceive('))
        self.assertEqual(patched.count('LoadSoundPack()'), 2)  # definition + player
        # A TF read error ends the clip instead of spinning.
        self.assertLess(player.index('if (n == 0) break;'),
                        player.index('ScalePcm(chunk, n, PromptVolumePct())'))
        self.assertNotIn('fopen', patched)

    def test_v5_pet_lines_share_the_prompt_path(self):
        patched = self._board()
        player = self._body(patched, 'void PromptTaskLoop()')
        # The line is chosen (Personality, via the planner) before older
        # audio is cut, so a dropped line never silences a prompt.
        self.assertLess(player.index('phrase_planner_.Next(context, NowMs())'),
                        player.index('CancelLocalStoryPcm()'))
        self.assertIn('PET_PHRASE_SKIPPED', player)
        self.assertIn('sound_bank_.LoadPhrase(phrase_id, &clip)', player)
        self.assertIn('PET_PHRASE_PLAY id=%u ctx=%s src=%s', player)
        self.assertIn('0x50485253u', player)  # own session ids
        # Same volume path as the option names (self-test volume, quiet
        # hours); the XiaoZhi codec volume is never touched.
        self.assertEqual(player.count('ScalePcm('), 1)
        self.assertNotIn('SetOutputVolume', patched)
        allowed = self._body(patched, 'bool PromptAllowed(bool bedtime = false)')
        self.assertIn('(bedtime || !sleep_active_.load())', allowed)
        self.assertIn('kDeviceStateIdle', allowed)
        post = self._body(patched, 'void PostPhrase(')
        for guard in ('!pet_mode_.load()', 'napping_.load()',
                      'sleep_active_.load() && context != kid_app::PhraseContext::Bedtime'):
            self.assertIn(guard, post)
        self.assertIn('PostPrompt(request)', post)
        self.assertNotIn('LocalStoryPcm', post)
        # Where the pet speaks.
        commit = self._body(patched, 'static void PetCommitted(')
        self.assertGreater(commit.index('PostPhrase(kid_app::PhraseContext::Feed)'),
                           commit.index('pet_event_mode_.store('))
        self.assertIn('PostPhrase(kid_app::PhraseContext::Greeting)', commit)
        gesture = self._body(patched, 'void HandleGesture(')
        self.assertIn('if (acted) PostPhrase(kid_app::PhraseContext::Giggle)', gesture)
        self.assertEqual(gesture.count('PostLocal('), 1)
        begin = self._body(patched, 'void BeginMorning(')
        self.assertIn('PostPhrase(kid_app::PhraseContext::MorningRecap)', begin)
        sleep = patched[patched.index('case kid_app::SleepAction::ShowSleepScene:'):]
        sleep = sleep[:sleep.index('return;')]
        self.assertLess(sleep.index('StopPrompt()'),
                        sleep.index('PostPhrase(kid_app::PhraseContext::Bedtime)'))
        self.assertLess(sleep.index('CancelLocalStoryPcm()'),
                        sleep.index('PostPhrase(kid_app::PhraseContext::Bedtime)'))
        restore = patched[patched.index('case kid_app::SleepAction::RestoreScene:'):]
        self.assertIn('StopPrompt()', restore[:restore.index('return;')])
        off = self._body(patched, 'void PowerOffForSleep()')
        self.assertLess(off.index('StopPrompt()'), off.index('pmic_->PowerOff()'))
        # Lines come from the reviewed table only: no text, no TTS here.
        self.assertNotIn('PhraseTemplate', patched)
        self.assertNotIn('.text', player)

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
                       'PET_DIAG_WAKE', 'PET_DIAG_EPD'):
            self.assertIn(marker, patched)
        # RTC is only written from a plausible system clock (server time).
        sync = patched[patched.index('void SyncRtcFromSystemTime()'):]
        sync = sync[:sync.index('\n    }\n')]
        self.assertIn('PlausibleWallClock', sync)
        # V3-06: the parent combo opens the parent menu; the V1-05 timed
        # power-on test is gone (self-test "alarm" steps cover the RTC wake).
        self.assertIn('event.kind == kid_app::InputKind::ParentCombo && pet_mode_.load()',
                      patched)
        self.assertNotIn('ArmRtcWakeTest', patched)
        self.assertNotIn('PET_DIAG_ALARM_TEST', patched)
        # V2-09: the RTC-not-set refusal lives in the shared alarm helper.
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

    def test_v2_status_bar_wiring(self):
        patched = self._board()
        self.assertIn('#include "kid_app/status_bar.h"', patched)
        self.assertIn('#include <wifi_manager.h>', patched)
        # Readings: heartbeat on pet_boot_diag, SHTC3 with CRC, PMIC, Wi-Fi,
        # quiet hours; published as one packed word.
        loop = patched[patched.index('void RunDiagLoop()'):]
        loop = loop[:loop.index('\n    }\n')]
        self.assertLess(loop.index('UpdateRhythm();'), loop.index('UpdateStatusBar();'))
        update = patched[patched.index('void UpdateStatusBar()'):]
        update = update[:update.index('\n    }\n')]
        for needle in ('RtcRead(&rtc, &stopped)', 'PlausibleWallClock(rtc)',
                       'ReadShtc3TempC10(&env_temp_c10_)', 'pmic_->GetBatteryLevel()',
                       'pmic_->IsCharging()', 'quiet_hours_.load()',
                       'WifiManager::GetInstance().IsConnected()',
                       'kid_app::MakeStatusBar(in, status_latest_)',
                       'status_bits_.store(kid_app::PackStatus(bar))', 'PET_STATUS '):
            self.assertIn(needle, update)
        self.assertIn('if (pmic_) {', update)  # board without PMIC
        self.assertIn('now - env_read_ms_ >= kEnvReadMs', update)  # SHTC3 once a minute
        shtc3 = patched[patched.index('bool ReadShtc3TempC10('):]
        shtc3 = shtc3[:shtc3.index('\n    }\n')]
        self.assertIn('kid_app::Shtc3TempC10(env, temp_c10)', shtc3)
        self.assertIn('measure, sizeof(measure), env, sizeof(env), 40)', shtc3)
        self.assertNotIn('pdMS_TO_TICKS(1)', shtc3)
        # Drawing: only from PetIdle when the director has nothing to draw
        # on a quiet, awake home screen (ambient), never between two frames
        # and never while going to sleep.
        idle = patched[patched.index('static void PetIdle('):]
        idle = idle[:idle.index('\n    }\n')]
        none = idle.index('if (step.kind == kid_app::DirectorStep::Kind::None) {')
        self.assertIn('if (ambient) self->MaybeRefreshStatus(displayed);', idle[none:none + 140])
        status = patched[patched.index('void MaybeRefreshStatus('):]
        status = status[:status.index('\n    }\n')]
        self.assertIn('displayed.kind != kid_app::SceneKind::Home', status)
        self.assertIn('status_schedule_.Due(status_shown_, latest, NowMs())', status)
        self.assertIn('anim_director_.Active()', status)
        self.assertLess(status.index('xSemaphoreTake(pet_screen_mutex_'),
                        status.index('DrawPetFrameLocked(displayed, "status", &latest)'))
        self.assertIn('pet_mode_.load() && !sleep_active_.load()', status)
        # Only status refreshes (also failed ones) start the one-minute wait.
        self.assertLess(status.index('xSemaphoreGive(pet_screen_mutex_)'),
                        status.index('status_schedule_.Drawn(NowMs());'))
        self.assertEqual(patched.count('status_schedule_.Drawn('), 1)
        # One drawing path: home frames carry the shown status, a status
        # refresh the new one, a full refresh the newest; counted per region.
        draw = patched[patched.index('bool DrawPetFrameLocked('):]
        draw = draw[:draw.index('\n    }\n')]
        self.assertIn('kid_app::StatusBar* status = nullptr', draw)
        self.assertIn(': status_shown_;', draw)
        full = draw.index('if (mode == kid_app::RefreshMode::Full) scene.status = NewestStatus(')
        self.assertLess(draw.index('refresh_planner_.Plan(scene)'), full)
        self.assertLess(full, draw.index('display_->ShowPetScene(scene)'))
        self.assertLess(draw.index('refresh_planner_.Completed(scene, mode, okay)'),
                        draw.index('status_shown_ = scene.status'))
        self.assertIn('refresh_planner_.StatusPartials()', draw)
        # The packed scene word stays as it was; status travels separately.
        self.assertIn('kid_app::PackScene(scene)', patched)
        self.assertIn('std::atomic<uint32_t> status_bits_', patched)

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
        # Full border on every full 1-bit picture: pet scene, story page and
        # the V4-05 book page.
        self.assertEqual(impl.count('EPD_SendData(0x01);\n            EPD_DisplayPartBaseImage();'), 3)

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

    @staticmethod
    def _body(text, start):
        body = text[text.index(start):]
        return body[:body.index('\n    }\n')]

    def test_v3_power_manager_wiring(self):
        patched = self._board()
        self.assertIn('#include "kid_app/power_manager.h"', patched)
        self.assertIn('kid_app::PowerManager power_;', patched)
        # pet_input owns it: inputs first, then the timers, then the sleep flow.
        loop = self._body(patched, 'void InputTaskLoop()')
        self.assertLess(loop.index('HandleInputEvent(event)'), loop.index('UpdatePower(state)'))
        self.assertLess(loop.index('UpdatePower(state)'), loop.index('sleep_flow_.Tick(NowMs())'))
        # Every input is activity, before the sleep flow sees it; only a dial
        # touch that ends a nap / the morning is swallowed.
        handle = self._body(patched, 'void HandleInputEvent(')
        self.assertLess(handle.index('power_.OnActivity(NowMs())'),
                        handle.index('sleep_flow_.OnInput('))
        self.assertIn('IsDialInput(event.kind)', handle)
        self.assertIn('power_save_timer_->WakeUp()', handle)
        # Gestures from pet_boot_diag only poke; pet_input applies it.
        gesture = self._body(patched, 'void HandleGesture(')
        self.assertIn('power_poke_.store(true)', gesture)
        update = self._body(patched, 'void UpdatePower(')
        self.assertIn('power_poke_.exchange(false)', update)
        self.assertIn('state != kDeviceStateIdle || sleep_flow_.Active()', update)
        # Idle power-off only on battery and never during a scripted trial.
        self.assertIn('on_battery_.load() && !selftest_seen_.load()', update)
        self.assertIn('day_phase_.load()', update)
        self.assertIn('power_.Tick(now, conditions)', update)
        self.assertIn('power_.Reset(now)', update)
        diag = self._body(patched, 'void RunDiagLoop()')
        self.assertIn('on_battery_.store(pmic_ && pmic_->IsDischarging())', diag)
        run = self._body(patched, 'void MaybeRunSelfTest()')
        self.assertLess(run.index('selftest_seen_.store(true)'), run.index('PET_SELFTEST_START'))
        # Power-off always goes through the sleep flow (picture, cancel
        # window, RTC alarm), never straight to the PMIC.
        events = self._body(patched, 'bool HandlePowerEvent(')
        self.assertIn('HandleSleepAction(sleep_flow_.Request(NowMs()))', events)
        self.assertNotIn('PowerOff', events.replace('PET_POWER', ''))
        for marker in ('PET_POWER state=idle', 'PET_POWER state=nap', 'PET_POWER_SLEEP'):
            self.assertIn(marker, events)
        # Drowsy face and morning picture: no idle blink or cleanup.
        idle = self._body(patched, 'static void PetIdle(')
        self.assertIn('!self->napping_.load()', idle)
        self.assertIn('!self->morning_shown_.load()', idle)
        # The nap face is UI only: never a durable Pet action.
        nap = self._body(patched, 'void PublishPowerScene(')
        self.assertIn('kid_app::Emotion::Sleepy', nap)
        self.assertNotIn('PostLocal', nap)

    def test_v3_xiaozhi_power_save_timer_takeover(self):
        from scripts.p1_integrate import patch_power_save_timer
        upstream = ('    void InitializePowerSaveTimer() {\n'
                    '        power_save_timer_ = new PowerSaveTimer(-1, 100, 300);\n'
                    '        power_save_timer_->OnShutdownRequest([this]() {\n'
                    '            GetDisplay()->SetChatMessage("system", "OFF");\n'
                    '            vTaskDelay(pdMS_TO_TICKS(1000));\n'
                    '            pmic_->PowerOff();\n'
                    '        });\n')
        patched = patch_power_save_timer(upstream)
        body = patched[patched.index('OnShutdownRequest'):]
        # In Pet mode the 300 s timer only restarts; outside it is unchanged.
        self.assertLess(body.index('if (pet_mode_.load())'), body.index('"OFF"'))
        self.assertLess(body.index('power_save_timer_->WakeUp();\n                return;'),
                        body.index('pmic_->PowerOff()'))
        self.assertIn('new PowerSaveTimer(-1, 100, 300)', patched)
        with self.assertRaises(RuntimeError):
            patch_power_save_timer('no anchor here')
        import inspect
        from scripts import p1_integrate
        self.assertIn('patch_power_save_timer(patch_board(', inspect.getsource(p1_integrate.run))

    def test_v3_morning_flow_wiring(self):
        patched = self._board()
        run = self._body(patched, 'void MaybeRunSelfTest()')
        alarm = run[run.index('woke_by_alarm_.load() == 1'):run.index('heap_caps_malloc')]
        # Alarm wake: good morning, unless a self-test alarm woke the board
        # (the host restores it next: never power off with tomorrow's alarm).
        self.assertLess(alarm.index('RtcWakeMark() == kSelfTestWakeMark'),
                        alarm.index('RunMorningWake("alarm")'))
        self.assertIn('PET_MORNING_SKIPPED reason=selftest_wake', alarm)
        self.assertIn('MorningFallbackPowerOff()', alarm)
        # Self-test "morning": the alarm-wake path without a power cycle,
        # refused unless an alarm is still armed.
        step = run[run.index('case kid_app::SelfTestOp::Morning:'):]
        step = step[:step.index('break;')]
        self.assertLess(step.index('!sleep_alarm_s_.load()'), step.index('RunMorningWake("selftest")'))
        self.assertIn('PET_SELFTEST_ABORT reason=morning', step)
        # Every sleep power-off records who wakes the board next.
        off = self._body(patched, 'void PowerOffForSleep()')
        self.assertLess(off.index('sleep_alarm_s_.exchange(0)'), off.index('WriteRtcWakeMark('))
        self.assertLess(off.index('WriteRtcWakeMark('), off.index('ArmRtcAlarmIn(wake_s)'))
        self.assertIn('WriteRtcWakeMark(wake_s ? kSelfTestWakeMark : 0)', off)
        self.assertIn('kid_app::Pcf85063::kRamByte', patched)
        # Opening Pet mode for the wake shows the morning picture at once.
        toggle = self._body(patched, 'void TogglePetRuntime()')
        self.assertIn('morning_request_.load() || morning_shown_.load()', toggle)
        self.assertIn('MorningScene()', toggle)
        self.assertIn('kid_app::Animation::Morning', toggle)
        scene = self._body(patched, 'kid_app::SceneModel MorningScene()')
        self.assertIn('kid_app::ScenePose::Sunrise', scene)
        self.assertIn('scene.kind = kid_app::SceneKind::Home', scene)
        begin = self._body(patched, 'void BeginMorning(')
        self.assertIn('power_.StartMorning(now)', begin)
        self.assertIn('RequestAnimation(kid_app::Animation::Morning, scene)', begin)
        self.assertIn('PET_MORNING_BEGIN', begin)
        events = self._body(patched, 'bool HandlePowerEvent(')
        self.assertIn('PET_MORNING_STAYED', events)
        wake = self._body(patched, 'bool RunMorningWake(')
        self.assertLess(wake.index('morning_request_.store(true)'), wake.index('RequestPetModeToggle()'))
        self.assertIn('PET_MORNING_FALLBACK', wake)
        # Fallback (Pet mode never opened): sleeping picture, tomorrow's
        # alarm, then PMIC off; never in the middle of an OTA upgrade.
        fallback = self._body(patched, 'void MorningFallbackPowerOff()')
        self.assertLess(fallback.index('kDeviceStateUpgrading'), fallback.index('pmic_->PowerOff()'))
        self.assertLess(fallback.index('ShowPetScene(scene)'), fallback.index('pmic_->PowerOff()'))
        self.assertLess(fallback.index('ArmRtcAlarmIn(wake_s)'), fallback.index('pmic_->PowerOff()'))
        self.assertIn('WriteRtcWakeMark(0)', fallback)
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
        # V3-03: a voice session (or a face still shown) blocks idle, nap and
        # idle power-off; leaving a nap keeps the face.
        power = patched[patched.index('void UpdatePower('):]
        power = power[:power.index('\n    }\n')]
        self.assertIn('state != kDeviceStateIdle || sleep_flow_.Active() ||\n'
                      '                          voice_face_.load() != 0', power)
        nap = patched[patched.index('void PublishPowerScene('):]
        nap = nap[:nap.index('\n    }\n')]
        self.assertIn('pet_mailbox_.Publish(drowsy ? scene : WithVoiceFace(scene))', nap)

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
        self.assertIn('# expect max_partial_ms 2500', scenario)

    def test_v3_low_battery_and_charging_wiring(self):
        patched = self._board()
        self.assertIn('#include "kid_app/battery_policy.h"', patched)
        self.assertIn('kid_app::BatteryPolicy battery_;', patched)
        # pet_boot_diag samples every 2 s (PMIC or the self-test value) into
        # one word; a board without PMIC publishes nothing.
        read = self._body(patched, 'bool ReadBattery(')
        self.assertLess(read.index('battery_inject_.load()'), read.index('pmic_->GetBatteryLevel()'))
        self.assertIn('if (!pmic_) return false;', read)
        for needle in ('pmic_->IsCharging()', 'pmic_->IsDischarging()'):
            self.assertIn(needle, read)
        self.assertNotIn('Write', read)  # PMIC: reads only
        sample = self._body(patched, 'void SampleBattery(')
        self.assertIn('kBatterySampleMs', sample)
        self.assertIn('kid_app::PackBatteryReading(reading)', sample)
        diag = self._body(patched, 'void RunDiagLoop()')
        self.assertLess(diag.index('SampleBattery(now);'), diag.index('UpdateStatusBar();'))
        # pet_input owns the policy: after the inputs and the power timers,
        # before the sleep flow ticks.
        loop = self._body(patched, 'void InputTaskLoop()')
        self.assertLess(loop.index('UpdatePower(state);'), loop.index('UpdateBattery(state);'))
        self.assertLess(loop.index('UpdateBattery(state);'), loop.index('sleep_flow_.Tick(NowMs())'))
        update = self._body(patched, 'void UpdateBattery(')
        self.assertIn('battery_.Update(reading, now)', update)
        self.assertIn('PET_BATTERY event=%s state=%s', update)
        # Reactions only on the awake Pet home screen, never while going to
        # sleep, napping, in the morning window, talking or reading.
        allowed = update[update.index('const bool allowed'):update.index('battery_.Reaction(')]
        for needle in ('!sleep_flow_.Active()', '!sleep_active_.load()', '!napping_.load()',
                       '!morning_shown_.load()', 'voice_face_.load() == 0',
                       'state == kDeviceStateIdle', 'PetInputRouter::IsHomeRail(',
                       'kBatteryReactAfterPetMs', 'battery_scripted_.load()'):
            self.assertIn(needle, allowed)
        self.assertIn('kid_app::Animation::Charge', update)
        self.assertIn('kid_app::Animation::Yawn', update)
        self.assertIn('RequestHomeAnimation(', update)
        self.assertNotIn('PostLocal', update)  # UI only, nothing durable
        self.assertNotIn('last_pet_scene_.store', update)
        # Critical: through the sleep flow (never straight to the PMIC),
        # marked so the power-off arms no wake alarm; only in Pet mode.
        events = self._body(patched, 'void HandleBatteryEvent(')
        critical = events[events.index('case kid_app::BatteryEvent::Critical:'):]
        self.assertLess(critical.index('sleep_battery_ = true;'),
                        critical.index('HandleSleepAction(sleep_flow_.Request(now))'))
        self.assertIn('if (!sleep_flow_.Active())', critical)
        self.assertIn('skipped=pet_off', critical)
        self.assertNotIn('PowerOff', events)
        # Plugged in: activity, and it cancels a flat-battery shutdown.
        plugged = events[events.index('case kid_app::BatteryEvent::Plugged:'):
                         events.index('case kid_app::BatteryEvent::Critical:')]
        self.assertIn('sleep_battery_ && sleep_flow_.Cancel()', plugged)
        self.assertIn('HandlePowerEvent(power_.OnActivity(now))', plugged)
        off = self._body(patched, 'void PowerOffForSleep()')
        self.assertLess(off.index('const bool battery = sleep_battery_;'),
                        off.index('SecondsUntilWake()'))
        self.assertIn('if (!wake_s && !battery) wake_s = SecondsUntilWake();', off)
        self.assertIn('"none_battery"', off)
        self.assertLess(off.index('pet_service_.Settled()'), off.index('pmic_->PowerOff()'))
        restore = patched[patched.index('case kid_app::SleepAction::RestoreScene:'):]
        restore = restore[:restore.index('return;')]
        self.assertIn('sleep_battery_ = false;', restore)
        # PowerManager: low battery shortens idle-to-sleep; never an idle
        # power-off while on USB power.
        power = self._body(patched, 'void UpdatePower(')
        self.assertIn('on_battery_.load() && !selftest_seen_.load() &&\n'
                      '                                   !battery_.External()', power)
        self.assertIn('conditions.low_battery = battery_.LowPower();', power)
        # Status band shows the scripted reading too.
        status = self._body(patched, 'void UpdateStatusBar()')
        self.assertLess(status.index('pmic_->GetBatteryLevel()'), status.index('battery_inject_.load()'))
        # Self-test step: a critical level needs an armed alarm (as morning).
        run = self._body(patched, 'void MaybeRunSelfTest()')
        step = run[run.index('case kid_app::SelfTestOp::Battery:'):]
        step = step[:step.index('break;')]
        self.assertLess(step.index('!sleep_alarm_s_.load()'), step.index('battery_inject_.store('))
        self.assertIn('PET_SELFTEST_ABORT reason=battery', step)
        self.assertIn('kid_app::SelfTestScript::kBatteryOff', step)
        self.assertIn('battery_scripted_.store(true)', step)
        scenario = (ROOT / 'tools' / 'selftest' / 'v3_battery.pettest').read_text(encoding='ascii')
        self.assertTrue(scenario.startswith('PETSELFTEST1\n'))
        steps = [line.strip() for line in scenario.splitlines()
                 if line.strip() and not line.lstrip().startswith('#')]
        alarm = next(i for i, s in enumerate(steps) if s.startswith('alarm '))
        self.assertLess(alarm, steps.index('battery 5 0'))
        for needle in ('battery 60 1', 'battery 15 0', 'mark begin'):
            self.assertIn(needle, steps)
        for expect in ('min:event=plugged', 'min:anim=yawn', 'min:event=critical',
                       'min:wake=selftest', 'min:battery=1', 'none:PET_SLEEP_POWEROFF_RETURNED'):
            self.assertIn('# expect ' + expect, scenario)

    def test_v4_env_tips_wiring(self):
        patched = self._board()
        self.assertIn('#include "kid_app/env_tips.h"', patched)
        self.assertIn('kid_app::EnvTipPolicy env_tips_;', patched)
        # pet_boot_diag publishes the reading (the self-test value replaces
        # the sensor) before the status band uses it; pet_input owns the policy.
        status = self._body(patched, 'void UpdateStatusBar()')
        self.assertLess(status.index('ReadShtc3TempC10(&env_temp_c10_)'),
                        status.index('env_inject_c10_.load()'))
        self.assertLess(status.index('env_inject_c10_.load()'), status.index('env_sample_.store('))
        self.assertLess(status.index('env_sample_.store('), status.index('in.temp_ok = env_ok_;'))
        loop = self._body(patched, 'void InputTaskLoop()')
        self.assertLess(loop.index('UpdateBattery(state);'), loop.index('UpdateEnvTips(state);'))
        self.assertLess(loop.index('UpdateEnvTips(state);'), loop.index('sleep_flow_.Tick(NowMs())'))
        update = self._body(patched, 'void UpdateEnvTips(')
        self.assertIn('env_tips_.Observe(ok, temp_c10, now)', update)
        # Board self-heating: offset set before each reading, none for the
        # self-test (which states the room temperature).
        self.assertLess(update.index('env_tips_.SetOffsetC10('),
                        update.index('env_tips_.Observe(ok, temp_c10, now)'))
        for needle in ('kEnvNoInject ? 0', 'battery_.External()',
                       'EnvTipPolicy::kUsbHeatC10', 'EnvTipPolicy::kBatteryHeatC10'):
            self.assertIn(needle, update)
        # Tips only on the awake Pet home screen and never in quiet hours.
        allowed = update[update.index('const bool allowed'):update.index('env_tips_.Take(')]
        for needle in ('!sleep_flow_.Active()', '!sleep_active_.load()', '!napping_.load()',
                       '!morning_shown_.load()', 'voice_face_.load() == 0',
                       'state == kDeviceStateIdle', 'PetInputRouter::IsHomeRail(',
                       'kEnvReactAfterPetMs', 'env_scripted_.load()', '!ParentMenuOpen()',
                       '!GameActive()', 'quiet_hours_.load()'):
            self.assertIn(needle, allowed)
        self.assertIn('env_tips_.Take(quiet, allowed, local_day_.load(), now)', update)
        for needle in ('kid_app::Animation::TipHot', 'kid_app::Animation::TipCold',
                       'kid_app::PhraseContext::HotTip', 'kid_app::PhraseContext::ColdTip',
                       'RequestHomeAnimation(', 'PostPhrase(', 'PET_ENV_TIP kind=%s',
                       'PET_ENV_TIP_HELD reason=quiet'):
            self.assertIn(needle, update)
        self.assertNotIn('PostLocal', update)  # UI only, nothing durable
        self.assertNotIn('last_pet_scene_.store', update)
        # The self-test step injects tenths of a degree, "off" hands back.
        run = self._body(patched, 'void MaybeRunSelfTest()')
        step = run[run.index('case kid_app::SelfTestOp::Temp:'):]
        step = step[:step.index('break;')]
        self.assertIn('kid_app::SelfTestScript::kTempOff', step)
        self.assertIn('env_scripted_.store(true)', step)
        self.assertIn('static_cast<int>(step.ms) * 10', step)
        scenario = (ROOT / 'tools' / 'selftest' / 'v4_env_tips.pettest').read_text(encoding='ascii')
        self.assertTrue(scenario.startswith('PETSELFTEST1\n'))
        steps = [line.strip() for line in scenario.splitlines()
                 if line.strip() and not line.lstrip().startswith('#')]
        for needle in ('temp 22', 'temp 31', 'temp 12', 'temp off', 'mark begin', 'volume 15'):
            self.assertIn(needle, steps)
        self.assertLess(steps.index('temp 31'), steps.index('temp 12'))
        # The baseline must hold long enough to leave any starting zone,
        # and the scenario expects to have seen "normal" before the test.
        baseline = steps.index('temp 22')
        self.assertEqual(steps[baseline + 1], 'twait 70000')
        self.assertLess(baseline, steps.index('mark begin'))
        self.assertIn('# expect min:zone=normal', scenario)
        for expect in ('min:kind=hot', 'min:kind=cold',
                       'min:name=tip_hot', 'min:name=tip_cold', 'min:ctx=hot_tip',
                       'min:ctx=cold_tip', 'none:PET_ENV_TIP_HELD', 'selftest_done'):
            self.assertIn('# expect ' + expect, scenario)

    def test_v3_parent_menu_wiring(self):
        patched = self._board()
        for header in ('parent_menu.h', 'freertos_atomic_pet_service.h'):
            self.assertIn(f'#include "kid_app/{header}"', patched)
        # The combo (after the sleep-flow guard) opens the menu; while it is
        # open it takes every input before power, sleep and pet handling.
        handle = self._body(patched, 'void HandleInputEvent(')
        self.assertIn('OpenParentMenu();', handle)
        menu_at = handle.index('if (parent_menu_.IsOpen())')
        self.assertLess(menu_at, handle.index('power_.OnActivity(NowMs())'))
        self.assertLess(menu_at, handle.index('PetInputRouter::'))
        # Entering, moving and leaving are never pet interactions; only a
        # save reaches the ledger (SaveParentSettings, not a pet action).
        for fn in ('void OpenParentMenu()', 'void CloseParentMenu(', 'void HandleParentAction(',
                   'void UpdateParentMenu()', 'void ApplyParentSettings(',
                   'void LoadParentSettings()'):
            body = self._body(patched, fn)
            self.assertNotIn('PostLocal', body, fn)
            self.assertNotIn('PostMemory', body, fn)
        for fn in ('void OpenParentMenu()', 'void CloseParentMenu(', 'void UpdateParentMenu()',
                   'void ApplyParentSettings('):
            self.assertNotIn('pet_service_', self._body(patched, fn), fn)
        opened = self._body(patched, 'void OpenParentMenu()')
        self.assertIn('state != kDeviceStateIdle', opened)
        self.assertIn('parent_ready_.load()', opened)
        self.assertLess(opened.index('pet_mailbox_.parent_only.store(true)'),
                        opened.index('PublishParentSceneLocked()'))
        self.assertIn('SceneStateLock lock(scene_state_mutex_)', opened)
        self.assertIn('PET_PARENT_OPEN', opened)
        closed = self._body(patched, 'void CloseParentMenu(')
        self.assertLess(closed.index('pet_mailbox_.parent_only.store(false)'),
                        closed.index('UnpackPetScene(last_pet_scene_.load())'))
        self.assertIn('input_hub_.SetAutoRepeat(false)', closed)
        self.assertIn('PET_PARENT_EXIT parent_exit=%s', closed)
        # Only parent frames reach the panel while it is open.
        gate = patched[patched.index('struct GatedSceneMailbox'):]
        gate = gate[:gate.index('GatedSceneMailbox pet_mailbox_')]
        self.assertIn('scene.kind != kid_app::SceneKind::Parent) return false;', gate)
        self.assertIn('if (ParentMenuOpen()) return;', self._body(patched, 'void RequestAnimation('))
        # Saved settings are committed to the ledger first, then applied; a
        # failed commit reverts to what the ledger holds.
        action = self._body(patched, 'void HandleParentAction(')
        self.assertLess(action.index('pet_service_.SaveParentSettings(next, '),
                        action.index('ApplyParentSettings(next, "menu")'))
        self.assertIn('if (saved != kid_app::CommitResult::Committed) {', action)
        self.assertIn('const auto kept = pet_service_.ParentNow();', action)
        self.assertIn('parent_menu_.Revert(ParentSettingsNow())', action)
        for marker in ('PET_PARENT_SAVED item=%s', 'PET_PARENT_SAVE_FAILED item=%s',
                       'PET_PARENT_ITEM item=%s'):
            self.assertIn(marker, action)
        update = self._body(patched, 'void UpdateParentMenu()')
        self.assertIn('parent_menu_.Tick(NowMs()), "idle"', update)
        self.assertIn('CloseParentMenu("pet_off")', update)
        self.assertIn('input_hub_.SetAutoRepeat(parent_menu_.Editing())', update)
        loop = self._body(patched, 'void InputTaskLoop()')
        self.assertLess(loop.index('HandleInputEvent(event)'), loop.index('UpdateParentMenu()'))
        self.assertLess(loop.index('UpdateParentMenu()'), loop.index('UpdatePower(state)'))
        self.assertIn('!ParentMenuOpen() && kid_app::PetInputRouter::IsHomeRail(', loop)
        self.assertIn('|| ParentMenuOpen();', self._body(patched, 'void UpdatePower('))
        # BOOT does nothing while the menu is open.
        click = patched[patched.index('boot_button_.OnClick'):]
        self.assertLess(click.index('if (ParentMenuOpen()) return;'), click.index('ToggleChatState'))
        self.assertEqual(patched.count('DialPressHeld() || ParentMenuOpen()) return;'), 2)

    def test_v3_parent_settings_drive_the_board(self):
        patched = self._board()
        # Loaded once before the self-test task (morning flow) starts.
        boot = patched[patched.index('self->RunHardwareDiag();'):]
        self.assertLess(boot.index('self->LoadParentSettings();'),
                        boot.index('self->StartSelfTestTask();'))
        load = self._body(patched, 'void LoadParentSettings()')
        self.assertIn('const bool ready = StartPetLedger();', load)
        self.assertIn('if (!ready) ApplyParentSettings(kid_app::ParentSettings{}, "defaults");',
                      load)
        start = self._body(patched, 'bool StartPetLedger(')
        self.assertLess(start.index('ApplyParentSettings(pet_service_.ParentNow(), "ledger")'),
                        start.index('parent_ready_.store(true)'))
        # Day plan: every rhythm use goes through the parent settings.
        self.assertIn('kid_app::RhythmScheduler(ParentSettingsNow().schedule)', patched)
        self.assertIn('const auto rhythm = Rhythm();', self._body(patched, 'void UpdateRhythm()'))
        self.assertIn('const auto rhythm = Rhythm();',
                      self._body(patched, 'uint32_t SecondsUntilWake()'))
        apply = self._body(patched, 'void ApplyParentSettings(')
        self.assertIn('std::lock_guard<std::mutex> lock(parent_mutex_)', apply)
        self.assertIn('motion_enabled_.store(settings.motion_enabled)', apply)
        self.assertIn('rhythm_refresh_.store(true)', apply)
        diag = self._body(patched, 'void RunDiagLoop()')
        self.assertIn('if (rhythm_refresh_.exchange(false)) UpdateRhythm();', diag)
        # Motion off: the detector is off and injected gestures do nothing.
        self.assertIn('motion_enabled_.load();  // V3-06 parent switch', diag)
        gesture = self._body(patched, 'void HandleGesture(')
        self.assertIn('!motion_off && !ParentMenuOpen()', gesture)
        self.assertIn('motion_off ? " motion_off=1" : ""', gesture)
        # Volume cap in software only: prompts and story narration.
        self.assertIn('ScalePcm(chunk, n, PromptVolumePct())', patched)
        push = self._body(patched, 'static bool StoryAudioPush(')
        self.assertIn('kid_app::ScalePcm(self->story_scaled_.data(), count, pct)', push)
        self.assertIn('if (count > self->story_scaled_.size()) return false;', push)
        self.assertNotIn('SetOutputVolume', patched)
        self.assertNotIn('nvs_set', patched)
        # Pet lines (V5-04) share that one scaled path, quiet hours included.
        self.assertEqual(patched.count('kid_app::ScalePcm(chunk, n, PromptVolumePct())'), 1)
        player = self._body(patched, 'void PromptTaskLoop()')
        self.assertIn('sound_bank_.LoadPhrase(phrase_id, &clip)', player)
        self.assertIn('kid_app::ScalePcm(chunk, n, PromptVolumePct())', player)
        self.assertIn('ParentMenuOpen()', self._body(patched, 'void PostPhrase('))
        # Battery reactions wait; any sleep (a flat battery included) closes
        # the menu first, so a critical shutdown always wins.
        battery = self._body(patched, 'void UpdateBattery(')
        allowed = battery[battery.index('const bool allowed'):battery.index('battery_.Reaction(')]
        self.assertIn('!ParentMenuOpen()', allowed)
        show = patched[patched.index('case kid_app::SleepAction::ShowSleepScene: {'):]
        self.assertLess(show.index('if (parent_menu_.IsOpen()) CloseParentMenu("sleep");'),
                        show.index('sleep_active_.store(true);'))
        # Self-test "combo" step: BOOT + dial press on the raw-edge queue.
        run = self._body(patched, 'void MaybeRunSelfTest()')
        combo = run[run.index('case kid_app::SelfTestOp::Combo:'):]
        combo = combo[:combo.index('break;')]
        self.assertLess(combo.index('PostKeyEdge(kid_app::InputKey::Boot, true)'),
                        combo.index('vTaskDelay(pdMS_TO_TICKS(step.ms))'))
        self.assertIn('PostKeyEdge(kid_app::InputKey::DialPress, true)', combo)
        self.assertIn('PostKeyEdge(kid_app::InputKey::DialPress, false)', combo)
        self.assertIn('PostKeyEdge(kid_app::InputKey::Boot, false)', combo)
        scenario = (ROOT / 'tools' / 'selftest' / 'v3_parent_menu.pettest').read_text(
            encoding='ascii')
        self.assertTrue(scenario.startswith('PETSELFTEST1\n'))
        self.assertIn('combo 5500', scenario)
        self.assertIn('# expect commits 0', scenario)
        self.assertIn('# expect min:parent_exit=idle 1', scenario)
        self.assertIn('# expect min:motion_off=1 1', scenario)

    def test_selftest_time_scale_only_inside_a_scripted_trial(self):
        patched = self._board()
        self.assertIn('#include "kid_app/selftest_clock.h"', patched)
        run = self._body(patched, 'void MaybeRunSelfTest()')
        # The only setter calls in the whole board are in MaybeRunSelfTest,
        # after the ota_1 check and a valid script (else: no scaling ever).
        self.assertEqual(patched.count('SetForSelfTest('), run.count('SetForSelfTest('))
        self.assertEqual(run.count('SetForSelfTest('), 2)
        first = run.index('SetForSelfTest(')
        for guard in ('ESP_PARTITION_SUBTYPE_APP_OTA_1', 'PET_SELFTEST_NONE reason=alarm_wake',
                      'ParseSector(', 'PET_SELFTEST_NONE reason=no_script'):
            self.assertLess(run.index(guard), first, guard)
        # Real time again whenever the script ends (done or any abort).
        self.assertIn('struct RealTimeAtEnd', run)
        self.assertLess(run.index('real_time_at_end;'), run.index('PET_SELFTEST_START'))
        self.assertIn('SetForSelfTest(kid_app::SelfTestClock::kRealPct)', run)
        step = run[run.index('case kid_app::SelfTestOp::TimeScale:'):]
        self.assertIn('kid_app::SelfTestClock::SetForSelfTest(step.ms)', step[:step.index('break;')])
        # Nothing else in the tree sets it: the header defines it, the
        # host test exercises it, the board script calls it.
        allowed = {'firmware/components/kid_app/include/kid_app/selftest_clock.h',
                   'tests/cpp/test_selftest_clock.cc', 'scripts/p1_integrate.py'}
        for path in list((ROOT / 'firmware').rglob('*')) + list((ROOT / 'scripts').rglob('*')) + \
                list((ROOT / 'integrations').rglob('*')) + list((ROOT / 'tools').rglob('*')):
            if path.suffix not in ('.h', '.cc', '.cpp', '.c', '.py', '.sh'):
                continue
            rel = path.relative_to(ROOT).as_posix()
            if rel in allowed:
                continue
            self.assertNotIn('SetForSelfTest', path.read_text(encoding='utf-8', errors='replace'),
                             rel)

    def test_selftest_time_scale_covers_feature_timers_not_physics(self):
        src = ROOT / 'firmware' / 'components' / 'kid_app' / 'src'
        scaled = {
            'pet_animation.cc': ('Scaled(kBlinkMinMs)', 'Scaled(kCleanupIdleMs)'),
            'power_manager.cc': ('Scaled(kMorningAwakeMs)', 'Scaled(kIdleAfterMs)',
                                 'Scaled(drowsy ? kIdleAfterMs : kNapAfterMs)',
                                 'Scaled(off_after)'),
            'parent_menu.cc': ('Scaled(kIdleExitMs)',),
            'mini_game.cc': ('Scaled(kIdleExitMs)', 'Scaled(kRepeatPromptMs)'),
            'battery_policy.cc': ('Scaled(kCriticalRetryMs)', 'Scaled(kEatEveryMs)',
                                  'Scaled(kYawnEveryMs)'),
            'env_tips.cc': ('Scaled(kRepeatMs)', 'Scaled(kHoldMs)', 'Scaled(kPendingMs)'),
        }
        for name, needles in scaled.items():
            text = (src / name).read_text(encoding='utf-8')
            for needle in needles:
                self.assertIn('SelfTestClock::' + needle, text, name)
        # Refresh physics, audio, the sleep cancel window, voice-face and
        # prompt settle times and the wall-clock status band stay real.
        for name in ('sleep_flow.cc', 'status_bar.cc', 'prompt_scheduler.cc', 'voice_face.cc',
                     'refresh_planner.cc', 'pet_refresh_gate.cc', 'pet_phrases.cc',
                     'freertos_pet_display_worker.cc', 'audio_coordinator.cc',
                     'book_reader.cc'):
            self.assertNotIn('SelfTestClock', (src / name).read_text(encoding='utf-8'), name)
        patched = self._board()
        self.assertIn('kid_app::SelfTestClock::Scaled(kBatterySampleMs)', patched)
        self.assertIn('kid_app::SelfTestClock::Scaled(kBatteryReactAfterPetMs)', patched)
        self.assertIn('kid_app::SelfTestClock::Scaled(kEnvReactAfterPetMs)', patched)
        # EPD and audio waits in the board never go through the scale.
        for body in ('void PowerOffForSleep()', 'void MorningFallbackPowerOff()'):
            self.assertNotIn('SelfTestClock', self._body(patched, body), body)

    def test_selftest_waitidle_waits_for_xiaozhi_idle_with_a_timeout(self):
        patched = self._board()
        run = self._body(patched, 'void MaybeRunSelfTest()')
        step = run[run.index('case kid_app::SelfTestOp::WaitIdle:'):]
        step = step[:step.index('break;')]
        self.assertIn('GetDeviceState() != kDeviceStateIdle', step)
        self.assertIn('NowMs() - start < step.ms', step)
        self.assertIn('kid_app::SelfTestScript::kIdleSettleMs', step)
        self.assertIn('PET_SELFTEST_IDLE ready=', step)
        self.assertNotIn('ABORT', step)  # not ready: the pet toggle decides

    def test_board_runs_the_atomic_ledger(self):
        patched = self._board()
        # One Pet owner: the atomic ledger; the legacy service and the V3-06
        # board-only settings store are gone from the board.
        self.assertIn('kid_app::FreertosAtomicPetService pet_service_;', patched)
        self.assertNotIn('FreertosPetService pet_service_', patched)
        self.assertNotIn('freertos_pet_service.h', patched)
        self.assertNotIn('NvsParentSettingsStore', patched)
        self.assertNotIn('parent_store_', patched)
        start = self._body(patched, 'bool StartPetLedger(')
        self.assertIn('std::lock_guard<std::mutex> lock(pet_ledger_mutex_)', start)
        self.assertLess(start.index('pet_service_.SetDayProvider(&LocalDay, this)'),
                        start.index('pet_service_.Start('))
        self.assertIn('StartMode::MigrateLegacyLocal', start)
        self.assertIn('&PetCommitted, nullptr, this', start)  # no outbound: local only
        self.assertIn('PET_LEDGER_READY ok=%d migration=%s failure=%s', start)
        # Pet mode and the boot both go through the same guarded start.
        init = self._body(patched, 'bool InitializePetRuntime()')
        self.assertIn('if (!StartPetLedger(&restored)) return false;', init)
        boot = patched[patched.index('self->RunHardwareDiag();'):]
        self.assertLess(boot.index('self->LoadParentSettings();'),
                        boot.index('self->StartSelfTestTask();'))
        # Commits keep their scene publishing and log marker.
        committed = self._body(patched, 'static void PetCommitted(')
        self.assertIn('outcome != kid_app::CommitResult::Committed) return;', committed)
        self.assertIn('self->pet_mailbox_.Publish(self->WithVoiceFace(scene))', committed)
        self.assertIn('PET_EVENT_COMMITTED id_class=%s fed=%u', committed)
        # Power-off waits for the ledger and logs the generation to restore.
        off = self._body(patched, 'void PowerOffForSleep()')
        self.assertLess(off.index('pet_service_.Settled()'), off.index('pmic_->PowerOff()'))
        self.assertIn('ledger_gen=%llu', off)
        self.assertLess(off.index('pet_service_.GenerationNow()'), off.index('pmic_->PowerOff()'))
        # V5: day for memory counters from the RTC, temperament for pet lines,
        # memory events only through the ledger.
        rhythm = self._body(patched, 'void UpdateRhythm()')
        self.assertIn('local_day_.store(ok ? static_cast<uint32_t>(kid_app::EpochFromCivil(rtc)'
                      ' / 86400) : 0);', rhythm)
        player = self._body(patched, 'void PromptTaskLoop()')
        self.assertLess(player.index('phrase_planner_.SetTemperament(pet_service_.TemperamentNow())'),
                        player.index('phrase_planner_.Next(context, NowMs())'))
        self.assertIn('pet_service_.PostMemory(event)',
                      self._body(patched, 'bool RecordMemory('))
        # Every child action still goes through PostLocal on that one service.
        self.assertGreaterEqual(patched.count('pet_service_.PostLocal('), 5)
        scenario = (ROOT / 'tools' / 'selftest' / 'v7_ledger_persist.pettest').read_text(
            encoding='ascii')
        self.assertTrue(scenario.startswith('PETSELFTEST1\n'))
        self.assertIn('# expect ledger_restored 1', scenario)
        self.assertLess(scenario.index('\nalarm '), scenario.index('\ntap pwr'))
        self.assertIn('# expect none:PET_LEDGER_PERSIST_FAILED 0', scenario)
        self.assertIn('# expect min:released=1 1', scenario)

    def test_reader_never_traps_the_child_when_ledger_writes_fail(self):
        patched = self._board()
        self.assertIn('#include "kid_app/ledger_persist_policy.h"', patched)
        # A dropped StopStory (persist failed) still shows the home rail.
        committed = self._body(patched, 'static void PetCommitted(')
        self.assertLess(committed.index('kid_app::StoryEscapeNeeded(event.action, outcome)'),
                        committed.index('outcome != kid_app::CommitResult::Committed) return;'))
        self.assertIn('self->EscapeStoryHome("persist")', committed)
        escape = self._body(patched, 'void EscapeStoryHome(')
        self.assertIn('SceneStateLock lock(scene_state_mutex_)', escape)
        self.assertIn('PetScenePresenter::Present(kid_app::PetSnapshot{})', escape)
        self.assertLess(escape.index('last_pet_scene_.store('),
                        escape.index('pet_event_mode_.store(kid_app::PetMode::Idle)'))
        self.assertIn('PET_STORY_ESCAPE reason=%s', escape)
        self.assertNotIn('PostLocal', escape)  # UI only, nothing durable
        # "Home" that cannot even be queued escapes too.
        step = self._body(patched, 'void ApplyBookStep(')
        home = step[step.index('PET_BOOK_HOME'):]
        self.assertLess(home.index('PostLocal(kid_app::PetAction::StopStory)'),
                        home.index('EscapeStoryHome("rejected")'))
        # Restored Story without a readable book goes home at start.
        init = self._body(patched, 'bool InitializePetRuntime()')
        restore = init[init.index('book_outline_ready_.store(true);'):]
        self.assertIn('kid_app::RestoredStoryNeedsHome(pet_event_mode_.load(),', restore)
        self.assertIn('story_sd_.Ready() && book_outline_.pages > 0', restore)
        self.assertLess(restore.index('PET_STORY_RESTORE_HOME reason=no_book'),
                        restore.index('PostLocal(kid_app::PetAction::StopStory)'))
        self.assertIn('EscapeStoryHome("no_book")', restore)
        # The ledger worker logs the real NVS error and partition usage.
        service = (ROOT / 'firmware/components/kid_app/src/freertos_atomic_pet_service.cc'
                   ).read_text(encoding='utf-8')
        self.assertIn('PET_LEDGER_PERSIST_FAILED err=%s kind=%u attempt=%u dropped=%d', service)
        self.assertIn('NvsErrorName(store_.LastError())', service)
        self.assertIn('PET_NVS_STATS when=%s used=%u free=%u total=%u', service)
        self.assertIn('LogNvsUsage("boot")', service)
        self.assertIn('LogNvsUsage("persist_failed")', service)
        self.assertNotIn('persist blocked', service)
        self.assertIn('Run(store_,old_pet,old_parent,bridge,marker,&release)', service)

    def test_v4_mini_game_wiring(self):
        patched = self._board()
        self.assertIn('#include "kid_app/mini_game.h"', patched)
        self.assertIn('#include "kid_app/game_pack_reader.h"', patched)
        # The Game rail item starts a game (UI only) after the lock is
        # released; quiet hours or a low battery fall back to "say hi".
        rail = self._body(patched, 'kid_app::PetInputDecision HandleHomeRailLocked(')
        self.assertLess(rail.index('HomeItem::Game && GameStartAllowed()'),
                        rail.index('PetInputRouter::DecideHome('))
        allowed = self._body(patched, 'bool GameStartAllowed()')
        self.assertIn('quiet_hours_.load()', allowed)
        self.assertIn('battery_.LowPower()', allowed)
        self.assertIn('!= kDeviceStateIdle ? "voice"', allowed)
        handle = self._body(patched, 'void HandleInputEvent(')
        # Sleep and the parent combo still come first; then the game takes the dial.
        self.assertLess(handle.index('InputKind::SleepRequest && pet_mode_.load()'),
                        handle.index('HandleGameInput(event.kind)'))
        self.assertLess(handle.index('OpenParentMenu();'), handle.index('HandleGameInput(event.kind)'))
        self.assertLess(handle.index('if (game_requested_)'), handle.index('pet_service_.PostLocal('))
        start = self._body(patched, 'void StartGame()')
        self.assertIn('kid_app::BuiltinGameRounds()', start)  # works without a TF card
        self.assertIn('game_seed_.exchange(0)', start)
        self.assertIn('pet_mailbox_.game_only.store(true)', start)
        self.assertNotIn('pet_service_', start)  # no Pet action, no NVS
        # Only Game frames reach the panel while it runs.
        mailbox = self._body(patched, 'struct GatedSceneMailbox')
        self.assertIn('game_only.load() && scene.kind != kid_app::SceneKind::Game', mailbox)
        end = self._body(patched, 'void EndGame(const char* reason, bool publish_home = true)')
        self.assertIn('pet_mailbox_.game_only.store(false)', end)
        self.assertIn('last_pet_scene_.load()', end)
        self.assertIn('GameCompletedMemoryEvent(', end)
        self.assertIn('posted = RecordMemory(memory);', end)  # the #64 ledger path only
        self.assertNotIn('pet_service_', end)
        # Every other owner of the screen or the speaker ends the game first.
        show = patched[patched.index('case kid_app::SleepAction::ShowSleepScene: {'):]
        self.assertLess(show.index('EndGame("sleep", false)'), show.index('sleep_active_.store(true);'))
        self.assertLess(self._body(patched, 'void OpenParentMenu()').index('EndGame("parent", false)'),
                        self._body(patched, 'void OpenParentMenu()').index('parent_menu_.Open('))
        self.assertIn('EndGame("morning", false)', self._body(patched, 'void BeginMorning('))
        update = self._body(patched, 'void UpdateGame(DeviceState state)')
        self.assertIn('EndGame("pet_off")', update)
        self.assertIn('EndGame("voice")', update)
        self.assertIn('game_.Tick(NowMs())', update)
        loop = self._body(patched, 'void InputTaskLoop()')
        self.assertLess(loop.index('UpdateGame(state)'), loop.index('UpdatePower(state)'))
        # No pet lines, gestures, battery reactions, animations or naps over a game.
        self.assertIn('GameActive()', self._body(patched, 'void PostPhrase('))
        self.assertIn('!GameActive()', self._body(patched, 'void HandleGesture('))
        self.assertIn('if (GameActive()) return;', self._body(patched, 'void RequestAnimation('))
        self.assertIn('if (GameActive()) conditions.busy = true;', self._body(patched, 'void UpdatePower('))
        battery = self._body(patched, 'void UpdateBattery(')
        self.assertIn('!GameActive()', battery[battery.index('const bool allowed'):])
        # Game sounds share the one prompt player and its volume rules.
        player = self._body(patched, 'void PromptTaskLoop()')
        self.assertIn('kid_app::LoadGameClip(GamePackNow(), request.game, &game_clip)', player)
        self.assertIn('game_clip.Copy(offset, chunk, 480)', player)
        self.assertIn('0x47414D45u', player)
        self.assertEqual(patched.count('kid_app::ScalePcm(chunk, n, PromptVolumePct())'), 1)
        self.assertIn('LoadGamePack();', self._body(patched, 'void LoadSoundPack()'))
        self.assertIn('game_file_.Open("/sdcard/games", "default", "kpg")',
                      self._body(patched, 'void LoadGamePack()'))
        # Self-test "game" step fixes the next seed.
        run = self._body(patched, 'void MaybeRunSelfTest()')
        self.assertIn('case kid_app::SelfTestOp::Game:', run)
        self.assertIn('game_seed_.store(step.ms)', run)
        scenario = (ROOT / 'tools' / 'selftest' / 'v4_mini_game.pettest').read_text(
            encoding='ascii')
        self.assertTrue(scenario.startswith('PETSELFTEST1\n'))
        self.assertIn('\ngame 4242\n', scenario)
        self.assertIn('# expect commits 0', scenario)
        self.assertIn('# expect min:end=completed 1', scenario)

if __name__ == "__main__":
    unittest.main()
