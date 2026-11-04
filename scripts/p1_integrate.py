#!/usr/bin/env python3
"""Overlay the first pet ePaper demonstration onto a pinned XiaoZhi firmware tree.

Run this only in a disposable checkout that passed P0 SHA verification.
No flashing, erasing or network I/O. Narrow edits only to the Waveshare board,
its CustomEpdDisplay class and main's component dependency.
"""
from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MANIFEST = json.loads((ROOT / "version-lock.json").read_text(encoding="utf-8"))
BOARD_REL = "main/boards/waveshare/esp32-s3-epaper-3.97"


def once(text: str, before: str, after: str, label: str) -> str:
    count = text.count(before)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one anchor; found {count}")
    return text.replace(before, after, 1)


def patch_main_cmake(text: str) -> str:
    return once(
        text, "                    PRIV_REQUIRES\n                        esp_pm",
        "                    PRIV_REQUIRES\n                        kid_app\n                        esp_pm",
        "main/CMakeLists.txt",
    )


def patch_display_h(text: str) -> str:
    text = once(text, '#include "lcd_display.h"',
        '#include "lcd_display.h"\n#include "kid_app/scene_model.h"\n#include "kid_app/story_pack_reader.h"',
        "display header include")
    text = once(text, "    void EPD_Sleep();", """    void EPD_Sleep();

    // P1 manual-only pet display prototype; no voice/LLM dependency.
    bool ShowPetPreview(kid_app::Emotion emotion);
    bool ShowPetScene(const kid_app::SceneModel& scene);
    // Partial waveform on the whole buffer (no flash); base must be shown.
    bool ShowPetSceneFast(const kid_app::SceneModel& scene);
    bool ShowStoryPage(kid_app::StoryPackReader& reader, uint16_t page);\n    void ExitPetPreview();
""", "display header public")
    text = once(text, "    uint8_t* buffer = NULL;", "    uint8_t* buffer = NULL;\n    bool pet_preview_enabled_ = false; // LVGL lock protected", "display header state")
    return text


DISPLAY_IMPL = r'''
bool CustomEpdDisplay::ShowPetPreview(kid_app::Emotion emotion) {
    return ShowPetScene({kid_app::SceneKind::Home, emotion, 1});
}

bool CustomEpdDisplay::ShowPetScene(const kid_app::SceneModel& scene) {
    // Do not use the FreeRTOS task stack for a 48KiB mono framebuffer.
    void* memory = heap_caps_malloc(sizeof(kid_app::MonoFrame),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!memory) {
        ESP_LOGE(TAG, "Pet framebuffer PSRAM allocation failed");
        return false;
    }

    auto* frame = new (memory) kid_app::MonoFrame();
    kid_app::SceneComposer::Render(scene, *frame);

    bool okay = false;
    if (lvgl_port_lock(5000)) {
        // Only one producer touches the physical display at a time.
        if (buffer && epd_spi_data.buffer_len >= static_cast<int>(kid_app::MonoFrame::kSize)) {
            memcpy(buffer, frame->Data().data(), kid_app::MonoFrame::kSize);
            pet_preview_enabled_ = true;
            EPD_SendCommand(0x3C);  // border waveform as in EPD_Init (full)
            EPD_SendData(0x01);
            EPD_DisplayPartBaseImage();
            okay = true;
        }
        lvgl_port_unlock();
    }
    frame->~MonoFrame();
    heap_caps_free(memory);
    return okay;
}

bool CustomEpdDisplay::ShowPetSceneFast(const kid_app::SceneModel& scene) {
    void* memory = heap_caps_malloc(sizeof(kid_app::MonoFrame),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!memory) {
        ESP_LOGE(TAG, "Pet framebuffer PSRAM allocation failed");
        return false;
    }
    auto* frame = new (memory) kid_app::MonoFrame();
    kid_app::SceneComposer::Render(scene, *frame);
    bool okay = false;
    if (lvgl_port_lock(5000)) {
        if (buffer && epd_spi_data.buffer_len >= static_cast<int>(kid_app::MonoFrame::kSize)) {
            memcpy(buffer, frame->Data().data(), kid_app::MonoFrame::kSize);
            pet_preview_enabled_ = true;
            // New image into RAM 0x24 and the partial waveform, then the same
            // image into RAM 0x26 so the next partial diffs against what the
            // panel now shows. A full ShowPetScene() must precede the first.
            EPD_SendCommand(0x3C);  // border waveform: hold during partial
            EPD_SendData(0x80);
            EPD_SendCommand(0x24);
            writeBytes(buffer, epd_spi_data.buffer_len);
            EPD_TurnOnDisplayPart();
            EPD_SendCommand(0x26);
            writeBytes(buffer, epd_spi_data.buffer_len);
            okay = true;
        }
        lvgl_port_unlock();
    }
    frame->~MonoFrame();
    heap_caps_free(memory);
    return okay;
}

bool CustomEpdDisplay::ShowStoryPage(kid_app::StoryPackReader& reader, uint16_t page) {
    // Acquire a single 48 KiB PSRAM frame; do SD reads BEFORE the EPD lock.
    void* memory = heap_caps_malloc(sizeof(kid_app::MonoFrame),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!memory) return false;
    auto* frame = new (memory) kid_app::MonoFrame();
    const bool loaded = reader.ReadFrame(page, *frame);
    bool okay = false;
    if (loaded && lvgl_port_lock(5000)) {
        if (buffer && epd_spi_data.buffer_len >= static_cast<int>(kid_app::MonoFrame::kSize)) {
            memcpy(buffer, frame->Data().data(), kid_app::MonoFrame::kSize);
            pet_preview_enabled_ = true;
            EPD_SendCommand(0x3C);  // border waveform as in EPD_Init (full)
            EPD_SendData(0x01);
            EPD_DisplayPartBaseImage();
            okay = true;
        }
        lvgl_port_unlock();
    }
    frame->~MonoFrame();
    heap_caps_free(memory);
    return okay;
}

void CustomEpdDisplay::ExitPetPreview() {
    if (!lvgl_port_lock(5000)) return;
    pet_preview_enabled_ = false;
    // Tell the existing XiaoZhi LVGL page to redraw after the pet overlay.
    lv_obj_invalidate(lv_screen_active());
    lvgl_port_unlock();
}
'''


def patch_display_cc(text: str) -> str:
    text = once(text, "#include <vector>", "#include <vector>\n#include <new>\n#include <cstring>\n#include <esp_heap_caps.h>", "display impl headers")
    text = once(text,
        "    driver->EPD_Clear();",
        """    if (driver->pet_preview_enabled_) {
        // LVGL continues updating its logical objects, but does not overwrite
        // the manual pet frame on the physical display until ExitPetPreview().
        lv_disp_flush_ready(disp);
        return;
    }
    driver->EPD_Clear();""",
        "display LVGL flush",
    )
    return text.rstrip() + "\n\n" + DISPLAY_IMPL.strip() + "\n"


