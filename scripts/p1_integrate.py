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
            // Prototype full refresh: physical partial refresh remains an open P1 task.
            EPD_DisplayPartBaseImage();
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
    kid_app::FreertosPetDisplayWorker pet_display_worker_{pet_mailbox_, 5000};
    SemaphoreHandle_t pet_screen_mutex_ = nullptr;
    std::atomic<bool> pet_mode_{false};
    std::atomic<bool> pet_toggle_busy_{false};
    std::atomic<bool> pet_runtime_ready_{false};
    std::atomic<kid_app::PetMode> pet_event_mode_{kid_app::PetMode::Idle};
    std::atomic<uint32_t> last_pet_scene_{0};

    static uint32_t PackPetScene(const kid_app::SceneModel& scene) {
        return (static_cast<uint32_t>(scene.book_page) << 16) |
               (static_cast<uint32_t>(scene.emotion) << 8) |
               static_cast<uint32_t>(scene.kind);
    }

    static kid_app::SceneModel UnpackPetScene(uint32_t bits) {
        return {static_cast<kid_app::SceneKind>(bits & 0xFF),
                static_cast<kid_app::Emotion>((bits >> 8) & 0xFF),
                static_cast<uint16_t>(bits >> 16)};
    }

    static bool StoryAudioBegin(uint64_t token, void* context) {
        auto* self = static_cast<WaveshareEsp32s3ePaper3inch97*>(context);
        if (!self || !self->pet_mode_.load() ||
            Application::GetInstance().GetDeviceState() != kDeviceStateIdle)
            return false;
        return Application::GetInstance().GetAudioService().BeginLocalStoryPcm(token);
    }
    static bool StoryAudioPush(uint64_t token, const int16_t* pcm,
                               size_t count, void* context) {
        auto* self = static_cast<WaveshareEsp32s3ePaper3inch97*>(context);
        if (!self || !self->pet_mode_.load() ||
            Application::GetInstance().GetDeviceState() != kDeviceStateIdle)
            return false;
        return Application::GetInstance().GetAudioService().TryQueueLocalStoryPcm(
            token, pcm, count);
    }
    static void StoryAudioCancel(void*) {
        Application::GetInstance().GetAudioService().CancelLocalStoryPcm();
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
        if (self->pet_mode_.load()) {
            if (scene.kind == kid_app::SceneKind::Reader &&
                self->story_sd_.Ready()) {
                okay = self->display_->ShowStoryPage(
                    *self->story_sd_.Reader(), scene.book_page);
                // Only speak after EPD shows a validated book page;
                // invalid/out-of-range pages must cancel prior narration.
                if (self->story_pcm_.Ready()) {
                    if (okay) self->story_pcm_.RequestPage(scene.book_page);
                    else self->story_pcm_.Stop();
                }
            }
            // Missing SD or a page outside a reviewed book falls back to
            // the pre-existing monochrome reader placeholder.
            if (!okay) okay = self->display_->ShowPetScene(scene);
        }
        xSemaphoreGive(self->pet_screen_mutex_);
        return okay;
    }

    static void PetCommitted(const kid_app::PetEvent& event,
                              kid_app::PumpResult outcome,
                              const kid_app::PetEffects& effects,
                              const kid_app::PetSnapshot& snapshot, void* context) {
        auto* self = static_cast<WaveshareEsp32s3ePaper3inch97*>(context);
        if (!self || outcome != kid_app::PumpResult::Committed) return;
        self->pet_event_mode_.store(snapshot.mode);
        // Suspend the old page immediately when a new page/scene is
        // committed. The new page starts only after the slow EPD refresh.
        if (self->story_pcm_.Ready() &&
            (snapshot.mode != kid_app::PetMode::Story || effects.repaint)) {
            // Invalidate queued audio now, not after the slower SD feeder
            // wakes up. A previously spoken page must not overlap EPD.
            StoryAudioCancel(self);
            self->story_pcm_.Stop();
        }
        const auto scene = kid_app::PetScenePresenter::Present(snapshot);
        self->last_pet_scene_.store(PackPetScene(scene));
        if (effects.repaint) self->pet_mailbox_.Publish(scene);
        // Deliberately do NOT play audio here. AudioCoordinator/AudioPort
        // will be wired separately; only committed events can emit effects.
        ESP_LOGI(TAG, "PET_EVENT_COMMITTED id_class=%s",
                 (event.id & kid_app::kLocalEventBit) ? "local" : "external");
    }

    bool InitializePetRuntime() {
        if (pet_runtime_ready_.load()) return true;
        if (!pet_screen_mutex_) pet_screen_mutex_ = xSemaphoreCreateMutex();
        if (!pet_screen_mutex_) return false;
        if (!pet_mailbox_ready_) {
            if (!pet_mailbox_.Initialize()) return false;
            pet_mailbox_ready_ = true;
        }
        if (!pet_display_task_ready_) {
            if (!pet_display_worker_.Start(&PetRender, this)) return false;
            pet_display_task_ready_ = true;
        }
        if (!pet_event_task_ready_) {
            kid_app::PetSnapshot restored{};
            if (!pet_service_.Start(&PetCommitted, this, &restored)) return false;
            const auto scene = kid_app::PetScenePresenter::Present(restored);
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
        pet_mailbox_.Publish(UnpackPetScene(last_pet_scene_.load()));
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

    // Three-way dial per Waveshare schematic: Button_Up GPIO4,
    // Button_Function GPIO5, Button_Down GPIO6 (active low, pulled up).
    // Button callbacks only enqueue raw edges; one task owns InputHub.
    Button dial_up_{GPIO_NUM_4};
    Button dial_press_{GPIO_NUM_5};
    Button dial_down_{GPIO_NUM_6};
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
        // BOOT/PWR keep their existing board handlers; only dial semantics
        // become durable Pet actions, and only while Pet mode is on.
        if (!pet_mode_.load()) return;
        const auto decision =
            kid_app::PetInputRouter::DecideDial(pet_event_mode_.load(), event.kind);
        if (decision.valid && !pet_service_.PostLocal(decision.action))
            ESP_LOGW(TAG, "PET_INPUT_REJECTED");
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
        ESP_LOGI(TAG, "PET_DIAG_DIAL idle_levels up=%d press=%d down=%d",
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
                // ADR-009: PWR is a cancellable sleep request in Pet mode,
                // never an immediate power-off. Sleep flow arrives in V2-09.
                ESP_LOGI(TAG, "PET_SLEEP_REQUEST_PENDING");
                return;
            }""", "board extra click hooks")
    text = once(text, "    void InitializeButtons() {",
        """    void InitializeButtons() {
        // Distinct Pet app identity at board setup; the upstream official
        // OTA0 firmware never emits this marker. Logging does not enable Pet.
        ESP_LOGI(TAG, "PET_FIRMWARE_BOOT_MARKER phase=board_init");
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
                static_cast<WaveshareEsp32s3ePaper3inch97*>(context)->RunHardwareDiag();
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
