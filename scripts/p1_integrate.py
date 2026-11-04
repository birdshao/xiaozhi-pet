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
    text = once(text, "    void ExitPetPreview();\n", """    // V4-05 picture book (48,000 B frames, MonoFrame layout; the caller
    // reads the card before): 1-bit full page, official 4-grey full page
    // (RAM 0x24/0x26 planes, then the 1-bit fallback as the partial base),
    // and a 1-bit partial refresh against an explicit previous image.
    bool ShowBookMono(const uint8_t* mono);
    bool ShowBookGrey(const uint8_t* ram24, const uint8_t* ram26, const uint8_t* mono);
    bool ShowBookPartial(const uint8_t* next, const uint8_t* previous);
    void ExitPetPreview();
""", "display header book")
    return text


def patch_display_voice_h(text: str) -> str:
    text = once(text, "#include <driver/gpio.h>",
                "#include <driver/gpio.h>\n#include <atomic>\n#include <cstring>",
                "display header voice includes")
    text = once(text, "    void EPD_DrawColorPixel(uint16_t x, uint16_t y, uint8_t color);",
        """    void EPD_DrawColorPixel(uint16_t x, uint16_t y, uint8_t color);

    // V6-01: XiaoZhi shows the recognised sentence (server "stt") as a
    // "user" chat message. The pet only counts them (never reads the text)
    // to tell "thinking" from "listening"; LVGL handling is unchanged.
    void SetChatMessage(const char* role, const char* content) override {
        if (role && content && content[0] && std::strcmp(role, "user") == 0)
            user_utterances_.fetch_add(1);
        LcdDisplay::SetChatMessage(role, content);
    }
    uint32_t UserUtterances() const { return user_utterances_.load(); }
    std::atomic<uint32_t> user_utterances_{0};
""", "display header voice utterances")
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

bool CustomEpdDisplay::ShowBookMono(const uint8_t* mono) {
    bool okay = false;
    if (mono && lvgl_port_lock(5000)) {
        if (buffer && epd_spi_data.buffer_len >= static_cast<int>(kid_app::MonoFrame::kSize)) {
            memcpy(buffer, mono, kid_app::MonoFrame::kSize);
            pet_preview_enabled_ = true;
            EPD_SendCommand(0x3C);  // border waveform as in EPD_Init (full)
            EPD_SendData(0x01);
            EPD_DisplayPartBaseImage();
            okay = true;
        }
        lvgl_port_unlock();
    }
    return okay;
}

bool CustomEpdDisplay::ShowBookGrey(const uint8_t* ram24, const uint8_t* ram26,
                                    const uint8_t* mono) {
    bool okay = false;
    if (ram24 && ram26 && mono && lvgl_port_lock(5000)) {
        const int n = static_cast<int>(kid_app::MonoFrame::kSize);
        if (buffer && epd_spi_data.buffer_len >= n) {
            pet_preview_enabled_ = true;
            // Waveshare's official EPD_3IN97_Init_4GRAY / Display_4Gray: the
            // EPD_Init registers, a fixed temperature (0x1A = 0x5A) that
            // selects the panel's 4-grey waveform, both RAM planes, then
            // 0x22 = 0xD7 (refresh without reloading the sensor value).
            EPD_Init();
            EPD_SendCommand(0x1A);
            EPD_SendData(0x5A);
            EPD_SendCommand(0x24);
            writeBytes(ram24, n);
            EPD_SendCommand(0x26);
            writeBytes(ram26, n);
            EPD_SendCommand(0x22);
            EPD_SendData(0xD7);
            EPD_SendCommand(0x20);
            read_busy();
            // Back to the 1-bit baseline without a refresh: registers and
            // sensor temperature as after boot, both RAMs = the page's 1-bit
            // fallback, so the next partial (question cards) diffs against it.
            EPD_Init();
            memcpy(buffer, mono, n);
            EPD_SendCommand(0x24);
            writeBytes(buffer, n);
            EPD_SendCommand(0x26);
            writeBytes(buffer, n);
            okay = true;
        }
        lvgl_port_unlock();
    }
    return okay;
}