BOARD_WORKER = r'''
    // Opt-in child pet runtime for experimental firmware only. Stock OTA0
    // remains pristine; this code is overlaid only in a disposable checkout.
    kid_app::FreertosPetSceneMailbox pet_mailbox_;
    kid_app::FreertosPetService pet_service_;
    // 500 ms pacing: full refreshes are only triggered by scene changes or
    // the planner's ghosting limit; highlights use partial refreshes.
    kid_app::FreertosPetDisplayWorker pet_display_worker_{pet_mailbox_, 500};
    SemaphoreHandle_t pet_screen_mutex_ = nullptr;
    std::atomic<bool> pet_mode_{false};
    std::atomic<bool> pet_toggle_busy_{false};
    std::atomic<bool> pet_runtime_ready_{false};
    std::atomic<kid_app::PetMode> pet_event_mode_{kid_app::PetMode::Idle};
    std::atomic<uint32_t> last_pet_scene_{0};
    kid_app::HomeMenu home_menu_;              // pet_input task only
    std::atomic<uint8_t> home_selection_{kid_app::HomeMenu::kDefaultIndex};
    kid_app::RefreshPlanner refresh_planner_;  // kid_epd task only
    std::atomic<bool> planner_reset_{false};
    // Short animations, idle blink and deferred ghosting cleanup. Idle()/
    // SceneShown()/FrameShown() run on kid_epd; Request()/Reset() come from
    // other tasks, so every call takes anim_mutex_ (never held while drawing).
    kid_app::PetAnimationDirector anim_director_;
    SemaphoreHandle_t anim_mutex_ = nullptr;

    void RequestAnimation(kid_app::Animation animation, const kid_app::SceneModel& target) {
        SceneStateLock lock(anim_mutex_);
        anim_director_.Request(animation, target, NowMs());
    }

    // Host-tested layout lives in kid_app::PackScene / UnpackScene.
    static uint32_t PackPetScene(const kid_app::SceneModel& scene) {
        return kid_app::PackScene(scene);
    }
    static kid_app::SceneModel UnpackPetScene(uint32_t bits) {
        return kid_app::UnpackScene(bits);
    }

    // Guards pet_event_mode_ + last_pet_scene_ + home_selection_ + the
    // mailbox publish as one step, shared by the pet and input tasks, so a
    // stale expression or a highlight different from the selected item
    // can never be the last frame published.
    SemaphoreHandle_t scene_state_mutex_ = nullptr;
    struct SceneStateLock {
        explicit SceneStateLock(SemaphoreHandle_t m) : mutex(m) {
            if (mutex) xSemaphoreTake(mutex, portMAX_DELAY);
        }
        ~SceneStateLock() {
            if (mutex) xSemaphoreGive(mutex);
        }
        SemaphoreHandle_t mutex;
    };

    static bool StoryAudioBegin(uint64_t token, void* context) {
        auto* self = static_cast<WaveshareEsp32s3ePaper3inch97*>(context);
        if (!self || !self->pet_mode_.load() || self->sleep_active_.load() ||
            Application::GetInstance().GetDeviceState() != kDeviceStateIdle)
            return false;
        return Application::GetInstance().GetAudioService().BeginLocalStoryPcm(token);
    }
    static bool StoryAudioPush(uint64_t token, const int16_t* pcm,
                               size_t count, void* context) {
        auto* self = static_cast<WaveshareEsp32s3ePaper3inch97*>(context);
        if (!self || !self->pet_mode_.load() || self->sleep_active_.load() ||
            Application::GetInstance().GetDeviceState() != kDeviceStateIdle)
            return false;
        return Application::GetInstance().GetAudioService().TryQueueLocalStoryPcm(
            token, pcm, count);
    }
    static void StoryAudioCancel(void*) {
        Application::GetInstance().GetAudioService().CancelLocalStoryPcm();
    }

    // Draws one frame. Caller holds pet_screen_mutex_ and checked pet_mode_.
    // source: gate (published scene), anim (director frame), cleanup.
    bool DrawPetFrameLocked(const kid_app::SceneModel& scene, const char* source) {
        bool okay = false;
        const uint32_t epd_started_ms = NowMs();
        if (scene.kind == kid_app::SceneKind::Reader && story_sd_.Ready()) {
            okay = display_->ShowStoryPage(*story_sd_.Reader(), scene.book_page);
            // Only speak after EPD shows a validated book page;
            // invalid/out-of-range pages must cancel prior narration.
            if (story_pcm_.Ready()) {
                if (okay && !sleep_active_.load()) story_pcm_.RequestPage(scene.book_page);
                else story_pcm_.Stop();
            }
        }
        // Missing SD or a page outside a reviewed book falls back to
        // the pre-existing monochrome reader placeholder.
        if (planner_reset_.exchange(false)) refresh_planner_.ForceFull();
        kid_app::RefreshMode mode = kid_app::RefreshMode::Full;
        if (!okay) {
            mode = refresh_planner_.Plan(scene);
            okay = mode == kid_app::RefreshMode::Partial
                       ? display_->ShowPetSceneFast(scene)
                       : display_->ShowPetScene(scene);
        }
        refresh_planner_.Completed(scene, mode, okay);
        // The sleep cancel window starts once the final picture (all three
        // Z letters, ScenePose::Still) is on the panel.
        if (okay && scene.kind == kid_app::SceneKind::Sleep &&
            scene.pose == kid_app::ScenePose::Still) {
            const uint32_t shown_ms = NowMs();
            sleep_shown_ms_.store(shown_ms ? shown_ms : 1);
            ESP_LOGI(TAG, "PET_SLEEP_SHOWN");
        }
        // V1-08 / V2-06: refresh duration per mode; ghosting by eye.
        ESP_LOGI(TAG, "PET_DIAG_EPD kind=%d page=%u sel=%u mode=%s ok=%d ms=%u n=%u "
                 "pose=%u src=%s rail=%u pet=%u",
                 static_cast<int>(scene.kind), scene.book_page, scene.home_selection,
                 mode == kid_app::RefreshMode::Partial ? "partial" : "full", okay ? 1 : 0,
                 static_cast<unsigned>(NowMs() - epd_started_ms),
                 static_cast<unsigned>(++epd_refreshes_), static_cast<unsigned>(scene.pose),
                 source, refresh_planner_.RailPartials(), refresh_planner_.PetPartials());
        return okay;
    }

    static bool PetRender(const kid_app::SceneModel& scene, void* context) {
        auto* self = static_cast<WaveshareEsp32s3ePaper3inch97*>(context);
        if (!self || !self->pet_screen_mutex_ ||
            xSemaphoreTake(self->pet_screen_mutex_, pdMS_TO_TICKS(30000)) != pdTRUE) {
            return false;
        }
        // This mutex also serializes ExitPetPreview, so no late pet frame
        // can overwrite the original XiaoZhi LVGL screen after exit.
        bool okay = false;
        if (self->pet_mode_.load()) okay = self->DrawPetFrameLocked(scene, "gate");
        xSemaphoreGive(self->pet_screen_mutex_);
        if (okay) {
            SceneStateLock lock(self->anim_mutex_);
            self->anim_director_.SceneShown(scene, NowMs());
        }
        return okay;
    }

    // kid_epd, ~every 100 ms while no published scene is waiting.
    static void PetIdle(const kid_app::SceneModel& displayed, void* context) {
        auto* self = static_cast<WaveshareEsp32s3ePaper3inch97*>(context);
        if (!self || !self->pet_mode_.load()) return;
        // Blink and cleanup only on a quiet, awake Pet home screen.
        const bool ambient = !self->sleep_active_.load() &&
                             displayed.kind == kid_app::SceneKind::Home &&
                             Application::GetInstance().GetDeviceState() == kDeviceStateIdle;
        kid_app::DirectorStep step{};
        {
            SceneStateLock lock(self->anim_mutex_);
            step = self->anim_director_.Idle(displayed, NowMs(), ambient,
                                             self->refresh_planner_.NeedsCleanup());
        }
        if (step.kind == kid_app::DirectorStep::Kind::None) return;
        if (xSemaphoreTake(self->pet_screen_mutex_, pdMS_TO_TICKS(1000)) != pdTRUE) return;
        const bool cleanup = step.kind == kid_app::DirectorStep::Kind::Cleanup;
        bool okay = false;
        if (self->pet_mode_.load()) {
            if (cleanup) {
                ESP_LOGI(TAG, "PET_EPD_CLEANUP rail=%u pet=%u", self->refresh_planner_.RailPartials(),
                         self->refresh_planner_.PetPartials());
                self->refresh_planner_.ForceFull();
            }
            okay = self->DrawPetFrameLocked(step.scene, cleanup ? "cleanup" : "anim");
        }
        xSemaphoreGive(self->pet_screen_mutex_);
        SceneStateLock lock(self->anim_mutex_);
        if (cleanup) {
            self->anim_director_.SceneShown(step.scene, NowMs());
            return;
        }
        self->anim_director_.FrameShown(NowMs());
        ESP_LOGI(TAG, "PET_ANIM_FRAME anim=%s pose=%u ok=%d last=%d",
                 kid_app::AnimationName(step.animation), static_cast<unsigned>(step.scene.pose),
                 okay ? 1 : 0, step.last ? 1 : 0);
        if (step.last) ESP_LOGI(TAG, "PET_ANIM_DONE name=%s", kid_app::AnimationName(step.animation));
    }

    static void PetCommitted(const kid_app::PetEvent& event,
                              kid_app::PumpResult outcome,
                              const kid_app::PetEffects& effects,
                              const kid_app::PetSnapshot& snapshot, void* context) {
        auto* self = static_cast<WaveshareEsp32s3ePaper3inch97*>(context);
        if (!self || outcome != kid_app::PumpResult::Committed) return;
        // Suspend the old page immediately when a new page/scene is
        // committed. The new page starts only after the slow EPD refresh.
        if (self->story_pcm_.Ready() &&
            (snapshot.mode != kid_app::PetMode::Story || effects.repaint)) {
            // Invalidate queued audio now, not after the slower SD feeder
            // wakes up. A previously spoken page must not overlap EPD.
            StoryAudioCancel(self);
            self->story_pcm_.Stop();
        }
        {
            SceneStateLock lock(self->scene_state_mutex_);
            auto scene = kid_app::PetScenePresenter::Present(snapshot);
            scene.home_selection = self->home_selection_.load();
            self->last_pet_scene_.store(PackPetScene(scene));
            if (effects.repaint && !self->sleep_active_.load()) self->pet_mailbox_.Publish(scene);
            // Mode after scene: the input task never sees a home mode
            // paired with a stale reader scene.
            self->pet_event_mode_.store(snapshot.mode);
            // Plays once this scene is on the panel (hearts after the happy
            // face), also when the expression did not change.
            if (!self->sleep_active_.load()) {
                if (event.action == kid_app::PetAction::Feed)
                    self->RequestAnimation(kid_app::Animation::Feed, scene);
                else if (event.action == kid_app::PetAction::Pickup)
                    self->RequestAnimation(kid_app::Animation::Greet, scene);
            }
        }
        // Deliberately do NOT play audio here. AudioCoordinator/AudioPort
        // will be wired separately; only committed events can emit effects.
        ESP_LOGI(TAG, "PET_EVENT_COMMITTED id_class=%s",
                 (event.id & kid_app::kLocalEventBit) ? "local" : "external");
    }

    bool InitializePetRuntime() {
        if (pet_runtime_ready_.load()) return true;
        if (!pet_screen_mutex_) pet_screen_mutex_ = xSemaphoreCreateMutex();
        if (!pet_screen_mutex_) return false;
        if (!scene_state_mutex_) scene_state_mutex_ = xSemaphoreCreateMutex();
        if (!scene_state_mutex_) return false;
        if (!anim_mutex_) anim_mutex_ = xSemaphoreCreateMutex();
        if (!anim_mutex_) return false;
        if (!pet_mailbox_ready_) {
            if (!pet_mailbox_.Initialize()) return false;
            pet_mailbox_ready_ = true;
        }
        if (!pet_display_task_ready_) {
            if (!pet_display_worker_.Start(&PetRender, this, &PetIdle)) return false;
            pet_display_task_ready_ = true;
        }
        if (!pet_event_task_ready_) {
            kid_app::PetSnapshot restored{};
            if (!pet_service_.Start(&PetCommitted, this, &restored)) return false;
            auto scene = kid_app::PetScenePresenter::Present(restored);
            scene.home_selection = home_selection_.load();
            last_pet_scene_.store(PackPetScene(scene));
            pet_event_mode_.store(restored.mode);
            pet_event_task_ready_ = true;
        }
        if (!story_sd_attempted_) {
            story_sd_attempted_ = true;
            // Default approved demo book only. Never format an SD card.
            // Missing/malformed media cannot prevent pet mode startup.
            if (!story_sd_.Initialize("forest_friends"))
                ESP_LOGW(TAG, "PET_STORY_SD_FALLBACK");
        }
        if (story_sd_.Ready() && !story_pcm_.Ready() &&
            !story_pcm_.Start("/sdcard/books", "forest_friends",
                              {&StoryAudioBegin, &StoryAudioPush,
                               &StoryAudioCancel}, this))
            ESP_LOGW(TAG, "PET_STORY_AUDIO_UNAVAILABLE");
        pet_runtime_ready_.store(true);
        return true;
    }

    // Story SD is optional; accessed by the toggle and EPD tasks only after
    // initialization. No media IO on BOOT interrupt or Pet/NVS task.
    kid_app::WaveshareStorySd story_sd_;
    kid_app::FreertosStoryPcmWorker story_pcm_;
    bool story_sd_attempted_ = false;

    // Only the toggle worker accesses these booleans, preventing double-start.
    bool pet_mailbox_ready_ = false;
    bool pet_display_task_ready_ = false;
    bool pet_event_task_ready_ = false;

    void TogglePetRuntime() {
        if (pet_mode_.load()) {
            if (story_pcm_.Ready()) story_pcm_.Stop();
            Application::GetInstance().GetAudioService().CancelLocalStoryPcm();
            pet_display_worker_.SetEnabled(false);
            if (pet_screen_mutex_ &&
                xSemaphoreTake(pet_screen_mutex_, pdMS_TO_TICKS(30000)) == pdTRUE) {
                pet_mode_.store(false);
                display_->ExitPetPreview();
                planner_reset_.store(true);
                {
                    SceneStateLock lock(anim_mutex_);
                    anim_director_.Reset(NowMs());
                }
                xSemaphoreGive(pet_screen_mutex_);
                ESP_LOGI(TAG, "PET_RUNTIME_OFF");
            } else {
                // Never claim an exit completed without serializing screen I/O.
                pet_display_worker_.SetEnabled(true);
                ESP_LOGW(TAG, "PET_RUNTIME_EXIT_RETRY");
            }
            return;
        }
        // Voice always keeps its original single-click behavior unless idle.
        if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) return;
        if (!InitializePetRuntime()) {
            ESP_LOGE(TAG, "PET_RUNTIME_INIT_FAILED (NVS kept intact)");
            return;
        }
        pet_mode_.store(true);
        pet_display_worker_.SetEnabled(true);
        {
            SceneStateLock lock(scene_state_mutex_);
            const auto scene = UnpackPetScene(last_pet_scene_.load());
            pet_mailbox_.Publish(scene);
            // TuanTuan waves hello when Pet mode opens on the home screen.
            {
                SceneStateLock anim_lock(anim_mutex_);
                anim_director_.Reset(NowMs());
            }
            RequestAnimation(kid_app::Animation::Greet, scene);
        }
        ESP_LOGI(TAG, "PET_RUNTIME_ON");
    }

    void RequestPetModeToggle() {
        if (pet_toggle_busy_.exchange(true)) return;
        const BaseType_t started = xTaskCreate(
            [](void* context) {
                auto* self = static_cast<WaveshareEsp32s3ePaper3inch97*>(context);
                self->TogglePetRuntime();
                self->pet_toggle_busy_.store(false);
                vTaskDelete(nullptr);
            }, "pet_toggle", 8192, this, 2, nullptr);
        if (started != pdPASS) {
            pet_toggle_busy_.store(false);
            ESP_LOGE(TAG, "PET_RUNTIME_TOGGLE_TASK_FAILED");
        }
    }

    // Three-way dial (active low, pulled up). The schematic labels GPIO4
    // Button_Up and GPIO6 Button_Down, but the 2026-11-02 V1 trial showed
    // the direction a child pushes is the opposite: physical up = GPIO6,
    // physical down = GPIO4. Centre press = GPIO5 (Button_Function).
    // Button callbacks only enqueue raw edges; one task owns InputHub.
    Button dial_up_{GPIO_NUM_6};
    Button dial_press_{GPIO_NUM_5};
    Button dial_down_{GPIO_NUM_4};
    QueueHandle_t pet_input_queue_ = nullptr;
    kid_app::InputHub input_hub_;

    struct RawKeyEdge {
        uint8_t key;
        uint8_t down;
        uint32_t at_ms;
    };

    static uint32_t NowMs() {
        return static_cast<uint32_t>(esp_timer_get_time() / 1000);
    }

    // BOOT + dial press is the parent combo. Legacy iot_button BOOT
    // handlers check this so the combo never toggles chat or Pet mode.
    static bool DialPressHeld() { return gpio_get_level(GPIO_NUM_5) == 0; }

    void PostKeyEdge(kid_app::InputKey key, bool down) {
        if (key == kid_app::InputKey::Power && down) pwr_edges_.fetch_add(1);
        if (!pet_input_queue_) return;
        const RawKeyEdge edge{static_cast<uint8_t>(key),
                              static_cast<uint8_t>(down ? 1 : 0), NowMs()};
        xQueueSend(pet_input_queue_, &edge, 0);
    }

    static const char* InputKindName(kid_app::InputKind kind) {
        switch (kind) {
            case kid_app::InputKind::DialUp: return "DialUp";
            case kid_app::InputKind::DialDown: return "DialDown";
            case kid_app::InputKind::DialPress: return "DialPress";
            case kid_app::InputKind::DialLong: return "DialLong";
            case kid_app::InputKind::Talk: return "Talk";
            case kid_app::InputKind::BootLong: return "BootLong";
            case kid_app::InputKind::SleepRequest: return "SleepRequest";
            case kid_app::InputKind::ParentCombo: return "ParentCombo";
        }
        return "Unknown";
    }

    void HandleInputEvent(const kid_app::InputEvent& event) {
        ESP_LOGI(TAG, "PET_INPUT kind=%s repeat=%d pet=%d", InputKindName(event.kind),
                 event.repeat ? 1 : 0, pet_mode_.load() ? 1 : 0);
        // A child using the dial is activity; do not auto-power-off.
        if (power_save_timer_) power_save_timer_->WakeUp();
        if (event.kind == kid_app::InputKind::SleepRequest) sleep_requests_.fetch_add(1);
        // Going to sleep: inputs can only cancel it (V2-09).
        if (pet_mode_.load() && sleep_flow_.Active()) {
            HandleSleepAction(sleep_flow_.OnInput(event.kind, NowMs()));
            return;
        }
        // V1-05 only: until the parent menu exists, the parent combo in Pet
        // mode runs the RTC-alarm power-on test.
        if (event.kind == kid_app::InputKind::ParentCombo && pet_mode_.load()) {
            ArmRtcWakeTest();
            return;
        }
        if (event.kind == kid_app::InputKind::SleepRequest && pet_mode_.load()) {
            HandleSleepAction(sleep_flow_.Request(NowMs()));
            return;
        }
        // BOOT/PWR keep their existing board handlers; only dial semantics
        // become durable Pet actions, and only while Pet mode is on.
        if (!pet_mode_.load()) return;
        kid_app::PetInputDecision decision{};
        bool begin_sleep = false;
        {
            SceneStateLock lock(scene_state_mutex_);
            const auto pet_mode = pet_event_mode_.load();
            decision = kid_app::PetInputRouter::IsHomeRail(pet_mode)
                           ? HandleHomeRailLocked(event, pet_mode, &begin_sleep)
                           : kid_app::PetInputRouter::DecideDial(pet_mode, event.kind);
        }
        if (begin_sleep) {
            HandleSleepAction(sleep_flow_.Request(NowMs()));
            return;
        }
        // The durable action is queued outside the lock; the pet task
        // takes the same lock when it commits.
        if (decision.valid && !pet_service_.PostLocal(decision.action))
            ESP_LOGW(TAG, "PET_INPUT_REJECTED");
    }

    // Home action rail (V2-03): moving the highlight is UI-only (no NVS);
    // a dial press turns the selected item into a durable Pet action.
    // Caller holds scene_state_mutex_.
    kid_app::PetInputDecision HandleHomeRailLocked(const kid_app::InputEvent& event,
                                                   kid_app::PetMode pet_mode,
                                                   bool* begin_sleep) {
        if (event.kind == kid_app::InputKind::DialUp ||
            event.kind == kid_app::InputKind::DialDown) {
            if (!home_menu_.Move(event.kind == kid_app::InputKind::DialUp ? -1 : +1)) return {};
            const uint8_t index = home_menu_.Index();
            home_selection_.store(index);
            auto scene = UnpackPetScene(last_pet_scene_.load());
            scene.home_selection = index;
            last_pet_scene_.store(PackPetScene(scene));
            pet_mailbox_.Publish(scene);
            ESP_LOGI(TAG, "PET_HOME_SELECT index=%u", index);
            return {};
        }
        if (event.kind != kid_app::InputKind::DialPress) return {};
        if (home_menu_.Selected() == kid_app::HomeItem::Sleep) {
            *begin_sleep = true;  // started after the lock is released
            return {};
        }
        return kid_app::PetInputRouter::DecideHome(pet_mode, home_menu_.Selected());
    }

    // ---- V2-09 "put the pet to sleep" ----
    // pet_input task owns the flow; the EPD worker only reports the moment
    // the sleep scene is on the panel (that starts the 3 s cancel window).
    kid_app::SleepFlow sleep_flow_;
    std::atomic<uint32_t> sleep_shown_ms_{0};
    std::atomic<bool> sleep_active_{false};
    std::atomic<uint16_t> sleep_alarm_s_{0};  // self-test only: wake after N s
    uint16_t sleep_seq_ = 0;  // makes each sleep frame distinct for the gate

    void HandleSleepAction(kid_app::SleepAction action) {
        switch (action) {
            case kid_app::SleepAction::None:
                return;
            case kid_app::SleepAction::ShowSleepScene: {
                sleep_active_.store(true);
                sleep_shown_ms_.store(0);
                if (story_pcm_.Ready()) story_pcm_.Stop();
                Application::GetInstance().GetAudioService().CancelLocalStoryPcm();
                kid_app::SceneModel scene{};
                scene.kind = kid_app::SceneKind::Sleep;
                scene.emotion = kid_app::Emotion::Sleepy;
                // SceneComposer ignores the page for Sleep; a fresh value stops
                // the gate from skipping a request right after a cancel.
                scene.book_page = ++sleep_seq_;
                // Falls asleep on screen: one Z, two, then all three.
                scene.pose = kid_app::ScenePose::Zzz1;
                SceneStateLock lock(scene_state_mutex_);
                pet_mailbox_.Publish(scene);
                RequestAnimation(kid_app::Animation::SleepIn, scene);
                ESP_LOGI(TAG, "PET_SLEEP_REQUESTED");
                return;
            }
            case kid_app::SleepAction::RestoreScene: {
                sleep_active_.store(false);
                SceneStateLock lock(scene_state_mutex_);
                pet_mailbox_.Publish(UnpackPetScene(last_pet_scene_.load()));
                ESP_LOGI(TAG, "PET_SLEEP_CANCELLED");
                return;
            }
            case kid_app::SleepAction::PowerOff:
                PowerOffForSleep();
                return;
        }
    }

    void PowerOffForSleep() {
        if (story_pcm_.Ready()) story_pcm_.Stop();
        Application::GetInstance().GetAudioService().CancelLocalStoryPcm();
        // Let any queued Pet event commit to NVS first (bounded wait).
        for (int i = 0; i < 20 && !pet_service_.Settled(); ++i) vTaskDelay(pdMS_TO_TICKS(100));
        const uint16_t wake_s = sleep_alarm_s_.exchange(0);
        const bool alarm = wake_s && ArmRtcAlarmIn(wake_s);
        ESP_LOGI(TAG, "PET_SLEEP_POWEROFF settled=%d alarm_s=%u armed=%d",
                 pet_service_.Settled() ? 1 : 0, static_cast<unsigned>(wake_s), alarm ? 1 : 0);
        vTaskDelay(pdMS_TO_TICKS(300));
        if (pmic_) pmic_->PowerOff();
        vTaskDelay(pdMS_TO_TICKS(3000));
        // Still running: the PMIC did not cut power (e.g. USB VBUS present).
        ESP_LOGW(TAG, "PET_SLEEP_POWEROFF_RETURNED");
        if (alarm) ClearRtcAlarm(false);
        // Presses queued while blocked must not replay (e.g. a new sleep).
        xQueueReset(pet_input_queue_);
        input_hub_ = kid_app::InputHub{};
        sleep_flow_.PowerOffReturned();
        HandleSleepAction(kid_app::SleepAction::RestoreScene);
    }

    void InputTaskLoop() {
        RawKeyEdge edge{};
        for (;;) {
            const auto state = Application::GetInstance().GetDeviceState();
            input_hub_.SetVoiceActive(state == kDeviceStateListening ||
                                      state == kDeviceStateSpeaking);
            while (xQueueReceive(pet_input_queue_, &edge, pdMS_TO_TICKS(20)) == pdTRUE) {
                const auto key = static_cast<kid_app::InputKey>(edge.key);
                if (edge.down) input_hub_.Press(key, edge.at_ms);
                else input_hub_.Release(key, edge.at_ms);
            }
            input_hub_.Tick(NowMs());
            kid_app::InputEvent event{};
            while (input_hub_.Next(&event)) HandleInputEvent(event);
            if (!pet_mode_.load() && sleep_flow_.Cancel()) {
                sleep_active_.store(false);
                ESP_LOGI(TAG, "PET_SLEEP_CANCELLED reason=pet_off");
            }
            if (sleep_flow_.Active()) {
                if (sleep_flow_.Phase() == kid_app::SleepPhase::Showing) {
                    const uint32_t shown_ms = sleep_shown_ms_.exchange(0);
                    if (shown_ms) sleep_flow_.OnSceneShown(shown_ms);
                }
                HandleSleepAction(sleep_flow_.Tick(NowMs()));
            }
        }
    }

    void StartPetInput() {
        pet_input_queue_ = xQueueCreate(16, sizeof(RawKeyEdge));
        if (!pet_input_queue_ ||
            xTaskCreate([](void* context) {
                static_cast<WaveshareEsp32s3ePaper3inch97*>(context)->InputTaskLoop();
            }, "pet_input", 4096, this, 2, nullptr) != pdPASS) {
            ESP_LOGW(TAG, "PET_INPUT_TASK_FAILED");
            return;
        }
        dial_up_.OnPressDown([this]() { PostKeyEdge(kid_app::InputKey::DialUp, true); });
        dial_up_.OnPressUp([this]() { PostKeyEdge(kid_app::InputKey::DialUp, false); });
        dial_press_.OnPressDown([this]() { PostKeyEdge(kid_app::InputKey::DialPress, true); });
        dial_press_.OnPressUp([this]() { PostKeyEdge(kid_app::InputKey::DialPress, false); });
        dial_down_.OnPressDown([this]() { PostKeyEdge(kid_app::InputKey::DialDown, true); });
        dial_down_.OnPressUp([this]() { PostKeyEdge(kid_app::InputKey::DialDown, false); });
        boot_button_.OnPressDown([this]() { PostKeyEdge(kid_app::InputKey::Boot, true); });
        boot_button_.OnPressUp([this]() { PostKeyEdge(kid_app::InputKey::Boot, false); });
        pwr_button_.OnPressDown([this]() { PostKeyEdge(kid_app::InputKey::Power, true); });
        pwr_button_.OnPressUp([this]() { PostKeyEdge(kid_app::InputKey::Power, false); });
    }

    // V1 read-only hardware acceptance. Probes and reads only: never sets
    // RTC time/alarm, never writes PMIC registers, never stores readings.
    bool DiagI2c(uint8_t address, const uint8_t* tx, size_t tx_len,
                 uint8_t* rx, size_t rx_len, uint32_t wait_ms = 0) {
        i2c_device_config_t config = {};
        config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        config.device_address = address;
        config.scl_speed_hz = 100000;
        i2c_master_dev_handle_t device = nullptr;
        if (i2c_master_bus_add_device(i2c_bus_, &config, &device) != ESP_OK) return false;
        esp_err_t err;
        if (rx_len && !wait_ms) {
            err = i2c_master_transmit_receive(device, tx, tx_len, rx, rx_len, 100);
        } else {
            err = i2c_master_transmit(device, tx, tx_len, 100);
            if (err == ESP_OK && rx_len) {
                vTaskDelay(pdMS_TO_TICKS(wait_ms));
                err = i2c_master_receive(device, rx, rx_len, 100);
            }
        }
        i2c_master_bus_rm_device(device);
        return err == ESP_OK;
    }

    void RunHardwareDiag() {
        ESP_LOGI(TAG, "PET_DIAG_DIAL idle_levels gpio4=%d gpio5=%d gpio6=%d",
                 gpio_get_level(GPIO_NUM_4), gpio_get_level(GPIO_NUM_5),
                 gpio_get_level(GPIO_NUM_6));
        // ES8311, TG28/AXP2101, PCF85063, QMI8658 (SA0 low/high), SHTC3.
        static const uint8_t kAddresses[] = {0x18, 0x34, 0x51, 0x6A, 0x6B, 0x70};
        uint8_t imu_address = 0;
        for (uint8_t address : kAddresses) {
            const bool ok = i2c_master_probe(i2c_bus_, address, 50) == ESP_OK;
            ESP_LOGI(TAG, "PET_DIAG_I2C addr=0x%02x ok=%d", address, ok ? 1 : 0);
            if (ok && (address == 0x6A || address == 0x6B)) imu_address = address;
        }
        uint8_t reg = 0x00, who = 0;  // QMI8658 WHO_AM_I, expected 0x05
        const bool imu_ok = imu_address && DiagI2c(imu_address, &reg, 1, &who, 1);
        ESP_LOGI(TAG, "PET_DIAG_IMU addr=0x%02x ok=%d who_am_i=0x%02x",
                 imu_address, imu_ok ? 1 : 0, who);
        if (imu_ok && who == 0x05) imu_address_ = imu_address;
        const uint8_t wake[] = {0x35, 0x17}, measure[] = {0x78, 0x66}, sleep[] = {0xB0, 0x98};
        uint8_t env[6] = {};
        // Waits assume a 100 Hz tick: >= 10 ms after wake (240 us needed),
        // >= 30 ms for a normal-mode measurement (12.1 ms max).
        bool env_ok = DiagI2c(0x70, wake, sizeof(wake), nullptr, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        env_ok = env_ok && DiagI2c(0x70, measure, sizeof(measure), env, sizeof(env), 40);
        DiagI2c(0x70, sleep, sizeof(sleep), nullptr, 0);
        const int raw_t = (env[0] << 8) | env[1], raw_h = (env[3] << 8) | env[4];
        ESP_LOGI(TAG, "PET_DIAG_ENV ok=%d temp_c10=%d hum_pct=%d", env_ok ? 1 : 0,
                 env_ok ? -450 + (1750 * raw_t) / 65536 : 0,
                 env_ok ? (100 * raw_h) / 65536 : 0);
        uint8_t time_reg = 0x04, clock[7] = {};  // seconds..years, BCD
        const bool rtc_ok = DiagI2c(0x51, &time_reg, 1, clock, sizeof(clock));
        ESP_LOGI(TAG, "PET_DIAG_RTC ok=%d osc_stopped=%d time=20%02x-%02x-%02x %02x:%02x:%02x",
                 rtc_ok ? 1 : 0, (clock[0] >> 7) & 1, clock[6], clock[5] & 0x1F,
                 clock[3] & 0x3F, clock[2] & 0x3F, clock[1] & 0x7F, clock[0] & 0x7F);
        if (pmic_) {
            ESP_LOGI(TAG, "PET_DIAG_PMIC battery=%d charging=%d discharging=%d",
                     pmic_->GetBatteryLevel(), pmic_->IsCharging() ? 1 : 0,
                     pmic_->IsDischarging() ? 1 : 0);
        }
    }

    // ---- V1 round 3: measurement loop (diagnostic firmware only) ----
    // Writes are limited to: QMI8658 sensor configuration (volatile), the
    // PCF85063 time from a plausible server clock, and the alarm used by the
    // explicit parent-combo power-on test. Nothing is stored or uploaded.
    uint8_t imu_address_ = 0;
    std::atomic<uint32_t> pwr_edges_{0};
    std::atomic<uint32_t> sleep_requests_{0};
    std::atomic<uint32_t> epd_refreshes_{0};
    bool rtc_synced_ = false;
    std::atomic<int> woke_by_alarm_{-1};  // set once at board init

    bool RtcRead(kid_app::CivilTime* out, bool* stopped) {
        uint8_t reg = kid_app::Pcf85063::kTimeRegister, regs[7] = {};
        return DiagI2c(0x51, &reg, 1, regs, sizeof(regs)) &&
               kid_app::Pcf85063::DecodeTime(regs, out, stopped);
    }

    static constexpr uint32_t kAlarmWakeMagic = 0xA1A2B0C5;
    static constexpr uint32_t kAlarmWakeNoteSeconds = 60;

    // True if this boot is an RTC-alarm wake, or a chip reset shortly after
    // one (the alarm flag was already cleared by that first boot).
    bool NoteAlarmWake(bool alarm_now) {
        kid_app::CivilTime rtc{};
        bool stopped = true;
        const bool rtc_ok = RtcRead(&rtc, &stopped) && !stopped;
        const uint32_t now = rtc_ok ? static_cast<uint32_t>(kid_app::EpochFromCivil(rtc)) : 0;
        if (alarm_now) {
            g_pet_alarm_wake_magic = kAlarmWakeMagic;
            g_pet_alarm_wake_epoch = now;
            return true;
        }
        if (esp_reset_reason() == ESP_RST_POWERON) {  // no-init RAM is garbage
            g_pet_alarm_wake_magic = 0;
            return false;
        }
        return g_pet_alarm_wake_magic == kAlarmWakeMagic && rtc_ok &&
               now >= g_pet_alarm_wake_epoch &&
               now - g_pet_alarm_wake_epoch <= kAlarmWakeNoteSeconds;
    }

    // record_wake: only the board-init call reports why this boot happened.
    void ClearRtcAlarm(bool record_wake = true) {
        uint8_t reg = kid_app::Pcf85063::kControl2, control2 = 0;
        if (!DiagI2c(0x51, &reg, 1, &control2, 1)) return;
        const int alarm_flag = (control2 & kid_app::Pcf85063::kAlarmFlag) ? 1 : 0;
        int carried = 0;
        if (record_wake) {
            const bool woke = NoteAlarmWake(alarm_flag != 0);
            carried = woke && !alarm_flag;
            woke_by_alarm_.store(woke ? 1 : 0);
        }
        ESP_LOGI(TAG, "PET_DIAG_WAKE alarm_flag=%d alarm_irq_enabled=%d carried=%d", alarm_flag,
                 (control2 & kid_app::Pcf85063::kAlarmInterruptEnable) ? 1 : 0, carried);
        if (!(control2 & (kid_app::Pcf85063::kAlarmFlag |
                          kid_app::Pcf85063::kAlarmInterruptEnable)))
            return;
        // One-shot test: disable every alarm field and the interrupt again.
        const uint8_t disable[] = {kid_app::Pcf85063::kAlarmRegister,
                                   0x80, 0x80, 0x80, 0x80, 0x80};
        DiagI2c(0x51, disable, sizeof(disable), nullptr, 0);
        const uint8_t clear[] = {kid_app::Pcf85063::kControl2,
                                 static_cast<uint8_t>(control2 & 0x3F)};
        DiagI2c(0x51, clear, sizeof(clear), nullptr, 0);
    }

    void SyncRtcFromSystemTime() {
        // XiaoZhi sets system time from the server, already shifted to
        // local time, so the epoch value maps directly to the wall clock.
        const time_t now = time(nullptr);
        const auto system = kid_app::CivilFromEpoch(static_cast<int64_t>(now));
        if (!kid_app::PlausibleWallClock(system)) return;
        kid_app::CivilTime rtc{};
        bool stopped = true;
        const bool readable = RtcRead(&rtc, &stopped);
        const int64_t drift = readable ? kid_app::EpochFromCivil(rtc) - now : 0;
        if (readable && !stopped && drift >= -2 && drift <= 2) {
            rtc_synced_ = true;
            return;
        }
        uint8_t write[8] = {kid_app::Pcf85063::kTimeRegister};
        kid_app::Pcf85063::EncodeTime(system, write + 1);
        rtc_synced_ = DiagI2c(0x51, write, sizeof(write), nullptr, 0);
        const int drift_s = drift > 2000000000 ? 2000000000
                          : drift < -2000000000 ? -2000000000
                                                : static_cast<int>(drift);
        ESP_LOGI(TAG, "PET_DIAG_RTC_SYNC ok=%d was_stopped=%d drift_s=%d "
                 "now=%04u-%02u-%02u %02u:%02u:%02u", rtc_synced_ ? 1 : 0,
                 stopped ? 1 : 0, drift_s, system.year,
                 system.month, system.day, system.hour, system.minute, system.second);
    }

    // PCF85063 alarm N seconds from now (RTC must already hold real time).
    bool ArmRtcAlarmIn(uint32_t seconds) {
        kid_app::CivilTime rtc{};
        bool stopped = true;
        if (!RtcRead(&rtc, &stopped) || stopped || !kid_app::PlausibleWallClock(rtc)) {
            ESP_LOGW(TAG, "PET_RTC_ALARM refused=rtc_not_set");
            return false;
        }
        const auto at = kid_app::CivilFromEpoch(kid_app::EpochFromCivil(rtc) + seconds);
        uint8_t alarm[6] = {kid_app::Pcf85063::kAlarmRegister};
        kid_app::Pcf85063::EncodeAlarm(at, alarm + 1);
        uint8_t reg = kid_app::Pcf85063::kControl2, control2 = 0;
        const bool armed = DiagI2c(0x51, alarm, sizeof(alarm), nullptr, 0) &&
                           DiagI2c(0x51, &reg, 1, &control2, 1);
        const uint8_t enable[] = {kid_app::Pcf85063::kControl2,
            static_cast<uint8_t>((control2 | kid_app::Pcf85063::kAlarmInterruptEnable) &
                                 ~kid_app::Pcf85063::kAlarmFlag)};
        if (!armed || !DiagI2c(0x51, enable, sizeof(enable), nullptr, 0)) {
            ESP_LOGW(TAG, "PET_RTC_ALARM refused=i2c_error");
            return false;
        }
        ESP_LOGI(TAG, "PET_RTC_ALARM armed_for=%02u:%02u:%02u", at.hour, at.minute, at.second);
        return true;
    }

    void ArmRtcWakeTest() {
        if (!ArmRtcAlarmIn(90)) {
            ESP_LOGW(TAG, "PET_DIAG_ALARM_TEST refused");
            return;
        }
        ESP_LOGI(TAG, "PET_DIAG_ALARM_TEST powering_off_in_ms=1500");
        vTaskDelay(pdMS_TO_TICKS(1500));
        if (pmic_) pmic_->PowerOff();
    }

    bool ConfigureImu() {
        if (!imu_address_) return false;
        // CTRL1 little-endian + auto-increment; CTRL2 +/-4 g, 62.5 Hz;
        // CTRL7 accelerometer only (gyro stays off).
        const uint8_t ctrl1[] = {0x02, 0x40}, ctrl2[] = {0x03, 0x17}, ctrl7[] = {0x08, 0x01};
        return DiagI2c(imu_address_, ctrl1, 2, nullptr, 0) &&
               DiagI2c(imu_address_, ctrl2, 2, nullptr, 0) &&
               DiagI2c(imu_address_, ctrl7, 2, nullptr, 0);
    }

    bool ReadAccel(int16_t* x, int16_t* y, int16_t* z) {
        uint8_t reg = 0x35, data[6] = {};  // AX_L .. AZ_H
        if (!DiagI2c(imu_address_, &reg, 1, data, sizeof(data))) return false;
        *x = static_cast<int16_t>(data[0] | (data[1] << 8));
        *y = static_cast<int16_t>(data[2] | (data[3] << 8));
        *z = static_cast<int16_t>(data[4] | (data[5] << 8));
        return true;
    }

    // ---- Host-scripted self-test (unattended hardware trials) ----
    // The host writes a PETSELFTEST1 script into the last 4 KiB of ota_1
    // (beyond the app image) before a trial and erases it afterwards. The
    // firmware only reads it; without a valid script nothing happens.
    static constexpr size_t kSelfTestSectorBytes = 4096;

    void MaybeRunSelfTest() {
        const esp_partition_t* running = esp_ota_get_running_partition();
        if (!running || running->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_1 ||
            running->size < 2 * kSelfTestSectorBytes) {
            ESP_LOGI(TAG, "PET_SELFTEST_NONE reason=not_ota1");
            return;
        }
        // A trial that powered off with an alarm must not loop on wake-up.
        if (woke_by_alarm_.load() == 1) {
            ESP_LOGI(TAG, "PET_SELFTEST_NONE reason=alarm_wake");
            return;
        }
        auto* sector = static_cast<uint8_t*>(heap_caps_malloc(kSelfTestSectorBytes,
                                                              MALLOC_CAP_8BIT));
        auto* script = new (std::nothrow) kid_app::SelfTestScript();
        const bool ok = sector && script &&
            esp_partition_read(running, running->size - kSelfTestSectorBytes, sector,
                               kSelfTestSectorBytes) == ESP_OK &&
            script->ParseSector(sector, kSelfTestSectorBytes);
        heap_caps_free(sector);
        if (!ok) {
            ESP_LOGI(TAG, "PET_SELFTEST_NONE reason=no_script");
            delete script;
            return;
        }
        ESP_LOGI(TAG, "PET_SELFTEST_START steps=%u total_ms=%u",
                 static_cast<unsigned>(script->Count()),
                 static_cast<unsigned>(script->TotalMs()));
        for (size_t i = 0; i < script->Count(); ++i) {
            const auto& step = script->Steps()[i];
            ESP_LOGI(TAG, "PET_SELFTEST_STEP i=%u op=%u key=%u ms=%u", static_cast<unsigned>(i),
                     static_cast<unsigned>(step.op), static_cast<unsigned>(step.key),
                     static_cast<unsigned>(step.ms));
            switch (step.op) {
                case kid_app::SelfTestOp::Wait:
                    vTaskDelay(pdMS_TO_TICKS(step.ms));
                    break;
                case kid_app::SelfTestOp::TogglePet: {
                    // A toggle is silently dropped while XiaoZhi is not idle
                    // or a previous toggle runs: retry, and abort the whole
                    // script rather than testing the wrong mode.
                    const bool before = pet_mode_.load();
                    bool changed = false;
                    for (int attempt = 0; attempt < 15 && !changed; ++attempt) {
                        RequestPetModeToggle();
                        for (int poll = 0; poll < 20 && !changed; ++poll) {
                            vTaskDelay(pdMS_TO_TICKS(100));
                            changed = pet_mode_.load() != before;
                        }
                    }
                    if (!changed) {
                        ESP_LOGW(TAG, "PET_SELFTEST_ABORT reason=pet_toggle step=%u",
                                 static_cast<unsigned>(i));
                        delete script;
                        return;
                    }
                    ESP_LOGI(TAG, "PET_SELFTEST_PET on=%d", pet_mode_.load() ? 1 : 0);
                    break;
                }
                case kid_app::SelfTestOp::Alarm:
                    sleep_alarm_s_.store(static_cast<uint16_t>(step.ms));
                    ESP_LOGI(TAG, "PET_SELFTEST_ALARM s=%u", static_cast<unsigned>(step.ms));
                    break;
                case kid_app::SelfTestOp::Mark:
                    ESP_LOGI(TAG, "PET_SELFTEST_MARK name=%s", step.name);
                    break;
                case kid_app::SelfTestOp::Hold:
                    // Same raw-edge queue as the physical buttons.
                    PostKeyEdge(step.key, true);
                    vTaskDelay(pdMS_TO_TICKS(step.ms));
                    PostKeyEdge(step.key, false);
                    vTaskDelay(pdMS_TO_TICKS(step.gap_ms));
                    break;
            }
        }
        ESP_LOGI(TAG, "PET_SELFTEST_DONE steps=%u", static_cast<unsigned>(script->Count()));
        delete script;
    }

    void StartSelfTestTask() {
        if (xTaskCreate([](void* context) {
                static_cast<WaveshareEsp32s3ePaper3inch97*>(context)->MaybeRunSelfTest();
                vTaskDelete(nullptr);
            }, "pet_selftest", 12288, this, 1, nullptr) != pdPASS)
            ESP_LOGW(TAG, "PET_SELFTEST_TASK_FAILED");
    }

    void RunDiagLoop() {
        const bool imu_ok = ConfigureImu();
        ESP_LOGI(TAG, "PET_DIAG_ACC configured=%d", imu_ok ? 1 : 0);
        vTaskDelay(pdMS_TO_TICKS(50));  // first accel sample
        kid_app::MotionStats motion;
        uint32_t last_summary_ms = NowMs(), last_heartbeat_ms = 0;
        for (;;) {
            int16_t x = 0, y = 0, z = 0;
            if (imu_ok && ReadAccel(&x, &y, &z)) motion.Add(x, y, z);
            const uint32_t now = NowMs();
            if (imu_ok && now - last_summary_ms >= 500) {
                last_summary_ms = now;
                const auto s = motion.TakeSummary();
                ESP_LOGI(TAG, "PET_DIAG_ACC n=%u mean_mg=%d,%d,%d mag_mg=%u..%u dev_mg=%u",
                         s.samples, s.mean_x_mg, s.mean_y_mg, s.mean_z_mg, s.min_mag_mg,
                         s.max_mag_mg, s.max_dev_mg);
            }
            if (now - last_heartbeat_ms >= 10000) {
                last_heartbeat_ms = now;
                SyncRtcFromSystemTime();
                ESP_LOGI(TAG, "PET_DIAG_HEARTBEAT up_s=%u pet=%d pwr_edges=%u sleep_req=%u "
                         "batt=%d chg=%d woke_by_alarm=%d rtc_synced=%d epd_n=%u",
                         static_cast<unsigned>(now / 1000), pet_mode_.load() ? 1 : 0,
                         static_cast<unsigned>(pwr_edges_.load()),
                         static_cast<unsigned>(sleep_requests_.load()),
                         pmic_ ? pmic_->GetBatteryLevel() : -1,
                         pmic_ && pmic_->IsCharging() ? 1 : 0, woke_by_alarm_.load(),
                         rtc_synced_ ? 1 : 0, static_cast<unsigned>(epd_refreshes_.load()));
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
'''