bool CustomEpdDisplay::ShowBookPartial(const uint8_t* next, const uint8_t* previous) {
    bool okay = false;
    if (next && previous && lvgl_port_lock(5000)) {
        const int n = static_cast<int>(kid_app::MonoFrame::kSize);
        if (buffer && epd_spi_data.buffer_len >= n) {
            memcpy(buffer, next, n);
            pet_preview_enabled_ = true;
            // As ShowPetSceneFast, but RAM 0x26 gets the caller's previous
            // image: pixels that differ from the new one are driven (the
            // question cards over a grey page), all others are left alone.
            EPD_SendCommand(0x3C);  // border waveform: hold during partial
            EPD_SendData(0x80);
            EPD_SendCommand(0x26);
            writeBytes(previous, n);
            EPD_SendCommand(0x24);
            writeBytes(buffer, n);
            EPD_TurnOnDisplayPart();
            EPD_SendCommand(0x26);
            writeBytes(buffer, n);
            okay = true;
        }
        lvgl_port_unlock();
    }
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
    // V3-06: while the parent menu is open only its own frames reach the
    // panel. Pet commits, voice faces and power pictures still update
    // last_pet_scene_ and come back when the menu closes. Opening and
    // closing flip the gate under scene_state_mutex_, like every publish.
    struct GatedSceneMailbox : kid_app::FreertosPetSceneMailbox {
        std::atomic<bool> parent_only{false};
        bool Publish(const kid_app::SceneModel& scene) {
            if (parent_only.load() && scene.kind != kid_app::SceneKind::Parent) return false;
            return kid_app::FreertosPetSceneMailbox::Publish(scene);
        }
    };
    GatedSceneMailbox pet_mailbox_;
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
        if (ParentMenuOpen()) return;  // V3-06: nothing plays over the parent menu
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
        // V3-06: the parent volume cap (and the quiet-hours ceiling) limits
        // narration in software; XiaoZhi's codec volume is never changed.
        const uint8_t pct = self->StoryVolumePct();
        if (pct < 100) {
            if (count > self->story_scaled_.size()) return false;
            for (size_t i = 0; i < count; ++i) self->story_scaled_[i] = pcm[i];
            kid_app::ScalePcm(self->story_scaled_.data(), count, pct);
            pcm = self->story_scaled_.data();
        }
        return Application::GetInstance().GetAudioService().TryQueueLocalStoryPcm(
            token, pcm, count);
    }
    static void StoryAudioCancel(void*) {
        Application::GetInstance().GetAudioService().CancelLocalStoryPcm();
    }

    // Draws one frame. Caller holds pet_screen_mutex_ and checked pet_mode_.
    // source: gate (published scene), anim (director frame), cleanup,
    // status (new status readings on the displayed scene).
    bool DrawPetFrameLocked(const kid_app::SceneModel& base, const char* source,
                            const kid_app::StatusBar* status = nullptr) {
        // V2-05: the home status band keeps what the panel shows unless this
        // is a status refresh; a full refresh picks up the newest readings
        // for free. Published scenes never carry a status (UnpackScene).
        kid_app::SceneModel scene = base;
        scene.status = scene.kind != kid_app::SceneKind::Home ? kid_app::StatusBar{}
                       : status                              ? *status
                                                             : status_shown_;
        bool okay = false;
        const uint32_t epd_started_ms = NowMs();
        kid_app::RefreshMode mode = kid_app::RefreshMode::Full;
        // V4-05: anything else on the panel (or Pet mode left) means the
        // next book frame starts with a full page again.
        if (scene.kind != kid_app::SceneKind::Reader || planner_reset_.load())
            book_panel_valid_ = false;
        if (scene.kind == kid_app::SceneKind::Reader && story_sd_.Ready()) {
            // V4-05: grey / 1-bit page and the question cards from the card.
            // Only speak after EPD shows a validated book page: book_shown_
            // tells the reader on pet_input (BookReader::OnShown), which
            // starts the narration; the end picture, a page outside the
            // book or a failure falls back to the placeholder, quietly.
            okay = DrawBookLocked(scene, &mode);
            if (okay) book_shown_.store(PackPetScene(scene));
            else if (story_pcm_.Ready()) story_pcm_.Stop();
        }
        // Missing SD or a page outside a reviewed book falls back to
        // the pre-existing monochrome reader placeholder.
        if (planner_reset_.exchange(false)) refresh_planner_.ForceFull();
        if (!okay) {
            mode = refresh_planner_.Plan(scene);
            if (mode == kid_app::RefreshMode::Full) scene.status = NewestStatus(scene.kind);
            okay = mode == kid_app::RefreshMode::Partial
                       ? display_->ShowPetSceneFast(scene)
                       : display_->ShowPetScene(scene);
        }
        refresh_planner_.Completed(scene, mode, okay);
        if (okay && scene.kind == kid_app::SceneKind::Home) status_shown_ = scene.status;
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
                 "pose=%u src=%s rail=%u pet=%u status=%u",
                 static_cast<int>(scene.kind), scene.book_page, scene.home_selection,
                 mode == kid_app::RefreshMode::Partial ? "partial" : "full", okay ? 1 : 0,
                 static_cast<unsigned>(NowMs() - epd_started_ms),
                 static_cast<unsigned>(++epd_refreshes_), static_cast<unsigned>(scene.pose),
                 source, refresh_planner_.RailPartials(), refresh_planner_.PetPartials(),
                 refresh_planner_.StatusPartials());
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
        // Blink and cleanup only on a quiet, awake Pet home screen (never
        // on a voice face, also while it lingers for its minimum dwell).
        const bool ambient = !self->sleep_active_.load() && !self->napping_.load() &&
                             !self->morning_shown_.load() &&
                             displayed.kind == kid_app::SceneKind::Home &&
                             Application::GetInstance().GetDeviceState() == kDeviceStateIdle &&
                             self->voice_face_.load() == 0;
        kid_app::DirectorStep step{};
        {
            SceneStateLock lock(self->anim_mutex_);
            step = self->anim_director_.Idle(displayed, NowMs(), ambient,
                                             self->refresh_planner_.NeedsCleanup());
        }
        if (step.kind == kid_app::DirectorStep::Kind::None) {
            if (ambient) self->MaybeRefreshStatus(displayed);
            return;
        }
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
        self->anim_director_.FrameShown(NowMs(), okay);
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
            // V4-05: the open book owns the reader picture (page, question).
            if (scene.kind == kid_app::SceneKind::Reader) scene = self->BookSceneLocked();
            self->last_pet_scene_.store(PackPetScene(scene));
            if (effects.repaint && !self->sleep_active_.load())
                self->pet_mailbox_.Publish(self->WithVoiceFace(scene));
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
        // V5-04: only committed events speak. Feeding and saying hi get a
        // short reviewed line, played by the pet_prompt task (never here).
        if (event.action == kid_app::PetAction::Feed)
            self->PostPhrase(kid_app::PhraseContext::Feed);
        else if (event.action == kid_app::PetAction::Pickup)
            self->PostPhrase(kid_app::PhraseContext::Greeting);
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
            if (!story_sd_.Initialize(story_book_))
                ESP_LOGW(TAG, "PET_STORY_SD_FALLBACK");
            // V2-08: pet_prompt may now look for the sound pack.
            sound_sd_.store(story_sd_.Mounted() ? kSoundSdMounted : kSoundSdAbsent);
            // V4-05: what the dial needs, read once here (pet_input never
            // reads the card); no book = an empty outline (placeholder).
            if (story_sd_.Ready() &&
                !kid_app::BuildBookOutline(story_sd_.Reader()->Content(), &book_outline_))
                ESP_LOGW(TAG, "PET_BOOK_OUTLINE_FAILED");
            book_outline_ready_.store(true);
        }
        if (story_sd_.Ready() && !story_pcm_.Ready() &&
            !story_pcm_.Start("/sdcard/books", story_book_,
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
            // V3-04: opened by an alarm wake, it shows the good-morning
            // picture straight away (pet_input then starts the 60 s window;
            // it may already have, it sees pet_mode_ before this lock).
            const bool morning = morning_request_.load() || morning_shown_.load();
            const auto scene = morning ? MorningScene() : UnpackPetScene(last_pet_scene_.load());
            pet_mailbox_.Publish(scene);
            // TuanTuan waves hello when Pet mode opens on the home screen.
            {
                SceneStateLock anim_lock(anim_mutex_);
                anim_director_.Reset(NowMs());
            }
            RequestAnimation(morning ? kid_app::Animation::Morning : kid_app::Animation::Greet,
                             scene);
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
        // V3-06: the open parent menu takes every input: no pet action, no
        // sleep flow (PowerManager counts the menu as busy).
        if (parent_menu_.IsOpen()) {
            HandleParentAction(parent_menu_.OnInput(event.kind, NowMs()), "long");
            return;
        }
        // V3-03/V3-04: input is activity; the first dial touch in a nap or
        // in the morning window only wakes TuanTuan (restores the picture).
        if (pet_mode_.load() && HandlePowerEvent(power_.OnActivity(NowMs())) &&
            IsDialInput(event.kind))
            return;
        // Going to sleep: inputs can only cancel it (V2-09).
        if (pet_mode_.load() && sleep_flow_.Active()) {
            HandleSleepAction(sleep_flow_.OnInput(event.kind, NowMs()));
            return;
        }
        // V3-06: the parent combo in Pet mode opens the parent menu. (The
        // V1-05 RTC power-on test it used to run is covered by self-test
        // "alarm" steps before a sleep power-off.)
        if (event.kind == kid_app::InputKind::ParentCombo && pet_mode_.load()) {
            OpenParentMenu();
            return;
        }
        if (event.kind == kid_app::InputKind::SleepRequest && pet_mode_.load()) {
            HandleSleepAction(sleep_flow_.Request(NowMs()));
            return;
        }
        // BOOT/PWR keep their existing board handlers; only dial semantics
        // become durable Pet actions, and only while Pet mode is on.
        if (!pet_mode_.load()) return;
        // V4-05: in the reader the dial drives the open book (UI-only, like
        // the home rail); only "home" becomes a durable action (StopStory).
        if (pet_event_mode_.load() == kid_app::PetMode::Story && book_.Active()) {
            ApplyBookStep(book_.OnInput(event.kind, NowMs()), "input");
            return;
        }
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
            prompt_scheduler_.OnSelect(index, NowMs());
            return {};
        }
        if (event.kind != kid_app::InputKind::DialPress) return {};
        prompt_scheduler_.Cancel();
        StopPrompt();
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
                // V3-06: going to sleep (PWR, rail, idle, flat battery) wins
                // over the parent menu; an unsaved draft is dropped.
                if (parent_menu_.IsOpen()) CloseParentMenu("sleep");
                sleep_active_.store(true);
                prompt_scheduler_.Cancel();
                StopPrompt();
                sleep_shown_ms_.store(0);
                if (story_pcm_.Ready()) story_pcm_.Stop();
                Application::GetInstance().GetAudioService().CancelLocalStoryPcm();
                // V5-04 good night, after the cancel above so it is not cut.
                PostPhrase(kid_app::PhraseContext::Bedtime);
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
                SecondsUntilWake();  // logs PET_RHYTHM_NEXT_WAKE
                return;
            }
            case kid_app::SleepAction::RestoreScene: {
                sleep_active_.store(false);
                sleep_battery_ = false;  // V3-05: a retry sets it again
                StopPrompt();  // a cancelled sleep cuts the good night
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
        StopPrompt();
        if (story_pcm_.Ready()) story_pcm_.Stop();
        Application::GetInstance().GetAudioService().CancelLocalStoryPcm();
        // Let any queued Pet event commit to NVS first (bounded wait).
        for (int i = 0; i < 20 && !pet_service_.Settled(); ++i) vTaskDelay(pdMS_TO_TICKS(100));
        // ADR-010: the RTC alarm powers TuanTuan on at the plan's wake time;
        // a self-test "alarm" step wins so unattended trials come back soon.
        uint32_t wake_s = sleep_alarm_s_.exchange(0);
        // V3-05: a critical battery arms no rhythm alarm (waking into a dead
        // battery only drains it further); PWR powers TuanTuan on again.
        const bool battery = sleep_battery_;
        sleep_battery_ = false;
        const char* wake_by = wake_s ? "selftest" : "rhythm";
        // V3-04: the next alarm boot skips the morning power-off after a
        // self-test alarm, so the host can restore the board.
        WriteRtcWakeMark(wake_s ? kSelfTestWakeMark : 0);
        if (!wake_s && !battery) wake_s = SecondsUntilWake();
        // Nothing armed (RTC not set, or a flat battery): PWR wakes the pet.
        if (!wake_s) wake_by = battery ? "none_battery" : "none";
        const bool alarm = wake_s && ArmRtcAlarmIn(wake_s);
        ESP_LOGI(TAG, "PET_SLEEP_POWEROFF settled=%d alarm_s=%u armed=%d wake=%s battery=%d",
                 pet_service_.Settled() ? 1 : 0, static_cast<unsigned>(wake_s), alarm ? 1 : 0,
                 wake_by, battery ? 1 : 0);
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

    // ---- V6-01 listening / thinking / speaking faces ----
    // pet_input owns the tracker. The face is a UI-only overlay on the home
    // scene (like the rail highlight): XiaoZhi's state never reaches NVS and
    // last_pet_scene_ keeps the durable picture, so the normal home face
    // comes back by itself. The tracker settles fast XiaoZhi flips and keeps
    // faces >= 1.5 s apart (one partial each, never a refresh storm).
    kid_app::VoiceFaceTracker voice_tracker_;   // pet_input task only
    std::atomic<uint8_t> voice_face_{0};        // kid_app::VoiceFace shown
    // Self-test "voice" step: forced phase (0 = XiaoZhi's real state) and
    // extra recognised sentences for "think".
    std::atomic<uint8_t> selftest_voice_{0};
    std::atomic<uint32_t> selftest_heard_{0};

    kid_app::SceneModel WithVoiceFace(const kid_app::SceneModel& scene) const {
        return kid_app::ApplyVoiceFace(scene,
                                       static_cast<kid_app::VoiceFace>(voice_face_.load()));
    }

    void UpdateVoiceFace(DeviceState state) {
        auto link = kid_app::VoiceLink::Off;
        if (state == kDeviceStateConnecting || state == kDeviceStateListening)
            link = kid_app::VoiceLink::Listening;
        else if (state == kDeviceStateSpeaking)
            link = kid_app::VoiceLink::Speaking;
        const auto forced = static_cast<kid_app::VoiceFace>(selftest_voice_.load());
        if (forced != kid_app::VoiceFace::None)
            link = forced == kid_app::VoiceFace::Speaking ? kid_app::VoiceLink::Speaking
                                                          : kid_app::VoiceLink::Listening;
        const bool pet = pet_mode_.load();
        if (!pet || sleep_active_.load()) link = kid_app::VoiceLink::Off;
        // pet_input starts in InitializeButtons(), before the EPD display
        // exists; Pet mode (and so display_) only comes later.
        const uint32_t heard = (pet ? display_->UserUtterances() : 0) + selftest_heard_.load();
        kid_app::VoiceFace face{};
        if (!voice_tracker_.Update(link, heard, NowMs(), &face)) return;
        SceneStateLock lock(scene_state_mutex_);
        voice_face_.store(static_cast<uint8_t>(face));
        ESP_LOGI(TAG, "PET_VOICE_FACE face=%s state=%d src=%s", kid_app::VoiceFaceName(face),
                 static_cast<int>(state),
                 forced != kid_app::VoiceFace::None ? "selftest" : "xiaozhi");
        if (pet_mode_.load() && !sleep_active_.load())
            pet_mailbox_.Publish(WithVoiceFace(UnpackPetScene(last_pet_scene_.load())));
    }

    void InputTaskLoop() {
        RawKeyEdge edge{};
        for (;;) {
            const auto state = Application::GetInstance().GetDeviceState();
            input_hub_.SetVoiceActive(state == kDeviceStateListening ||
                                      state == kDeviceStateSpeaking);
            UpdateVoiceFace(state);
            while (xQueueReceive(pet_input_queue_, &edge, pdMS_TO_TICKS(20)) == pdTRUE) {
                const auto key = static_cast<kid_app::InputKey>(edge.key);
                if (edge.down) input_hub_.Press(key, edge.at_ms);
                else input_hub_.Release(key, edge.at_ms);
            }
            input_hub_.Tick(NowMs());
            kid_app::InputEvent event{};
            while (input_hub_.Next(&event)) HandleInputEvent(event);
            UpdateParentMenu();
            UpdateBook();
            UpdatePower(state);
            UpdateBattery(state);
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
            // Name prompts only on the idle home rail (not in quiet, voice,
            // reader or while going to sleep).
            prompt_scheduler_.SetEnabled(
                pet_mode_.load() && !sleep_flow_.Active() && state == kDeviceStateIdle &&
                !ParentMenuOpen() && kid_app::PetInputRouter::IsHomeRail(pet_event_mode_.load()));
            kid_app::PromptRequest prompt{};
            if (prompt_scheduler_.Tick(NowMs(), &prompt)) PostPrompt(prompt);
        }
    }

    // ---- V4-05 picture-book reader ----
    // pet_input owns the BookReader (dial, timers, audio, memory requests);
    // kid_epd owns the frames and the panel. They meet in two words: the
    // book scene (page + question overlay, published like the home rail)
    // and book_shown_ (the view the panel now shows from the card).
    // Grey pages use the official 4-grey waveform; flip this to show every
    // page's 1-bit fallback instead (format and reader stay the same).
    static constexpr bool kBookGreyEnabled = true;
    char story_book_[16] = "forest_friends";  // self-test "book" may change it
    kid_app::BookOutline book_outline_;        // written once before ready
    std::atomic<bool> book_outline_ready_{false};
    kid_app::BookReader book_;                 // pet_input task only
    std::atomic<uint32_t> book_scene_{0};      // packed; scene_state_mutex_
    std::atomic<uint32_t> book_shown_{0};      // kid_epd -> pet_input
    // kid_epd only (pet_screen_mutex_): page picture, panel picture, next.
    kid_app::MonoFrame* book_base_ = nullptr;
    kid_app::MonoFrame* book_panel_frame_ = nullptr;
    kid_app::MonoFrame* book_next_ = nullptr;
    kid_app::BookView book_panel_{};
    bool book_panel_valid_ = false;

    // Caller holds scene_state_mutex_.
    kid_app::SceneModel BookSceneLocked() const {
        const uint32_t packed = book_scene_.load();
        return packed ? UnpackPetScene(packed) : kid_app::BookScene({}, false);
    }

    void UpdateBook() {
        const uint32_t now = NowMs();
        if (pet_event_mode_.load() != kid_app::PetMode::Story) {
            if (book_.Active()) {
                book_.Close();
                SceneStateLock lock(scene_state_mutex_);
                book_scene_.store(0);
                ESP_LOGI(TAG, "PET_BOOK_CLOSE");
            }
            return;
        }
        // Timers wait while asleep, napping, in the parent menu or out of
        // Pet mode; the picture comes back from last_pet_scene_.
        if (!pet_mode_.load() || sleep_active_.load() || napping_.load() || ParentMenuOpen())
            return;
        if (!book_.Active()) {
            if (!book_outline_ready_.load()) return;
            book_.Open(&book_outline_, now);
            book_shown_.store(0);
            ESP_LOGI(TAG, "PET_BOOK_OPEN book=%s pages=%u grey=%u questions=%u kpb2=%d",
                     story_book_, static_cast<unsigned>(book_outline_.pages),
                     static_cast<unsigned>(book_outline_.GreyPages()),
                     static_cast<unsigned>(book_outline_.QuestionTotal()),
                     book_outline_.kpb2 ? 1 : 0);
            kid_app::BookStep open{};
            open.publish = true;
            ApplyBookStep(open, "open");
        }
        const uint32_t shown = book_shown_.exchange(0);
        if (shown) ApplyBookStep(book_.OnShown(kid_app::BookViewOf(UnpackPetScene(shown)), now),
                                 "shown");
        ApplyBookStep(book_.Tick(now), "tick");
    }

    void ApplyBookStep(const kid_app::BookStep& step, const char* why) {
        PlayBookClip(step.clip);
        if (step.publish) {
            SceneStateLock lock(scene_state_mutex_);
            const auto scene = book_.Scene();
            const uint32_t packed = PackPetScene(scene);
            book_scene_.store(packed);
            last_pet_scene_.store(packed);
            if (!sleep_active_.load()) pet_mailbox_.Publish(scene);
            const auto& view = book_.View();
            ESP_LOGI(TAG, "PET_BOOK_VIEW page=%u q=%u sel=%u marked=%d end=%d why=%s",
                     static_cast<unsigned>(view.page), static_cast<unsigned>(view.question),
                     static_cast<unsigned>(view.highlight), view.marked ? 1 : 0,
                     book_.AtEnd() ? 1 : 0, why);
        }
        if (step.answer != kid_app::BookAnswer::None) {
            static const char* const kResult[] = {"none", "choice", "right", "wrong"};
            ESP_LOGI(TAG, "PET_BOOK_ANSWER result=%s chosen=%u page=%u q=%u",
                     kResult[static_cast<int>(step.answer)], static_cast<unsigned>(step.chosen),
                     static_cast<unsigned>(book_.View().page),
                     static_cast<unsigned>(book_.View().question));
        }
        if (step.memory.kind != kid_app::BookMemoryKind::None) RecordBookMemory(step.memory);
        if (step.home) {
            ESP_LOGI(TAG, "PET_BOOK_HOME");
            if (!pet_service_.PostLocal(kid_app::PetAction::StopStory))
                ESP_LOGW(TAG, "PET_INPUT_REJECTED");
        }
    }

    // pet_input. The story worker plays it through StoryAudioBegin/Push
    // (Pet mode, awake, XiaoZhi idle, parent volume cap); last wins.
    void PlayBookClip(const kid_app::BookClip& clip) {
        if (clip.kind == kid_app::BookClipKind::None || !story_pcm_.Ready()) return;
        if (clip.kind == kid_app::BookClipKind::Stop) {
            story_pcm_.Stop();
            StoryAudioCancel(this);
            return;
        }
        if (sleep_active_.load()) return;
        const auto kind = clip.kind == kid_app::BookClipKind::Page     ? kid_app::StoryClip::Page
                          : clip.kind == kid_app::BookClipKind::Prompt ? kid_app::StoryClip::Prompt
                                                                       : kid_app::StoryClip::Option;
        static const char* const kKind[] = {"page", "prompt", "option"};
        story_pcm_.RequestClip(kind, clip.page, clip.question, clip.option);
        ESP_LOGI(TAG, "PET_BOOK_AUDIO kind=%s page=%u q=%u option=%u",
                 kKind[static_cast<int>(kind)], static_cast<unsigned>(clip.page),
                 static_cast<unsigned>(clip.question), static_cast<unsigned>(clip.option));
    }

    // ADR-012: a book like (dial press on a like option of a like question)
    // or "book finished" becomes a MemoryEvent only through BookMemoryEvent
    // (the kid_app choice guard). The ledger owns event ids (shared replay
    // window) and the day: both go back to 0, and the atomic Pet worker
    // fills them in. The legacy FreertosPetService keeps no MemoryStore:
    // until the board runs FreertosAtomicPetService (PR #64) this is logged
    // (sink=none) and dropped; then it is `RecordMemory(event)` here.
    void RecordBookMemory(const kid_app::BookMemoryRequest& request) {
        kid_app::MemoryEvent event{};
        if (!kid_app::BookMemoryEvent(book_outline_, request, kid_app::kLocalEventBit, 0,
                                      &event)) {
            ESP_LOGW(TAG, "PET_BOOK_MEMORY_REJECTED kind=%u", static_cast<unsigned>(request.kind));
            return;
        }
        event.id = 0;  // the Pet worker assigns the next local id
        if (event.kind == kid_app::MemoryEventKind::PreferenceChosen)
            ESP_LOGI(TAG, "PET_BOOK_MEMORY kind=like like=%u:%u sink=none",
                     static_cast<unsigned>(event.slot), static_cast<unsigned>(event.option));
        else
            ESP_LOGI(TAG, "PET_BOOK_MEMORY kind=finished book=%u sink=none",
                     static_cast<unsigned>(event.option));
    }

    bool AllocBookFrames() {
        if (book_base_) return true;
        void* memory[3] = {};
        for (auto& m : memory) {
            m = heap_caps_malloc(sizeof(kid_app::MonoFrame), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!m) {
                for (auto* f : memory) heap_caps_free(f);
                ESP_LOGE(TAG, "PET_BOOK_FRAMES_FAILED");
                return false;
            }
        }
        book_base_ = new (memory[0]) kid_app::MonoFrame();
        book_panel_frame_ = new (memory[1]) kid_app::MonoFrame();
        book_next_ = new (memory[2]) kid_app::MonoFrame();
        return true;
    }

    // kid_epd: 4-grey page. Card reads and plane split happen before the
    // display lock; one 192 KiB PSRAM block only while drawing.
    bool ShowBookGreyLocked(kid_app::ContentPackReader& content, uint16_t page) {
        const size_t grey_bytes = kid_app::ContentPackReader::kGreyBytes;
        const size_t plane = kid_app::ContentPackReader::kMonoBytes;
        auto* grey = static_cast<uint8_t*>(
            heap_caps_malloc(grey_bytes + 2 * plane, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!grey) return false;
        uint8_t* ram24 = grey + grey_bytes;
        uint8_t* ram26 = ram24 + plane;
        const bool okay = content.ReadGrey(page, grey, grey_bytes) &&
                          kid_app::EpdGreyPlanes(grey, grey_bytes, ram24, ram26) &&
                          display_->ShowBookGrey(ram24, ram26, book_base_->Data().data());
        heap_caps_free(grey);
        return okay;
    }

    // kid_epd (pet_screen_mutex_). A new page (or a question ending) is a
    // full refresh: 4-grey when the page has a grey picture, else 1-bit.
    // Question cards and the dial highlight are 1-bit partial refreshes on
    // top. False = not a page of this book or a card/panel failure.
    bool DrawBookLocked(const kid_app::SceneModel& scene, kid_app::RefreshMode* mode) {
        auto& content = story_sd_.Reader()->Content();
        const auto view = kid_app::BookViewOf(scene);
        kid_app::ContentPage page{};
        if (!content.Page(view.page, &page) || !AllocBookFrames()) {
            book_panel_valid_ = false;
            return false;
        }
        const uint32_t started_ms = NowMs();
        const bool same_page = book_panel_valid_ && book_panel_.page == view.page;
        const char* how = "partial";
        if (!same_page || (!view.question && book_panel_.question)) {
            book_panel_valid_ = false;
            if (!story_sd_.Reader()->ReadFrame(view.page, *book_base_)) return false;
            bool okay = false;
            how = "mono";
            if (page.has_grey && kBookGreyEnabled) {
                okay = ShowBookGreyLocked(content, view.page);
                how = okay ? "grey" : "mono_after_grey";
            }
            if (!okay) okay = display_->ShowBookMono(book_base_->Data().data());
            if (!okay) return false;
            *mode = kid_app::RefreshMode::Full;
            *book_panel_frame_ = *book_base_;
            book_panel_ = kid_app::BookView{};
            book_panel_.page = view.page;
            book_panel_valid_ = true;
        } else {
            *mode = kid_app::RefreshMode::Partial;
        }
        if (view.question && view != book_panel_) {
            *book_next_ = *book_base_;
            bool okay = kid_app::ComposeBookOverlay(content, view, *book_next_);
            // A question's first frame drives every card pixel (the page
            // under the cards may be grey) and clears the previous question's
            // cards; later frames only change the rings.
            if (okay && book_panel_.question != view.question)
                okay = kid_app::ForceBookOverlay(content, view, *book_next_, *book_panel_frame_);
            okay = okay && display_->ShowBookPartial(book_next_->Data().data(),
                                                     book_panel_frame_->Data().data());
            if (!okay) {
                book_panel_valid_ = false;
                ESP_LOGW(TAG, "PET_BOOK_DRAW_FAILED page=%u q=%u",
                         static_cast<unsigned>(view.page), static_cast<unsigned>(view.question));
                return false;
            }
            *book_panel_frame_ = *book_next_;
        }
        book_panel_ = view;
        ESP_LOGI(TAG, "PET_BOOK_DRAW page=%u q=%u sel=%u marked=%d how=%s ms=%u",
                 static_cast<unsigned>(view.page), static_cast<unsigned>(view.question),
                 static_cast<unsigned>(view.highlight), view.marked ? 1 : 0, how,
                 static_cast<unsigned>(NowMs() - started_ms));
        return true;
    }

    // ---- V2-07 option name prompts ----
    // pet_input owns the scheduler; one pet_prompt task streams the embedded
    // clip through the existing XiaoZhi AudioService (single I2S owner).
    kid_app::PromptScheduler prompt_scheduler_;
    QueueHandle_t prompt_queue_ = nullptr;
    // Software prompt volume (%); only the self-test "volume" step lowers
    // it, for this boot only; PromptVolumePct() applies the parent cap. The
    // XiaoZhi codec volume/NVS stay untouched.
    std::atomic<uint8_t> prompt_volume_pct_{100};
    static constexpr uint8_t kPromptStop = 0xFF;

    // ---- V2-08 sound pack and V5-04 pet lines ----
    // The same pet_prompt task owns the TF pack (/sdcard/sounds/default.kps,
    // opened and CRC-checked once SD was tried), the bank that falls back to
    // the embedded option clips / a chime, and the planner that picks the
    // pet's reviewed line through Personality. One audio path for all.
    static constexpr uint8_t kSoundSdPending = 0, kSoundSdAbsent = 1, kSoundSdMounted = 2;
    std::atomic<uint8_t> sound_sd_{kSoundSdPending};
    std::atomic<uint32_t> phrase_tokens_{0};
    kid_app::SoundPackFilePort sound_file_;
    kid_app::SoundPackReader sound_pack_;
    kid_app::SoundBank sound_bank_{&kid_app::LoadPromptClip};
    kid_app::PhrasePlanner phrase_planner_;  // gentle until the parent sets one (V5-06)
    bool sound_pack_tried_ = false;

    void LoadSoundPack() {
        sound_pack_tried_ = true;
        const uint32_t started_ms = NowMs();
        const char* state = "no_sd";
        if (sound_sd_.load() == kSoundSdMounted) {
            state = "absent";
            if (sound_file_.Open("/sdcard/sounds", "default")) {
                state = sound_pack_.Open(sound_file_) ? "ready" : "invalid";
                if (!sound_pack_.Ready()) sound_file_.Close();
            }
        }
        sound_bank_.UsePack(&sound_pack_);
        ESP_LOGI(TAG, "PET_SOUND_PACK state=%s clips=%u rev=%u ms=%u", state,
                 static_cast<unsigned>(sound_pack_.ClipCount()),
                 static_cast<unsigned>(sound_pack_.Revision()),
                 static_cast<unsigned>(NowMs() - started_ms));
    }

    // Any task. Never asleep (good night only while falling asleep) or
    // napping; the player re-checks voice / Pet mode before and while it
    // plays. Overwrites a waiting option prompt, like a newer prompt would.
    void PostPhrase(kid_app::PhraseContext context) {
        if (!pet_mode_.load() || napping_.load() || ParentMenuOpen() ||
            (sleep_active_.load() && context != kid_app::PhraseContext::Bedtime))
            return;
        kid_app::PromptRequest request{};
        request.phrase = static_cast<uint8_t>(1 + static_cast<uint8_t>(context));
        request.token = phrase_tokens_.fetch_add(1) + 1;
        PostPrompt(request);
    }

    void PostPrompt(const kid_app::PromptRequest& request) {
        if (prompt_queue_) xQueueOverwrite(prompt_queue_, &request);
    }
    void StopPrompt() {
        kid_app::PromptRequest stop{};
        stop.index = kPromptStop;
        PostPrompt(stop);
    }

    // Same guard as story narration (StoryAudioBegin/Push); the good-night
    // line may also play on the sleep scene.
    bool PromptAllowed(bool bedtime = false) {
        return pet_mode_.load() && (bedtime || !sleep_active_.load()) &&
               Application::GetInstance().GetDeviceState() == kDeviceStateIdle;
    }

    void PromptTaskLoop() {
        kid_app::PromptRequest request{};
        int16_t chunk[480];
        for (;;) {
            if (!sound_pack_tried_ && sound_sd_.load() != kSoundSdPending) LoadSoundPack();
            if (xQueueReceive(prompt_queue_, &request,
                              sound_pack_tried_ ? portMAX_DELAY : pdMS_TO_TICKS(500)) != pdTRUE)
                continue;
            // V5-04: pick the pet's line first; one too soon after the last
            // is dropped without cutting what is playing.
            const bool phrase = request.phrase != 0;
            const auto context =
                static_cast<kid_app::PhraseContext>(phrase ? request.phrase - 1 : 0);
            const bool bedtime = phrase && context == kid_app::PhraseContext::Bedtime;
            const uint16_t phrase_id = phrase ? phrase_planner_.Next(context, NowMs()) : 0;
            if (phrase && !phrase_id) {
                ESP_LOGI(TAG, "PET_PHRASE_SKIPPED ctx=%s reason=gap",
                         kid_app::PhraseContextName(context));
                continue;
            }
            auto& audio = Application::GetInstance().GetAudioService();
            // Newest request wins: invalidate audio queued for an older prompt.
            audio.CancelLocalStoryPcm();
            if (request.index == kPromptStop) continue;
            // Logs: PET_PROMPT_* index=<rail item>, PET_PHRASE_* id=<phrase id>.
            const char* kind = phrase ? "PHRASE" : "PROMPT";
            const char* key = phrase ? "id" : "index";
            const unsigned what = phrase ? phrase_id : request.index;
            kid_app::SoundClip clip;
            if (phrase ? !sound_bank_.LoadPhrase(phrase_id, &clip)
                       : !sound_bank_.LoadOption(request.index, &clip)) {
                ESP_LOGW(TAG, "PET_%s_FAILED %s=%u reason=clip", kind, key, what);
                continue;
            }
            const uint64_t session =
                (uint64_t{phrase ? 0x50485253u : 0x50524F4Du} << 32) | request.token;
            if (phrase)
                ESP_LOGI(TAG, "PET_PHRASE_PLAY id=%u ctx=%s src=%s token=%u samples=%u", what,
                         kid_app::PhraseContextName(context), kid_app::SoundSourceName(clip.source),
                         static_cast<unsigned>(request.token), static_cast<unsigned>(clip.samples));
            else
                ESP_LOGI(TAG, "PET_PROMPT_PLAY index=%u token=%u samples=%u src=%s", what,
                         static_cast<unsigned>(request.token), static_cast<unsigned>(clip.samples),
                         kid_app::SoundSourceName(clip.source));
            bool started = false, allowed = true;
            for (int i = 0; i < 80 && !started; ++i) {  // older audio drains first
                if (uxQueueMessagesWaiting(prompt_queue_)) break;
                allowed = PromptAllowed(bedtime);
                if (!allowed) break;
                started = audio.BeginLocalStoryPcm(session);
                if (!started) vTaskDelay(pdMS_TO_TICKS(10));
            }
            if (!started) {
                if (!allowed)
                    ESP_LOGI(TAG, "PET_%s_INTERRUPTED %s=%u reason=state", kind, key, what);
                else if (uxQueueMessagesWaiting(prompt_queue_))
                    ESP_LOGI(TAG, "PET_%s_INTERRUPTED %s=%u", kind, key, what);
                else
                    ESP_LOGW(TAG, "PET_%s_FAILED %s=%u reason=busy", kind, key, what);
                continue;
            }
            const uint32_t started_ms = NowMs();
            size_t offset = 0;
            int stalls = 0;
            bool interrupted = false, state_lost = false;
            while (offset < clip.samples) {
                if (uxQueueMessagesWaiting(prompt_queue_)) {
                    interrupted = true;
                    break;
                }
                const size_t n = clip.Copy(offset, chunk, 480);
                if (n == 0) break;  // TF card read failed
                kid_app::ScalePcm(chunk, n, PromptVolumePct());
                if (audio.TryQueueLocalStoryPcm(session, chunk, n)) {
                    offset += n;
                    stalls = 0;
                    continue;
                }
                // Refused: either the 2-frame queue is full (wait) or the
                // session was cancelled (voice, pet mode off, sleep).
                if (!PromptAllowed(bedtime)) {
                    state_lost = true;
                    break;
                }
                if (++stalls > 50) break;  // 0.5 s: output stuck
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            if (state_lost)
                ESP_LOGI(TAG, "PET_%s_INTERRUPTED %s=%u reason=state", kind, key, what);
            else if (interrupted)
                ESP_LOGI(TAG, "PET_%s_INTERRUPTED %s=%u", kind, key, what);
            else if (offset < clip.samples)
                ESP_LOGW(TAG, "PET_%s_FAILED %s=%u reason=queue", kind, key, what);
            else
                ESP_LOGI(TAG, "PET_%s_QUEUED %s=%u ms=%u", kind, key, what,
                         static_cast<unsigned>(NowMs() - started_ms));
        }
    }

    void StartPetInput() {
        prompt_queue_ = xQueueCreate(1, sizeof(kid_app::PromptRequest));
        if (!prompt_queue_ ||
            xTaskCreate([](void* context) {
                static_cast<WaveshareEsp32s3ePaper3inch97*>(context)->PromptTaskLoop();
            }, "pet_prompt", 6144, this, 2, nullptr) != pdPASS)
            ESP_LOGW(TAG, "PET_PROMPT_TASK_FAILED");
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

    // ---- V3-03 PowerManager and V3-04 good morning ----
    // pet_input owns power_; other tasks only post through the atomics.
    // While Pet mode is on, XiaoZhi's PowerSaveTimer no longer powers off
    // (see patch_power_save_timer); PowerManager goes through the sleep flow.
    kid_app::PowerManager power_;
    bool power_pet_mode_ = false;               // pet_input: Pet mode last loop
    std::atomic<bool> power_poke_{false};       // gesture activity
    std::atomic<bool> morning_request_{false};  // alarm wake / self-test step
    std::atomic<bool> morning_shown_{false};    // morning window (no ambient)
    std::atomic<bool> napping_{false};          // drowsy face (no ambient)
    std::atomic<bool> on_battery_{false};       // PMIC discharging (heartbeat)
    std::atomic<bool> selftest_seen_{false};    // a script ran this boot
    // PCF85063 RAM byte, written at every sleep power-off.
    static constexpr uint8_t kSelfTestWakeMark = 0xA5;
    static constexpr int kMorningEnterTries = 45;  // x 2 s for Pet mode

    static bool IsDialInput(kid_app::InputKind kind) {
        return kind == kid_app::InputKind::DialUp || kind == kid_app::InputKind::DialDown ||
               kind == kid_app::InputKind::DialPress || kind == kid_app::InputKind::DialLong;
    }

    // Home with the selection kept: Sunrise first, Animation::Morning then
    // holds the stretch and ends on the sun-up wave.
    kid_app::SceneModel MorningScene() {
        auto scene = UnpackPetScene(last_pet_scene_.load());
        scene.kind = kid_app::SceneKind::Home;
        scene.emotion = kid_app::Emotion::Happy;
        scene.pose = kid_app::ScenePose::Sunrise;
        return scene;
    }

    // UI only: the durable pet state is not touched.
    void PublishPowerScene(bool drowsy) {
        SceneStateLock lock(scene_state_mutex_);
        if (sleep_active_.load()) return;
        auto scene = UnpackPetScene(last_pet_scene_.load());
        if (drowsy) {
            if (!kid_app::PetInputRouter::IsHomeRail(pet_event_mode_.load())) return;
            scene.emotion = kid_app::Emotion::Sleepy;
        }
        pet_mailbox_.Publish(drowsy ? scene : WithVoiceFace(scene));
    }

    // pet_input only. True when this input ended a nap or the morning
    // window (the picture is restored, the input is not acted on).
    bool HandlePowerEvent(kid_app::PowerEvent event) {
        switch (event) {
            case kid_app::PowerEvent::None:
                return false;
            case kid_app::PowerEvent::EnterIdle:
                ESP_LOGI(TAG, "PET_POWER state=idle");
                return false;
            case kid_app::PowerEvent::EnterNap:
                napping_.store(true);
                PublishPowerScene(true);
                ESP_LOGI(TAG, "PET_POWER state=nap phase=%s", PhaseName(day_phase_.load()));
                return false;
            case kid_app::PowerEvent::LeaveNap:
                napping_.store(false);
                PublishPowerScene(false);
                ESP_LOGI(TAG, "PET_POWER state=active reason=wake");
                return true;
            case kid_app::PowerEvent::StayAwake:
                morning_shown_.store(false);
                PublishPowerScene(false);
                ESP_LOGI(TAG, "PET_MORNING_STAYED");
                return true;
            case kid_app::PowerEvent::Sleep:
                napping_.store(false);
                morning_shown_.store(false);
                ESP_LOGI(TAG, "PET_POWER_SLEEP reason=%s",
                         kid_app::SleepReasonName(power_.LastSleepReason()));
                HandleSleepAction(sleep_flow_.Request(NowMs()));
                return false;
        }
        return false;
    }

    void BeginMorning(uint32_t now) {
        power_.StartMorning(now);
        napping_.store(false);
        morning_shown_.store(true);
        {
            // Same picture TogglePetRuntime published if it opened Pet mode
            // for this wake (the gate skips the repeat).
            SceneStateLock lock(scene_state_mutex_);
            const auto scene = MorningScene();
            pet_mailbox_.Publish(scene);
            RequestAnimation(kid_app::Animation::Morning, scene);
        }
        PostPhrase(kid_app::PhraseContext::MorningRecap);  // V5-04 good morning
        ESP_LOGI(TAG, "PET_MORNING_BEGIN awake_s=%u phase=%s",
                 static_cast<unsigned>(kid_app::PowerManager::kMorningAwakeMs / 1000),
                 PhaseName(day_phase_.load()));
    }

    // pet_input, every loop after the inputs.
    void UpdatePower(DeviceState state) {
        const uint32_t now = NowMs();
        const bool pet = pet_mode_.load();
        if (pet != power_pet_mode_) {
            power_pet_mode_ = pet;
            power_.Reset(now);
            napping_.store(false);
            morning_shown_.store(false);
            ESP_LOGI(TAG, "PET_POWER state=active reason=%s", pet ? "pet_on" : "pet_off");
        }
        if (!pet) return;
        // Sleep cancelled, or the PMIC did not cut power: awake again.
        if (power_.State() == kid_app::PowerState::Off && !sleep_flow_.Active()) {
            power_.Reset(now);
            ESP_LOGI(TAG, "PET_POWER state=active reason=sleep_ended");
        }
        if (morning_request_.exchange(false)) BeginMorning(now);
        if (power_poke_.exchange(false)) HandlePowerEvent(power_.OnActivity(now));
        kid_app::PowerConditions conditions;
        // A voice face still on the panel (its dwell, or a self-test
        // "voice" step) counts as a voice session too; so does a parent at
        // the menu (it closes itself after 30 s).
        conditions.busy = state != kDeviceStateIdle || sleep_flow_.Active() ||
                          voice_face_.load() != 0 || ParentMenuOpen();
        conditions.may_power_off = on_battery_.load() && !selftest_seen_.load() &&
                                   !battery_.External();  // V3-05: never while charging
        conditions.low_battery = battery_.LowPower();
        const int phase = day_phase_.load();
        conditions.phase_known = phase >= 0;
        if (conditions.phase_known) conditions.phase = static_cast<kid_app::DayPhase>(phase);
        HandlePowerEvent(power_.Tick(now, conditions));
    }

    // As after an RTC-alarm power-on: open Pet mode (XiaoZhi must be idle)
    // and let pet_input show the morning. Runs on pet_selftest.
    bool RunMorningWake(const char* source) {
        ESP_LOGI(TAG, "PET_MORNING_WAKE src=%s pet=%d", source, pet_mode_.load() ? 1 : 0);
        morning_request_.store(true);
        for (int attempt = 0; attempt < kMorningEnterTries && !pet_mode_.load(); ++attempt) {
            RequestPetModeToggle();
            for (int poll = 0; poll < 20 && !pet_mode_.load(); ++poll)
                vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (pet_mode_.load()) return true;
        morning_request_.store(false);
        ESP_LOGW(TAG, "PET_MORNING_FALLBACK reason=pet_mode state=%d",
                 static_cast<int>(Application::GetInstance().GetDeviceState()));
        return false;
    }

    // Real alarm wake, but Pet mode never opened (e.g. no Wi-Fi): keep the
    // rhythm anyway. Sleeping picture, tomorrow's alarm, PMIC off.
    void MorningFallbackPowerOff() {
        if (Application::GetInstance().GetDeviceState() == kDeviceStateUpgrading) return;
        if (pet_screen_mutex_ &&
            xSemaphoreTake(pet_screen_mutex_, pdMS_TO_TICKS(30000)) != pdTRUE)
            return;
        kid_app::SceneModel scene{};
        scene.kind = kid_app::SceneKind::Sleep;
        const bool shown = display_->ShowPetScene(scene);
        if (pet_screen_mutex_) xSemaphoreGive(pet_screen_mutex_);
        WriteRtcWakeMark(0);
        const uint32_t wake_s = SecondsUntilWake();
        const bool alarm = wake_s && ArmRtcAlarmIn(wake_s);
        ESP_LOGI(TAG, "PET_MORNING_POWEROFF shown=%d alarm_s=%u armed=%d", shown ? 1 : 0,
                 static_cast<unsigned>(wake_s), alarm ? 1 : 0);
        vTaskDelay(pdMS_TO_TICKS(300));
        if (pmic_) pmic_->PowerOff();
        vTaskDelay(pdMS_TO_TICKS(3000));
        ESP_LOGW(TAG, "PET_MORNING_POWEROFF_RETURNED");
        if (alarm) ClearRtcAlarm(false);
    }

    // -1 when the RTC cannot be read.
    int RtcWakeMark() {
        uint8_t reg = kid_app::Pcf85063::kRamByte, value = 0;
        return DiagI2c(0x51, &reg, 1, &value, 1) ? value : -1;
    }
    void WriteRtcWakeMark(uint8_t value) {
        const uint8_t write[] = {kid_app::Pcf85063::kRamByte, value};
        if (!DiagI2c(0x51, write, sizeof(write), nullptr, 0))
            ESP_LOGW(TAG, "PET_RTC_WAKE_MARK refused=i2c_error");
    }

    // ---- V3-05 low battery and charging ----
    // pet_boot_diag samples the PMIC (or what a self-test "battery" step
    // set) every 2 s into one word; pet_input owns the policy and acts on
    // it. Reactions are UI only, like the voice faces: no NVS, no Pet action.
    kid_app::BatteryPolicy battery_;           // pet_input only
    uint16_t battery_seen_ = 0;                // pet_input: last sample handled
    bool battery_pet_ = false;                 // pet_input: Pet mode last loop
    uint32_t battery_pet_since_ms_ = 0;        // pet_input: Pet mode opened
    bool sleep_battery_ = false;               // pet_input: critical, no alarm
    uint16_t battery_seq_ = 0;                 // pet_boot_diag only
    uint32_t battery_sample_ms_ = 0;           // pet_boot_diag only
    std::atomic<uint32_t> battery_sample_{0};  // PackBatteryReading | seq << 16
    std::atomic<uint32_t> battery_inject_{0};  // self-test reading, 0 = PMIC
    std::atomic<bool> battery_scripted_{false};  // a script set the battery
    static constexpr uint32_t kBatterySampleMs = 2000;
    static constexpr uint32_t kBatteryReactAfterPetMs = 10000;  // greet first

    // pet_boot_diag. False: no reading (board without PMIC).
    bool ReadBattery(kid_app::BatteryReading* out) {
        const uint32_t injected = battery_inject_.load();
        if (injected) {
            *out = kid_app::UnpackBatteryReading(injected);
            return true;
        }
        if (!pmic_) return false;
        out->pct = pmic_->GetBatteryLevel();
        out->charging = pmic_->IsCharging();
        out->on_battery = pmic_->IsDischarging();
        return true;
    }

    void SampleBattery(uint32_t now) {
        if (battery_sample_ms_ && now - battery_sample_ms_ < kBatterySampleMs) return;
        battery_sample_ms_ = now ? now : 1;
        kid_app::BatteryReading reading{};
        if (!ReadBattery(&reading)) return;
        if (++battery_seq_ == 0) battery_seq_ = 1;  // 0: nothing sampled yet
        battery_sample_.store(kid_app::PackBatteryReading(reading) |
                              (static_cast<uint32_t>(battery_seq_) << 16));
    }

    // pet_input, every loop after UpdatePower.
    void UpdateBattery(DeviceState state) {
        const uint32_t now = NowMs();
        const bool pet = pet_mode_.load();
        if (pet && !battery_pet_) battery_pet_since_ms_ = now;
        battery_pet_ = pet;
        const uint32_t sample = battery_sample_.load();
        const auto seq = static_cast<uint16_t>(sample >> 16);
        if (seq != battery_seen_) {
            battery_seen_ = seq;
            const auto reading = kid_app::UnpackBatteryReading(sample);
            const auto event = battery_.Update(reading, now);
            if (event != kid_app::BatteryEvent::None) {
                ESP_LOGI(TAG, "PET_BATTERY event=%s state=%s pct=%d chg=%d bat=%d src=%s",
                         kid_app::BatteryEventName(event),
                         kid_app::BatteryStateName(battery_.State()), reading.pct,
                         reading.charging ? 1 : 0, reading.on_battery ? 1 : 0,
                         battery_inject_.load() ? "selftest" : "pmic");
                HandleBatteryEvent(event, now);
            }
        }
        // Eat / yawn only on the awake Pet home screen: not going to sleep,
        // napping, in the morning window, talking or on the reader; and
        // not in a scripted trial that does not script the battery.
        const bool allowed =
            pet && now - battery_pet_since_ms_ >= kBatteryReactAfterPetMs &&
            !sleep_flow_.Active() && !sleep_active_.load() && !napping_.load() &&
            !morning_shown_.load() && voice_face_.load() == 0 && state == kDeviceStateIdle &&
            kid_app::PetInputRouter::IsHomeRail(pet_event_mode_.load()) &&
            (!selftest_seen_.load() || battery_scripted_.load()) &&
            !ParentMenuOpen();  // V3-06: not over the parent menu
        const auto reaction = battery_.Reaction(now, allowed);
        if (reaction == kid_app::BatteryReaction::None) return;
        const bool acted = RequestHomeAnimation(reaction == kid_app::BatteryReaction::Eat
                                                    ? kid_app::Animation::Charge
                                                    : kid_app::Animation::Yawn);
        ESP_LOGI(TAG, "PET_BATTERY_REACTION anim=%s acted=%d pct=%d",
                 kid_app::BatteryReactionName(reaction), acted ? 1 : 0, battery_.Percent());
    }

    // pet_input. Critical: the usual sleep flow (queued pet events commit,
    // sleeping picture, cancel window, PMIC off) but no wake alarm; it is
    // never a direct PMIC power-off.
    void HandleBatteryEvent(kid_app::BatteryEvent event, uint32_t now) {
        switch (event) {
            case kid_app::BatteryEvent::Plugged:
                if (!pet_mode_.load()) return;
                // Plugged in while going to sleep for a flat battery: stay up.
                if (sleep_battery_ && sleep_flow_.Cancel()) {
                    ESP_LOGI(TAG, "PET_BATTERY_SHUTDOWN_CANCELLED reason=plugged");
                    HandleSleepAction(kid_app::SleepAction::RestoreScene);
                }
                // Someone plugged the cable in: activity (ends a nap).
                HandlePowerEvent(power_.OnActivity(now));
                return;
            case kid_app::BatteryEvent::Critical:
                if (!pet_mode_.load()) {
                    ESP_LOGW(TAG, "PET_BATTERY_SHUTDOWN skipped=pet_off pct=%d",
                             battery_.Percent());
                    return;
                }
                sleep_battery_ = true;  // also when a sleep is already under way
                ESP_LOGI(TAG, "PET_BATTERY_SHUTDOWN pct=%d", battery_.Percent());
                if (!sleep_flow_.Active()) HandleSleepAction(sleep_flow_.Request(now));
                return;
            case kid_app::BatteryEvent::None:
            case kid_app::BatteryEvent::Unplugged:
            case kid_app::BatteryEvent::Full:
            case kid_app::BatteryEvent::EnteredLow:
                return;  // logged; the yawn follows from Reaction()
        }
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
    // PCF85063 time from a plausible server clock, and the wake-up alarm armed
    // at a sleep power-off. Nothing is stored or uploaded.
    uint8_t imu_address_ = 0;

    // ---- V4-02 gestures and V3 rhythm ----
    // The IMU windows are judged on pet_boot_diag; the self-test "gesture"
    // step injects into the same HandleGesture() path.
    kid_app::GestureDetector gesture_detector_;  // pet_boot_diag task only
    std::atomic<bool> face_down_{false};
    // The day plan comes from the parent settings (V3-06, see Rhythm()).
    std::atomic<int> day_phase_{-1};  // kid_app::DayPhase, -1 = RTC not set
    std::atomic<bool> quiet_hours_{false};

    static const char* GestureName(kid_app::Gesture gesture) {
        switch (gesture) {
            case kid_app::Gesture::Pickup: return "pickup";
            case kid_app::Gesture::GentleShake: return "gentle";
            case kid_app::Gesture::HardShake: return "hard";
            case kid_app::Gesture::FaceDown: return "facedown";
            case kid_app::Gesture::FaceUp: return "faceup";
            case kid_app::Gesture::None: break;
        }
        return "none";
    }

    static const char* PhaseName(int phase) {
        switch (phase) {
            case static_cast<int>(kid_app::DayPhase::Night): return "night";
            case static_cast<int>(kid_app::DayPhase::Morning): return "morning";
            case static_cast<int>(kid_app::DayPhase::Day): return "day";
            case static_cast<int>(kid_app::DayPhase::Nap): return "nap";
            case static_cast<int>(kid_app::DayPhase::Evening): return "evening";
        }
        return "unset";
    }

    // Plays `animation` on the current home scene; false off the home rail.
    bool RequestHomeAnimation(kid_app::Animation animation) {
        SceneStateLock lock(scene_state_mutex_);
        if (!kid_app::PetInputRouter::IsHomeRail(pet_event_mode_.load())) return false;
        RequestAnimation(animation, UnpackPetScene(last_pet_scene_.load()));
        return true;
    }

    // ADR-011: only light motion is rewarded. Pickup is the same durable
    // "the pet says hi" action as the Chat/Game rail items (a wave follows
    // from PetCommitted); shakes and peekaboo are reactions only.
    void HandleGesture(kid_app::Gesture gesture, const char* source) {
        if (gesture == kid_app::Gesture::None) return;
        if (gesture == kid_app::Gesture::FaceDown) face_down_.store(true);
        bool acted = false;
        // V3-06: parents can switch motion off; the parent menu ignores it.
        const bool motion_off = !motion_enabled_.load();
        if (pet_mode_.load() && !sleep_active_.load() && !motion_off && !ParentMenuOpen() &&
            Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
            if (power_save_timer_) power_save_timer_->WakeUp();
            power_poke_.store(true);
            switch (gesture) {
                case kid_app::Gesture::Pickup:
                    acted = kid_app::PetInputRouter::IsHomeRail(pet_event_mode_.load()) &&
                            pet_service_.PostLocal(kid_app::PetAction::Pickup);
                    break;
                case kid_app::Gesture::GentleShake:
                    acted = RequestHomeAnimation(kid_app::Animation::Giggle);
                    if (acted) PostPhrase(kid_app::PhraseContext::Giggle);
                    break;
                case kid_app::Gesture::HardShake:  // a gentle reminder, never a reward
                    acted = RequestHomeAnimation(kid_app::Animation::Dizzy);
                    break;
                case kid_app::Gesture::FaceUp:  // peekaboo: "found you"
                    acted = face_down_.exchange(false) &&
                            RequestHomeAnimation(kid_app::Animation::Greet);
                    break;
                case kid_app::Gesture::FaceDown:
                case kid_app::Gesture::None:
                    break;
            }
        }
        ESP_LOGI(TAG, "PET_GESTURE kind=%s src=%s acted=%d%s", GestureName(gesture), source,
                 acted ? 1 : 0, motion_off ? " motion_off=1" : "");
    }

    // Day phase and quiet hours from the PCF85063 wall clock (offline).
    void UpdateRhythm() {
        kid_app::CivilTime rtc{};
        bool stopped = true;
        const bool ok = RtcRead(&rtc, &stopped) && !stopped && kid_app::PlausibleWallClock(rtc);
        const auto rhythm = Rhythm();
        const int phase = ok ? static_cast<int>(rhythm.PhaseAt(rtc)) : -1;
        const bool quiet = ok && rhythm.QuietAt(rtc);
        const bool phase_changed = day_phase_.exchange(phase) != phase;
        const bool quiet_changed = quiet_hours_.exchange(quiet) != quiet;
        if (phase_changed || quiet_changed)
            ESP_LOGI(TAG, "PET_RHYTHM phase=%s quiet=%d time=%02u:%02u", PhaseName(phase),
                     quiet ? 1 : 0, ok ? rtc.hour : 0, ok ? rtc.minute : 0);
    }

    // Seconds until the plan's next wake-up, 0 when the RTC is not set.
    uint32_t SecondsUntilWake() {
        kid_app::CivilTime rtc{};
        bool stopped = true;
        if (!RtcRead(&rtc, &stopped) || stopped || !kid_app::PlausibleWallClock(rtc)) return 0;
        const auto rhythm = Rhythm();
        const uint32_t seconds = rhythm.SecondsUntilWake(rtc);
        const auto at = rhythm.NextWake(rtc);
        ESP_LOGI(TAG, "PET_RHYTHM_NEXT_WAKE in_s=%u at=%02u:%02u", static_cast<unsigned>(seconds),
                 at.hour, at.minute);
        return seconds;
    }

    // Prompt and pet-line volume: the self-test "volume" step, capped by the
    // parent volume and, in quiet hours, by the parent's quiet ceiling (V3-06).
    uint8_t PromptVolumePct() const {
        return kid_app::LimitVolumePct(ParentSettingsNow(), prompt_volume_pct_.load(),
                                       quiet_hours_.load());
    }
    uint8_t StoryVolumePct() const {
        return kid_app::LimitVolumePct(ParentSettingsNow(), 100, quiet_hours_.load());
    }
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

    // ---- V2-05 status bar (PRD 5.1) ----
    // pet_boot_diag reads the RTC, PMIC and Wi-Fi every heartbeat (SHTC3 once
    // a minute) and publishes one packed word; kid_epd draws it from PetIdle.
    std::atomic<uint32_t> status_bits_{kid_app::PackStatus(kid_app::StatusBar{})};
    kid_app::StatusBar status_latest_;  // pet_boot_diag only (hysteresis)
    bool env_read_once_ = false;        // pet_boot_diag only
    bool env_ok_ = false;
    int env_temp_c10_ = 0;
    uint32_t env_read_ms_ = 0;
    static constexpr uint32_t kEnvReadMs = 60000;
    kid_app::StatusBar status_shown_;          // kid_epd only: on the panel
    kid_app::StatusSchedule status_schedule_;  // kid_epd only

    // Newest readings for a full refresh (the band is drawn on Home only).
    kid_app::StatusBar NewestStatus(kid_app::SceneKind kind) const {
        return kind == kid_app::SceneKind::Home ? kid_app::UnpackStatus(status_bits_.load())
                                                : kid_app::StatusBar{};
    }

    // SHTC3 (0x70): wake, one normal-mode measurement (T first), sleep.
    bool ReadShtc3TempC10(int* temp_c10) {
        const uint8_t wake[] = {0x35, 0x17}, measure[] = {0x78, 0x66}, sleep[] = {0xB0, 0x98};
        uint8_t env[6] = {};
        bool ok = DiagI2c(0x70, wake, sizeof(wake), nullptr, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        ok = ok && DiagI2c(0x70, measure, sizeof(measure), env, sizeof(env), 40);
        DiagI2c(0x70, sleep, sizeof(sleep), nullptr, 0);
        return ok && kid_app::Shtc3TempC10(env, temp_c10);
    }

    void UpdateStatusBar() {
        const uint32_t now = NowMs();
        kid_app::StatusReadings in{};
        kid_app::CivilTime rtc{};
        bool stopped = true;
        if (RtcRead(&rtc, &stopped) && !stopped && kid_app::PlausibleWallClock(rtc)) {
            in.time_ok = true;
            in.hour = rtc.hour;
            in.minute = rtc.minute;
        }
        if (!env_read_once_ || now - env_read_ms_ >= kEnvReadMs) {
            env_read_once_ = true;
            env_read_ms_ = now;
            env_ok_ = ReadShtc3TempC10(&env_temp_c10_);
        }
        in.temp_ok = env_ok_;
        in.temp_c10 = env_temp_c10_;
        if (pmic_) {
            in.battery_pct = pmic_->GetBatteryLevel();
            in.charging = pmic_->IsCharging();
        }
        // V3-05: a self-test "battery" step stands in for the PMIC.
        if (const uint32_t injected = battery_inject_.load()) {
            const auto reading = kid_app::UnpackBatteryReading(injected);
            in.battery_pct = reading.pct;
            in.charging = reading.charging;
        }
        in.quiet = quiet_hours_.load();
        in.network = WifiManager::GetInstance().IsConnected() ? kid_app::NetworkState::Online
                                                               : kid_app::NetworkState::Offline;
        const auto bar = kid_app::MakeStatusBar(in, status_latest_);
        if (bar != status_latest_)
            ESP_LOGI(TAG, "PET_STATUS time=%02u:%02u temp_c=%d batt_bars=%u chg=%d quiet=%d net=%u",
                     bar.hour, bar.minute, bar.temp_c, bar.battery_bars, bar.charging ? 1 : 0,
                     bar.quiet ? 1 : 0, static_cast<unsigned>(bar.network));
        status_latest_ = bar;
        status_bits_.store(kid_app::PackStatus(bar));
    }

    // kid_epd (PetIdle) with nothing else to draw on a quiet, awake home
    // screen: new readings reach the panel only when they look different,
    // at most once a minute, as one small partial refresh of the band.
    void MaybeRefreshStatus(const kid_app::SceneModel& displayed) {
        if (displayed.kind != kid_app::SceneKind::Home) return;
        const auto latest = kid_app::UnpackStatus(status_bits_.load());
        if (!status_schedule_.Due(status_shown_, latest, NowMs())) return;
        {
            SceneStateLock lock(anim_mutex_);
            if (anim_director_.Active()) return;  // never between two frames
        }
        if (xSemaphoreTake(pet_screen_mutex_, pdMS_TO_TICKS(1000)) != pdTRUE) return;
        bool okay = false;
        if (pet_mode_.load() && !sleep_active_.load())
            okay = DrawPetFrameLocked(displayed, "status", &latest);
        xSemaphoreGive(pet_screen_mutex_);
        // Only status refreshes count (a failed one retries a minute later);
        // full refreshes pick up new readings without delaying the next one.
        status_schedule_.Drawn(NowMs());
        ESP_LOGI(TAG, "PET_STATUS_REFRESH ok=%d", okay ? 1 : 0);
    }

    // ---- V3-06 parent menu (PRD 4, 6, 10) ----
    // BOOT + dial press held 5 s in Pet mode. pet_input owns parent_menu_.
    // The settings are read by pet_boot_diag (day plan, motion), pet_prompt
    // and the story worker (volume) and pet_input / pet_selftest (next wake
    // alarm), so readers copy them under parent_mutex_. Saved to NVS key
    // kid_parent/settings (ParentSettingsCodec, as in ledger v3); XiaoZhi's
    // own NVS settings are never touched. Opening, moving and leaving the
    // menu are not pet interactions (nothing reaches pet_service_).
    kid_app::ParentMenu parent_menu_;              // pet_input task only
    kid_app::NvsParentSettingsStore parent_store_;
    kid_app::ParentSettings parent_settings_;      // guarded by parent_mutex_
    mutable std::mutex parent_mutex_;
    std::atomic<bool> parent_ready_{false};        // loaded (or defaults)
    std::atomic<bool> motion_enabled_{true};
    std::atomic<bool> rhythm_refresh_{false};      // pet_boot_diag re-runs UpdateRhythm
    std::array<int16_t, 480> story_scaled_{};      // story worker only (StoryAudioPush)

    bool ParentMenuOpen() const { return pet_mailbox_.parent_only.load(); }

    kid_app::ParentSettings ParentSettingsNow() const {
        std::lock_guard<std::mutex> lock(parent_mutex_);
        return parent_settings_;
    }
    // Invalid plans fall back to the default day (RhythmScheduler).
    kid_app::RhythmScheduler Rhythm() const {
        return kid_app::RhythmScheduler(ParentSettingsNow().schedule);
    }

    void ApplyParentSettings(const kid_app::ParentSettings& settings, const char* source) {
        {
            std::lock_guard<std::mutex> lock(parent_mutex_);
            parent_settings_ = settings;
        }
        motion_enabled_.store(settings.motion_enabled);
        rhythm_refresh_.store(true);
        const auto& d = settings.schedule;
        ESP_LOGI(TAG, "PET_PARENT_SETTINGS src=%s wake=%02u:%02u evening=%02u:%02u "
                 "quiet=%02u:%02u-%02u:%02u nap=%d cap=%u quiet_cap=%u motion=%d", source,
                 d.wake_min / 60, d.wake_min % 60, d.evening_min / 60, d.evening_min % 60,
                 d.quiet_start_min / 60, d.quiet_start_min % 60, d.quiet_end_min / 60,
                 d.quiet_end_min % 60, d.nap_enabled ? 1 : 0, settings.volume_cap_pct,
                 settings.quiet_volume_pct, settings.motion_enabled ? 1 : 0);
    }

    // pet_boot_diag, once, before the self-test task (and so the morning
    // flow) starts. Missing or corrupt: the defaults; NVS is left as it is.
    void LoadParentSettings() {
        kid_app::ParentSettings loaded;
        const auto result = parent_store_.Load(&loaded);
        ESP_LOGI(TAG, "PET_PARENT_LOAD result=%s", kid_app::ParentLoadResultName(result));
        ApplyParentSettings(result == kid_app::NvsParentSettingsStore::LoadResult::Loaded
                                ? loaded : kid_app::ParentSettings{}, "nvs");
        parent_ready_.store(true);
    }

    // Caller holds scene_state_mutex_.
    void PublishParentSceneLocked() {
        kid_app::SceneModel scene{};
        scene.kind = kid_app::SceneKind::Parent;
        scene.parent = parent_menu_.View();
        pet_mailbox_.Publish(scene);
    }

    void OpenParentMenu() {
        const auto state = Application::GetInstance().GetDeviceState();
        if (!parent_ready_.load() || sleep_active_.load() || state != kDeviceStateIdle) {
            ESP_LOGW(TAG, "PET_PARENT_REFUSED ready=%d state=%d", parent_ready_.load() ? 1 : 0,
                     static_cast<int>(state));
            return;
        }
        prompt_scheduler_.Cancel();
        StopPrompt();
        if (story_pcm_.Ready()) story_pcm_.Stop();
        Application::GetInstance().GetAudioService().CancelLocalStoryPcm();
        parent_menu_.Open(ParentSettingsNow(), NowMs());
        {
            SceneStateLock lock(scene_state_mutex_);
            pet_mailbox_.parent_only.store(true);
            {
                SceneStateLock anim_lock(anim_mutex_);
                anim_director_.Reset(NowMs());
            }
            PublishParentSceneLocked();
        }
        ESP_LOGI(TAG, "PET_PARENT_OPEN");
    }

    // Back to the pet's own (durable) picture, as it is now.
    void CloseParentMenu(const char* reason) {
        parent_menu_.Close();
        input_hub_.SetAutoRepeat(false);
        {
            SceneStateLock lock(scene_state_mutex_);
            pet_mailbox_.parent_only.store(false);
            if (pet_mode_.load() && !sleep_active_.load())
                pet_mailbox_.Publish(WithVoiceFace(UnpackPetScene(last_pet_scene_.load())));
        }
        ESP_LOGI(TAG, "PET_PARENT_EXIT parent_exit=%s", reason);
    }

    void HandleParentAction(kid_app::ParentMenuAction action, const char* exit_reason) {
        const char* item = kid_app::ParentItemName(parent_menu_.Selected());
        switch (action) {
            case kid_app::ParentMenuAction::None:
                return;
            case kid_app::ParentMenuAction::Exit:
                CloseParentMenu(exit_reason);
                return;
            case kid_app::ParentMenuAction::Redraw:
                break;
            case kid_app::ParentMenuAction::Saved: {
                const auto next = parent_menu_.Settings();
                if (!parent_store_.Save(next)) {
                    // Keep running with what NVS holds; show it again.
                    parent_menu_.Revert(ParentSettingsNow());
                    ESP_LOGW(TAG, "PET_PARENT_SAVE_FAILED item=%s", item);
                    break;
                }
                ApplyParentSettings(next, "menu");
                ESP_LOGI(TAG, "PET_PARENT_SAVED item=%s", item);
                break;
            }
        }
        {
            SceneStateLock lock(scene_state_mutex_);
            PublishParentSceneLocked();
        }
        ESP_LOGI(TAG, "PET_PARENT_ITEM item=%s editing=%d", item, parent_menu_.Editing() ? 1 : 0);
    }

    // pet_input, every loop: 30 s idle exit, Pet mode left, and dial
    // auto-repeat while a value is being edited.
    void UpdateParentMenu() {
        if (!parent_menu_.IsOpen()) return;
        if (!pet_mode_.load()) {
            CloseParentMenu("pet_off");
            return;
        }
        HandleParentAction(parent_menu_.Tick(NowMs()), "idle");
        input_hub_.SetAutoRepeat(parent_menu_.Editing());
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
            // V3-04: good morning, then back to sleep with tomorrow's alarm.
            // After a self-test alarm the host restores the board instead.
            if (RtcWakeMark() == kSelfTestWakeMark)
                ESP_LOGI(TAG, "PET_MORNING_SKIPPED reason=selftest_wake");
            else if (!RunMorningWake("alarm"))
                MorningFallbackPowerOff();
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
        // PowerManager never powers off by itself during a trial.
        selftest_seen_.store(true);
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
                case kid_app::SelfTestOp::Gesture:
                    HandleGesture(static_cast<kid_app::Gesture>(step.ms), "selftest");
                    break;
                case kid_app::SelfTestOp::Morning:
                    // The parser wants an earlier alarm step; it must still
                    // be armed, or the morning power-off would not come back.
                    if (!sleep_alarm_s_.load() || !RunMorningWake("selftest")) {
                        ESP_LOGW(TAG, "PET_SELFTEST_ABORT reason=morning step=%u",
                                 static_cast<unsigned>(i));
                        delete script;
                        return;
                    }
                    break;
                case kid_app::SelfTestOp::Voice:
                    // A script cannot talk to XiaoZhi: force the phase the
                    // faces follow ("think" = one more recognised sentence).
                    if (step.ms == static_cast<uint32_t>(kid_app::VoiceFace::Thinking))
                        selftest_heard_.fetch_add(1);
                    selftest_voice_.store(static_cast<uint8_t>(step.ms));
                    ESP_LOGI(TAG, "PET_SELFTEST_VOICE phase=%s",
                             kid_app::VoiceFaceName(static_cast<kid_app::VoiceFace>(step.ms)));
                    break;
                case kid_app::SelfTestOp::Battery: {
                    // V3-05: readings as if from the PMIC (status band and
                    // battery policy); "off" hands back to the real PMIC.
                    const bool off = step.ms == kid_app::SelfTestScript::kBatteryOff;
                    kid_app::BatteryReading reading{};
                    reading.pct = static_cast<int>(step.ms & 0xFF);
                    reading.charging = (step.ms & kid_app::SelfTestScript::kBatteryCharging) != 0;
                    reading.on_battery = !reading.charging;
                    // A critical level powers off: the alarm must still be
                    // armed, or the board would not come back (as "morning").
                    if (!off && reading.on_battery &&
                        reading.pct < kid_app::BatteryPolicy::kCriticalPct &&
                        !sleep_alarm_s_.load()) {
                        ESP_LOGW(TAG, "PET_SELFTEST_ABORT reason=battery step=%u",
                                 static_cast<unsigned>(i));
                        delete script;
                        return;
                    }
                    battery_scripted_.store(true);
                    battery_inject_.store(off ? 0 : kid_app::PackBatteryReading(reading));
                    ESP_LOGI(TAG, "PET_SELFTEST_BATTERY pct=%d chg=%d off=%d", reading.pct,
                             reading.charging ? 1 : 0, off ? 1 : 0);
                    break;
                }
                case kid_app::SelfTestOp::Volume:
                    prompt_volume_pct_.store(static_cast<uint8_t>(step.ms));
                    ESP_LOGI(TAG, "PET_SELFTEST_VOLUME pct=%u", static_cast<unsigned>(step.ms));
                    break;
                case kid_app::SelfTestOp::Mark:
                    ESP_LOGI(TAG, "PET_SELFTEST_MARK name=%s", step.name);
                    break;
                case kid_app::SelfTestOp::Book:
                    // V4-05: read a test book from the card instead of the
                    // demo book. The card is opened once, when Pet mode
                    // first starts; later the step only logs.
                    if (pet_runtime_ready_.load()) {
                        ESP_LOGW(TAG, "PET_SELFTEST_BOOK_IGNORED id=%s", step.name);
                    } else {
                        std::memcpy(story_book_, step.name, sizeof(story_book_));
                        ESP_LOGI(TAG, "PET_SELFTEST_BOOK id=%s", story_book_);
                    }
                    break;
                case kid_app::SelfTestOp::Combo:
                    // V3-06 parent entry: BOOT and dial press held together.
                    PostKeyEdge(kid_app::InputKey::Boot, true);
                    PostKeyEdge(kid_app::InputKey::DialPress, true);
                    vTaskDelay(pdMS_TO_TICKS(step.ms));
                    PostKeyEdge(kid_app::InputKey::DialPress, false);
                    PostKeyEdge(kid_app::InputKey::Boot, false);
                    vTaskDelay(pdMS_TO_TICKS(step.gap_ms));
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
        bool gestures_on = false;
        gesture_detector_.SetEnabled(false);
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
                const bool want = pet_mode_.load() && !sleep_active_.load() &&
                                  motion_enabled_.load();  // V3-06 parent switch
                if (want != gestures_on) {
                    gesture_detector_.SetEnabled(want);  // re-enable starts fresh
                    gestures_on = want;
                }
                HandleGesture(gesture_detector_.OnWindow(s, now), "imu");
            }
            SampleBattery(now);
            if (now - last_heartbeat_ms >= 10000) {
                last_heartbeat_ms = now;
                SyncRtcFromSystemTime();
                UpdateRhythm();
                on_battery_.store(pmic_ && pmic_->IsDischarging());
                UpdateStatusBar();
                ESP_LOGI(TAG, "PET_DIAG_HEARTBEAT up_s=%u pet=%d pwr_edges=%u sleep_req=%u "
                         "batt=%d chg=%d woke_by_alarm=%d rtc_synced=%d epd_n=%u",
                         static_cast<unsigned>(now / 1000), pet_mode_.load() ? 1 : 0,
                         static_cast<unsigned>(pwr_edges_.load()),
                         static_cast<unsigned>(sleep_requests_.load()),
                         pmic_ ? pmic_->GetBatteryLevel() : -1,
                         pmic_ && pmic_->IsCharging() ? 1 : 0, woke_by_alarm_.load(),
                         rtc_synced_ ? 1 : 0, static_cast<unsigned>(epd_refreshes_.load()));
            }
            // New parent day plan: phase and quiet hours now, not at the next heartbeat.
            if (rhythm_refresh_.exchange(false)) UpdateRhythm();
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
        '#include "kid_app/voice_face.h"\n'
        '#include "kid_app/selftest_script.h"\n'
        '#include "kid_app/sleep_flow.h"\n'
        '#include "kid_app/prompt_scheduler.h"\n'
        '#include "kid_app/prompt_clips.h"\n'
        '#include "kid_app/sound_bank.h"\n'
        '#include "kid_app/pet_phrases.h"\n'
        '#include "kid_app/status_bar.h"\n'
        '#include <wifi_manager.h>\n'
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
        '#include "kid_app/gesture_detector.h"\n'
        '#include "kid_app/rhythm_scheduler.h"\n'
        '#include "kid_app/power_manager.h"\n'
        '#include "kid_app/battery_policy.h"\n'
        '#include "kid_app/parent_menu.h"\n'
        '#include "kid_app/nvs_parent_settings_store.h"\n'
        '#include <mutex>\n'
        '#include <ctime>\n'
        '#include <esp_timer.h>\n'
        '#include <freertos/queue.h>\n'
        '#include "kid_app/waveshare_story_sd.h"\n'
        '#include "kid_app/freertos_story_pcm_worker.h"\n'
        '#include "kid_app/book_reader.h"\n'
        '#include "kid_app/memory_store.h"\n'
        '#include <cstring>',
        "board headers")
    text = once(text, "    void InitializeButtons() {",
                BOARD_WORKER.rstrip() + "\n\n    void InitializeButtons() {",
                "board pet worker")
    text = once(text, "            app.ToggleChatState();",
        """            if (DialPressHeld()) return;  // BOOT + dial = parent combo
            if (ParentMenuOpen()) return;  // V3-06: BOOT does nothing in the parent menu
            if (pet_mode_.load()) {
                // V6-01: on the pet home screen BOOT talks to XiaoZhi (press
                // to start, press again to stop); TuanTuan listens, thinks
                // and speaks along (UpdateVoiceFace). Not while going to
                // sleep or in quiet time; the reader keeps BOOT = next page.
                const auto pet_mode = pet_event_mode_.load();
                if (kid_app::PetInputRouter::IsHomeRail(pet_mode)) {
                    if (!sleep_active_.load()) app.ToggleChatState();
                    return;
                }
                const auto decision = kid_app::PetInputRouter::Decide(
                    pet_mode, kid_app::PetClick::Single);
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
            if (!pet_mode_.load() || DialPressHeld() || ParentMenuOpen()) return;
            const auto decision = kid_app::PetInputRouter::Decide(
                pet_event_mode_.load(), kid_app::PetClick::Double);
            if (decision.valid && !pet_service_.PostLocal(decision.action))
                ESP_LOGW(TAG, "PET_INPUT_REJECTED");
        });
        boot_button_.OnMultipleClick([this]() {
            if (!pet_mode_.load() || DialPressHeld() || ParentMenuOpen()) return;
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
                self->LoadParentSettings();  // V3-06, before any rhythm use
                self->StartSelfTestTask();
                self->RunDiagLoop();  // never returns
                vTaskDelete(nullptr);
            }, "pet_boot_diag", 4096, this, 1, nullptr);
        if (pet_diag_started != pdPASS)
            ESP_LOGW(TAG, "PET_BOOT_DIAG_TASK_FAILED");""",
        "board Pet boot diagnostic")
    return text


def patch_power_save_timer(text: str) -> str:
    """V3-03: in Pet mode PowerManager owns power-off (sleeping picture and
    the RTC alarm for the next wake); XiaoZhi's 300 s timer only restarts."""
    return once(text, "        power_save_timer_->OnShutdownRequest([this]() {\n",
        "        power_save_timer_->OnShutdownRequest([this]() {\n"
        "            if (pet_mode_.load()) {\n"
        "                power_save_timer_->WakeUp();\n"
        "                return;\n"
        "            }\n",
        "board PowerSaveTimer takeover")


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
        header: patch_display_voice_h(patch_display_h(header.read_text(encoding="utf-8"))),
        impl: patch_display_cc(impl.read_text(encoding="utf-8")),
        board: patch_power_save_timer(patch_board(board.read_text(encoding="utf-8"))),
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