def patch_board(text: str) -> str:
    text = once(text, "#include <stdio.h>",
        '#include <stdio.h>\n#include <atomic>\n'
        '#include "kid_app/freertos_pet_service.h"\n'
        '#include "kid_app/freertos_pet_scene_mailbox.h"\n'
        '#include "kid_app/freertos_pet_display_worker.h"\n'
        '#include "kid_app/pet_scene_presenter.h"\n'
        '#include "kid_app/pet_local_event_id.h"\n'
        '#include "kid_app/pet_input_router.h"\n'
        '#include "kid_app/input_hub.h"\n'
        '#include "kid_app/home_menu.h"\n'
        '#include "kid_app/refresh_planner.h"\n'
        '#include "kid_app/pet_animation.h"\n'
        '#include "kid_app/selftest_script.h"\n'
        '#include "kid_app/sleep_flow.h"\n'
        '#include "esp_attr.h"\n'
        '#include "esp_system.h"\n'
        '// Alarm-wake note: kept across chip resets (e.g. the USB-JTAG reset\n'
        '// when a host reopens the port right after an alarm power-on), lost\n'
        '// on power loss. See NoteAlarmWake().\n'
        'RTC_NOINIT_ATTR static uint32_t g_pet_alarm_wake_magic;\n'
        'RTC_NOINIT_ATTR static uint32_t g_pet_alarm_wake_epoch;\n'
        '#include "esp_ota_ops.h"\n'
        '#include "esp_partition.h"\n'
        '#include <esp_heap_caps.h>\n'
        '#include <new>\n'
        '#include "kid_app/rtc_clock.h"\n'
        '#include "kid_app/motion_stats.h"\n'
        '#include <ctime>\n'
        '#include <esp_timer.h>\n'
        '#include <freertos/queue.h>\n'
        '#include "kid_app/waveshare_story_sd.h"\n'
        '#include "kid_app/freertos_story_pcm_worker.h"',
        "board headers")
    text = once(text, "    void InitializeButtons() {",
                BOARD_WORKER.rstrip() + "\n\n    void InitializeButtons() {",
                "board pet worker")
    text = once(text, "            app.ToggleChatState();",
        """            if (DialPressHeld()) return;  // BOOT + dial = parent combo
            if (pet_mode_.load()) {
                const auto decision = kid_app::PetInputRouter::Decide(
                    pet_event_mode_.load(), kid_app::PetClick::Single);
                if (decision.valid && !pet_service_.PostLocal(decision.action))
                    ESP_LOGW(TAG, "PET_INPUT_REJECTED");
                return;
            }
            app.ToggleChatState();""", "board click hook")
    text = once(text, "        pwr_button_.OnClick([this]() {",
        """        boot_button_.OnLongPress([this]() {
            if (!DialPressHeld()) RequestPetModeToggle();
        });

        // Offline story controls: single advances a page, double starts or
        // exits the reader; triple feeds only on the Pet home screen.
        // Long BOOT still toggles Pet mode. All effects pass through NVS.
        boot_button_.OnDoubleClick([this]() {
            if (!pet_mode_.load() || DialPressHeld()) return;
            const auto decision = kid_app::PetInputRouter::Decide(
                pet_event_mode_.load(), kid_app::PetClick::Double);
            if (decision.valid && !pet_service_.PostLocal(decision.action))
                ESP_LOGW(TAG, "PET_INPUT_REJECTED");
        });
        boot_button_.OnMultipleClick([this]() {
            if (!pet_mode_.load() || DialPressHeld()) return;
            const auto decision = kid_app::PetInputRouter::Decide(
                pet_event_mode_.load(), kid_app::PetClick::Triple);
            if (decision.valid && !pet_service_.PostLocal(decision.action))
                ESP_LOGW(TAG, "PET_INPUT_REJECTED");
        }, 3);

        StartPetInput();

        pwr_button_.OnClick([this]() {
            if (pet_mode_.load()) {
                // ADR-009: in Pet mode PWR never powers off directly; the
                // same press reaches InputHub as SleepRequest (sleep flow).
                ESP_LOGI(TAG, "PET_PWR_CLICK_PET_MODE");
                return;
            }""", "board extra click hooks")
    text = once(text, "    void InitializeButtons() {",
        """    void InitializeButtons() {
        // Distinct Pet app identity at board setup; the upstream official
        // OTA0 firmware never emits this marker. Logging does not enable Pet.
        ESP_LOGI(TAG, "PET_FIRMWARE_BOOT_MARKER phase=board_init");
        // If the timed power-on test woke us, the PCF85063 keeps INT low
        // until AF/AIE are cleared; on the PMIC power-on pin that would read
        // as a long PWR press, so clear it before anything else.
        ClearRtcAlarm();
        // A single short-lived low-priority task repeats the marker after
        // serial monitoring can attach following esptool hard_reset.
        // No NVS, networking, GPIO, audio or EPD changes.
        // No NVS, networking, audio or EPD changes; I2C reads only.
        const BaseType_t pet_diag_started = xTaskCreate(
            [](void* context) {
                vTaskDelay(pdMS_TO_TICKS(5000));
                ESP_LOGI(TAG, "PET_FIRMWARE_BOOT_MARKER phase=delay5s");
                vTaskDelay(pdMS_TO_TICKS(7000));
                ESP_LOGI(TAG, "PET_FIRMWARE_BOOT_MARKER phase=delay12s");
                auto* self = static_cast<WaveshareEsp32s3ePaper3inch97*>(context);
                self->RunHardwareDiag();
                self->StartSelfTestTask();
                self->RunDiagLoop();  // never returns
                vTaskDelete(nullptr);
            }, "pet_boot_diag", 4096, this, 1, nullptr);
        if (pet_diag_started != pdPASS)
            ESP_LOGW(TAG, "PET_BOOT_DIAG_TASK_FAILED");""",
        "board Pet boot diagnostic")
    return text


def run(source: Path) -> None:
    git_head = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    if git_head != MANIFEST["firmware"]["commit"]:
        raise RuntimeError(f"Wrong firmware commit: {git_head}")
    dest = source / "components" / "kid_app"
    if dest.exists():
        raise RuntimeError(f"Target component already exists: {dest}")

    # Import the pinned audio queue patch from this repository, not upstream.
    sys.path.insert(0, str(ROOT))
    from scripts.p4_audio_queue_patch import (
        patch_audio_h, patch_audio_cc, patch_application_cc,
    )

    cmake = source / "main/CMakeLists.txt"
    header = source / BOARD_REL / "custom_lcd_display.h"
    impl = source / BOARD_REL / "custom_lcd_display.cc"
    board = source / BOARD_REL / "waveshare-s3-epaper-3.97.cc"
    audio_h = source / "main/audio/audio_service.h"
    audio_cc = source / "main/audio/audio_service.cc"
    application_cc = source / "main/application.cc"
    # Calculate all changes before writing anything so an anchor mismatch
    # cannot leave half a patched source tree.
    patches = {
        cmake: patch_main_cmake(cmake.read_text(encoding="utf-8")),
        header: patch_display_h(header.read_text(encoding="utf-8")),
        impl: patch_display_cc(impl.read_text(encoding="utf-8")),
        board: patch_board(board.read_text(encoding="utf-8")),
        audio_h: patch_audio_h(audio_h.read_text(encoding="utf-8")),
        audio_cc: patch_audio_cc(audio_cc.read_text(encoding="utf-8")),
        application_cc: patch_application_cc(application_cc.read_text(encoding="utf-8")),
    }
    src = ROOT / "firmware/components/kid_app"
    if not (src / "CMakeLists.txt").exists():
        raise RuntimeError("Missing kid_app CMakeLists.txt")
    shutil.copytree(src, dest)
    for path, content in patches.items():
        path.write_text(content, encoding="utf-8")
    print("P1 integration completed in disposable upstream checkout")
    for path in patches:
        print("Patched: " + str(path.relative_to(source)))


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--source", type=Path, required=True)
    args = p.parse_args()
    try:
        run(args.source.resolve())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as e:
        print(f"P1 integration rejected: {e}", file=sys.stderr)
        sys.exit(1)
