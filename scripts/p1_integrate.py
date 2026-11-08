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
    // V4-06: the same for a mini game (game_only, only Game frames).
    struct GatedSceneMailbox : kid_app::FreertosPetSceneMailbox {
        std::atomic<bool> parent_only{false};
        std::atomic<bool> game_only{false};
        bool Publish(const kid_app::SceneModel& scene) {
            if (parent_only.load() && scene.kind != kid_app::SceneKind::Parent) return false;
            // V8: care activities, the diary and the album run in the game frame.
            if (game_only.load() && scene.kind != kid_app::SceneKind::Game &&
                scene.kind != kid_app::SceneKind::Care)
                return false;
            return kid_app::FreertosPetSceneMailbox::Publish(scene);
        }
    };
    GatedSceneMailbox pet_mailbox_;
    // The one owner of pet state, V5 memory and V3-06 parent settings: the
    // atomic ledger (kid_pet_sync), see StartPetLedger().
    kid_app::FreertosAtomicPetService pet_service_;
    // V5_V6 §11.2: the PetLearning link (pairing, state snapshots, server
    // commands) on its own task. It reads ledger copies only and hands
    // commands to the Pet task (PostCommand); see StartServerSync().
    kid_app::FreertosServerSyncWorker server_sync_{pet_service_};
    std::atomic<uint8_t> sync_battery_pct_{0};    // last PMIC reading for snapshots
    std::atomic<bool> sync_charging_{false};
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
        if (GameActive()) return;      // V4-06: nor over a mini game
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
        // V8: clothes, room, need bubble and festival prop (home only, never
        // packed): always the newest look.
        scene.look = scene.kind == kid_app::SceneKind::Home
                         ? kid_app::UnpackLook(home_look_.load())
                         : kid_app::HomeLook{};
        bool okay = false;
        const uint32_t epd_started_ms = NowMs();
        kid_app::RefreshMode mode = kid_app::RefreshMode::Full;
        // V4-05: anything else on the panel (or Pet mode left) means the
        // next book frame starts with a full page again.
        if (scene.kind != kid_app::SceneKind::Reader || planner_reset_.load())
            book_panel_valid_ = false;
        // No book (no card / no such book): the "no book" picture is a
        // plain scene (ShowPetScene), never a card read.
        if (scene.kind == kid_app::SceneKind::Reader && story_sd_.Ready() &&
            kid_app::BookMissingOf(scene) == kid_app::BookMissing::None) {
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
        if (okay && scene.kind == kid_app::SceneKind::Home) {
            status_shown_ = scene.status;
            look_shown_ = scene.look;  // V8
        }
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
            if (ambient) self->MaybeRefreshLook(displayed);  // V8
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
                              kid_app::CommitResult outcome,
                              const kid_app::PetEffects& effects,
                              const kid_app::PetSnapshot& snapshot, void* context) {
        auto* self = static_cast<WaveshareEsp32s3ePaper3inch97*>(context);
        // "Home" from the reader could not be saved (dropped after retries):
        // still show home, or the dial would stay in a dead reader.
        if (self && kid_app::StoryEscapeNeeded(event.action, outcome))
            self->EscapeStoryHome("persist");
        if (!self || outcome != kid_app::CommitResult::Committed) return;
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
        if (event.action == kid_app::PetAction::Feed && self->feed_sticker_.exchange(false)) {
            // V8: a sticker earned by this meal is announced after the line.
            const uint16_t news = kid_app::CarePhraseId(kid_app::CareLine::StickerNew, 0,
                                                        self->pet_service_.TemperamentNow());
            self->PostPhrase(kid_app::PhraseContext::Feed, &news, 1);
        } else if (event.action == kid_app::PetAction::Feed)
            self->PostPhrase(kid_app::PhraseContext::Feed);
        else if (event.action == kid_app::PetAction::Pickup)
            self->PostPhrase(kid_app::PhraseContext::Greeting);
        ESP_LOGI(TAG, "PET_EVENT_COMMITTED id_class=%s fed=%u",
                 (event.id & kid_app::kLocalEventBit) ? "local" : "external",
                 static_cast<unsigned>(snapshot.interaction_count));
    }

    // ---- Atomic Pet ledger on the board ----
    // Started once, by pet_boot_diag (the parent settings are needed before
    // Pet mode: rhythm, alarm) or by the first Pet-mode toggle, whichever
    // comes first. MigrateLegacyLocal: the one-time verified migration from
    // the old kid_pet checkpoint and kid_parent/settings (released once the
    // ledger is verified: 16 KiB NVS has no room for both), then no outbox
    // (no server on this board yet).
    // A refused start erases nothing; the next toggle tries again.
    std::mutex pet_ledger_mutex_;
    bool pet_ledger_ready_ = false;                // under pet_ledger_mutex_
    kid_app::PetSnapshot pet_ledger_restored_{};   // under pet_ledger_mutex_
    std::atomic<uint32_t> local_day_{0};           // RTC civil day, 0 = unknown

    // Pet worker: the day for today's memory counters (V5-02).
    static uint32_t LocalDay(void* context) {
        return static_cast<WaveshareEsp32s3ePaper3inch97*>(context)->local_day_.load();
    }
    // Pet worker: the local minute for V8 care needs (0 = clock unknown).
    static uint32_t LocalMinute(void* context) {
        return static_cast<WaveshareEsp32s3ePaper3inch97*>(context)->local_minute_.load();
    }
    // Pet worker, after a V8 care commit: copy and return. The home look is
    // recomputed by pet_boot_diag; a new sticker is announced by pet_input
    // once the care picture is gone.
    static void CareCommitted(const kid_app::CareEvent& event, kid_app::CommitResult result,
                              uint32_t new_stickers, void* context) {
        auto* self = static_cast<WaveshareEsp32s3ePaper3inch97*>(context);
        if (!self) return;
        if (result == kid_app::CommitResult::Committed) self->rhythm_refresh_.store(true);
        if (event.kind == kid_app::CareEventKind::HabitDone)  // the session waits for it
            self->habit_answer_.store(result == kid_app::CommitResult::Committed ? 1 : 2);
        if (new_stickers && event.kind != kid_app::CareEventKind::FestivalSeen)
            self->sticker_news_.store(true);
        if ((event.kind == kid_app::CareEventKind::HabitSet ||
             event.kind == kid_app::CareEventKind::BirthdaySet) &&
            result != kid_app::CommitResult::Committed && result != kid_app::CommitResult::Duplicate)
            self->parent_care_failed_.store(true);
        ESP_LOGI(TAG, "PET_CARE_RESULT kind=%u result=%d new_stickers=0x%x",
                 static_cast<unsigned>(event.kind), static_cast<int>(result),
                 static_cast<unsigned>(new_stickers));
    }

    bool StartPetLedger(kid_app::PetSnapshot* restored = nullptr) {
        std::lock_guard<std::mutex> lock(pet_ledger_mutex_);
        if (!pet_ledger_ready_) {
            pet_service_.SetDayProvider(&LocalDay, this);
            pet_service_.SetMinuteProvider(&LocalMinute, this);  // V8 care needs
            pet_service_.SetCareCallback(&CareCommitted, this);
            server_sync_.Attach();  // command outcomes -> network task
            pet_ledger_ready_ = pet_service_.Start(
                kid_app::FreertosAtomicPetService::StartMode::MigrateLegacyLocal,
                "tuantuan-board", &PetCommitted, nullptr, this, &pet_ledger_restored_);
            const auto migration = pet_service_.Migration();
            const auto& s = pet_ledger_restored_;
            const auto today = pet_service_.TodayNow();  // small copy: 4 KiB boot stack
            ESP_LOGI(TAG, "PET_LEDGER_READY ok=%d migration=%s failure=%s flags=0x%02x "
                     "gen=%llu fed=%u mode=%d mood=%d today=%u feeds=%u temperament=%u",
                     pet_ledger_ready_ ? 1 : 0,
                     kid_app::PetLedgerMigration::OutcomeName(migration.outcome),
                     kid_app::PetLedgerMigration::FailureName(migration.failure),
                     static_cast<unsigned>(migration.flags),
                     static_cast<unsigned long long>(pet_service_.GenerationNow()),
                     static_cast<unsigned>(s.interaction_count), static_cast<int>(s.mode),
                     static_cast<int>(s.mood), static_cast<unsigned>(today.day),
                     static_cast<unsigned>(today.feeds),
                     static_cast<unsigned>(pet_service_.TemperamentNow()));
            if (pet_ledger_ready_) {
                // V3-06 settings now live in the ledger (schema v3).
                ApplyParentSettings(pet_service_.ParentNow(), "ledger");
                parent_ready_.store(true);
                StartServerSync();
            }
        }
        if (pet_ledger_ready_ && restored) *restored = pet_ledger_restored_;
        return pet_ledger_ready_;
    }

    // Once, with the ledger. Hooks only read atomics (Wi-Fi flag, RTC day,
    // last PMIC reading): the network task never touches I2C or the ledger.
    void StartServerSync() {
        kid_app::ServerSyncHooks hooks{};
        hooks.online = [](void*) { return WifiManager::GetInstance().IsConnected(); };
        hooks.local_day = &LocalDay;
        hooks.battery = [](void* context, uint8_t* pct, bool* charging) {
            auto* self = static_cast<WaveshareEsp32s3ePaper3inch97*>(context);
            *pct = self->sync_battery_pct_.load();
            *charging = self->sync_charging_.load();
        };
        hooks.context = this;
        const esp_app_desc_t* app = esp_app_get_description();
        const bool started = server_sync_.Start(hooks, app ? app->version : "unknown");
        ESP_LOGI(TAG, "PET_SYNC_START ok=%d", started ? 1 : 0);
    }

    // UI-only way out of the reader when "home" cannot be saved (pet worker,
    // dropped StopStory) or posted (pet_toggle, full inbox). The durable
    // state still says Story; the next committed action (e.g. a feed)
    // brings it in line, and a reboot without a book goes home again.
    void EscapeStoryHome(const char* reason) {
        if (story_pcm_.Ready()) {
            StoryAudioCancel(this);
            story_pcm_.Stop();
        }
        SceneStateLock lock(scene_state_mutex_);
        auto scene = kid_app::PetScenePresenter::Present(kid_app::PetSnapshot{});
        scene.home_selection = home_selection_.load();
        last_pet_scene_.store(PackPetScene(scene));
        if (!sleep_active_.load()) pet_mailbox_.Publish(WithVoiceFace(scene));
        // pet_input closes the book (UpdateBook) once the mode is not Story.
        pet_event_mode_.store(kid_app::PetMode::Idle);
        ESP_LOGW(TAG, "PET_STORY_ESCAPE reason=%s", reason);
    }

    // V5: structured memory from trusted on-device rules only (a like from a
    // dial choice question, a finished book or game); committed with the pet
    // state in the ledger, day from the RTC. Never from voice. Any task.
    bool RecordMemory(const kid_app::MemoryEvent& event) {
        const bool posted = pet_service_.PostMemory(event);
        if (!posted)
            ESP_LOGW(TAG, "PET_MEMORY_REJECTED kind=%u", static_cast<unsigned>(event.kind));
        return posted;
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
            if (!StartPetLedger(&restored)) return false;
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
            book_missing_ = story_sd_.Missing();
            if (story_sd_.Ready() &&
                !kid_app::BuildBookOutline(story_sd_.Reader()->Content(), &book_outline_)) {
                ESP_LOGW(TAG, "PET_BOOK_OUTLINE_FAILED");
                book_missing_ = kid_app::BookMissing::BadBook;
            }
            // V8 series: the "next book" arrow only shows when it is on the card.
            book_outline_.next_book_present =
                book_outline_.series_next != 0 &&
                story_sd_.ContentBookPresent(book_outline_.series_next);
            book_outline_ready_.store(true);
            // Restored in Story without a readable book (no card, empty or
            // broken pack): go home instead of opening an empty reader. If
            // even that cannot be saved, PetCommitted escapes to home.
            if (kid_app::RestoredStoryNeedsHome(pet_event_mode_.load(),
                                                story_sd_.Ready() && book_outline_.pages > 0)) {
                ESP_LOGI(TAG, "PET_STORY_RESTORE_HOME reason=no_book");
                if (!pet_service_.PostLocal(kid_app::PetAction::StopStory))
                    EscapeStoryHome("no_book");
            }
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
    // Button_Up and GPIO6 Button_Down. The 2026-11-02 V1 trial mapped them
    // the other way round; on 2026-11-07 the user asked to swap up/down
    // back, so up = GPIO4 and down = GPIO6 again (as the schematic says).
    // Centre press = GPIO5 (Button_Function).
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
            HandleParentInput(event.kind);
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
        // V4-06: a running mini game takes the dial (long press = home).
        if (GameActive()) {
            HandleGameInput(event.kind);
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
            // V8 bedtime ritual from the rail in the evening: tidy up, diary,
            // then the same sleep flow. PWR keeps its direct sleep (unchanged).
            if (BedtimeRitualAllowed()) StartCare(kid_app::CareKind::Tidy);
            else HandleSleepAction(sleep_flow_.Request(NowMs()));
            return;
        }
        if (game_requested_) {
            game_requested_ = false;
            StartGame();
            return;
        }
        if (care_requested_) {
            care_requested_ = false;
            OpenCareMenu(0);
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
        // V8: Feed opens the care menu (food, bath, brush, tickle, clothes,
        // habit); refused like a game, where it stays the old direct feed.
        if (home_menu_.Selected() == kid_app::HomeItem::Feed && GameStartAllowed()) {
            care_requested_ = true;  // started after the lock is released
            return {};
        }
        // V4-06: a mini game (UI only); refused in quiet hours or on a low
        // battery, where the pet says hi instead (PetInputRouter).
        if (home_menu_.Selected() == kid_app::HomeItem::Game && GameStartAllowed()) {
            game_requested_ = true;  // started after the lock is released
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
                EndGame("sleep", false);  // V4-06: a game too
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
        // One last state snapshot for the parent panel (bounded; at once when
        // there is no link): the commit above is what it will carry.
        server_sync_.FlushBeforePowerOff(6000);
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
        // ledger_gen: the last durable commit; the next boot's PET_LEDGER_READY
        // must restore exactly this generation (self-test ledger_restored).
        ESP_LOGI(TAG, "PET_SLEEP_POWEROFF settled=%d alarm_s=%u armed=%d wake=%s battery=%d "
                 "ledger_gen=%llu",
                 pet_service_.Settled() ? 1 : 0, static_cast<unsigned>(wake_s), alarm ? 1 : 0,
                 wake_by, battery ? 1 : 0,
                 static_cast<unsigned long long>(pet_service_.GenerationNow()));
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

    // ---- V6-02 fixed voice commands ----
    // The server's fixed-command matcher (S5) asks for one of a handful of
    // wire names (kid_app/voice_command.h) through the MCP tool
    // self.pet.request (InitializePetTools); any task may post one, pet_input
    // decides with the same rules as the buttons (DecideVoiceCommand) and
    // runs the same code paths as the rail / PWR / dial. Voice writes no
    // memory and gives no extra reward. One slot: a newer command replaces a
    // waiting one. Bit 0-7 the command, bits 8-23 a sequence (never 0 when
    // posted).
    std::atomic<uint32_t> voice_cmd_{0};
    std::atomic<uint16_t> voice_cmd_seq_{0};
    // pet_input's answer for a waiting self.pet.request call: bits 8-23 the
    // sequence, bits 0-7 kid_app::VoiceResultCode.
    std::atomic<uint32_t> voice_reply_{0};
    // pet_input only: a screen activity (game menu, songs, routines, food
    // picker, brushing) asked for while XiaoZhi talks. UpdateGame ends any
    // game while XiaoZhi is not idle, so it is decided again once it is (the
    // guard ends the chat after its sentence); dropped after kVoiceDeferMs.
    kid_app::VoiceCommand voice_deferred_ = kid_app::VoiceCommand::Unknown;
    uint32_t voice_deferred_at_ms_ = 0;
    static constexpr uint32_t kVoiceDeferMs = 20000;
    // The MCP tool runs on XiaoZhi's main task: wait for pet_input briefly
    // (its loop turns every ~20 ms), then answer "pending".
    static constexpr uint32_t kVoiceReplyWaitMs = 400;

    // Any task. Unknown names are logged and dropped right here. Returns the
    // sequence pet_input answers under (0: dropped).
    uint16_t RequestVoiceCommand(const char* name) {
        const auto command = kid_app::ParseVoiceCommand(name);
        if (command == kid_app::VoiceCommand::Unknown) {
            ESP_LOGI(TAG, "PET_VOICE_CMD name=%.15s result=unknown", name ? name : "");
            return 0;
        }
        uint16_t seq = static_cast<uint16_t>(voice_cmd_seq_.fetch_add(1) + 1);
        if (!seq) seq = 1;
        voice_cmd_.store(static_cast<uint32_t>(seq) << 8 | static_cast<uint32_t>(command));
        return seq;
    }

    // MCP tool (XiaoZhi's main task): pet_input's result for `seq`.
    const char* WaitVoiceResult(uint16_t seq) {
        for (uint32_t waited = 0; waited <= kVoiceReplyWaitMs; waited += 10) {
            const uint32_t reply = voice_reply_.load();
            if ((reply >> 8) == seq) return kid_app::VoiceResultName(reply & 0xFF);
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        return "pending";
    }

    // S5 -> device: the child guard on the XiaoZhi server calls this on a
    // local_command (docs/V6_VOICE_COMMANDS.md). Only the server's fixed
    // names pass; pet_input decides like for the buttons, so MCP carries a
    // proposal only (ADR-008). The reply is {"action":..,"result":..}.
    void InitializePetTools() {
        McpServer::GetInstance().AddTool("self.pet.request",
            "Fixed child voice command chosen by the server's command matcher; "
            "the pet decides whether to act. Not for open chat.",
            PropertyList({Property("action", kPropertyTypeString).SetMaxLength(15)}),
            [this](const PropertyList& properties) -> ToolResult {
                const std::string action = properties["action"].value<std::string>();
                if (!kid_app::IsVoiceRequestAction(action.c_str())) {
                    ESP_LOGW(TAG, "PET_VOICE_MCP result=refused len=%u",
                             static_cast<unsigned>(action.size()));
                    return std::unexpected(std::string("unknown action"));
                }
                const uint16_t seq = RequestVoiceCommand(action.c_str());
                const char* result = seq ? WaitVoiceResult(seq) : "unknown";
                ESP_LOGI(TAG, "PET_VOICE_MCP name=%s result=%s", action.c_str(), result);
                char reply[64];
                snprintf(reply, sizeof(reply), "{\"action\":\"%s\",\"result\":\"%s\"}",
                         action.c_str(), result);
                return ReturnValue{std::string(reply)};
            });
    }

    kid_app::VoiceScene CurrentVoiceScene() const {
        if (!pet_mode_.load()) return kid_app::VoiceScene::Off;
        if (ParentMenuOpen()) return kid_app::VoiceScene::Parent;
        if (sleep_flow_.Active() || sleep_active_.load()) return kid_app::VoiceScene::Sleeping;
        if (GameActive()) return kid_app::VoiceScene::Game;
        if (pet_event_mode_.load() == kid_app::PetMode::Story)
            return kid_app::VoiceScene::Reader;
        return kid_app::VoiceScene::Home;
    }

    // pet_input, every loop.
    void UpdateVoiceCommand(DeviceState state) {
        uint32_t word = voice_cmd_.exchange(0);
        bool later = false;  // a deferred command, now that XiaoZhi is idle
        if (word) {
            voice_deferred_ = kid_app::VoiceCommand::Unknown;  // the newer one wins
        } else if (voice_deferred_ != kid_app::VoiceCommand::Unknown) {
            if (NowMs() - voice_deferred_at_ms_ > kVoiceDeferMs) {
                ESP_LOGI(TAG, "PET_VOICE_CMD name=%s result=expired",
                         kid_app::VoiceCommandName(voice_deferred_));
                voice_deferred_ = kid_app::VoiceCommand::Unknown;
                return;
            }
            if (state != kDeviceStateIdle) return;
            word = static_cast<uint32_t>(voice_deferred_);  // sequence 0: nobody waits
            voice_deferred_ = kid_app::VoiceCommand::Unknown;
            later = true;
        }
        if (!word) return;
        const auto command = static_cast<kid_app::VoiceCommand>(word & 0xFF);
        const uint16_t seq = static_cast<uint16_t>(word >> 8);
        // A voice command is activity: it ends a nap or the morning window
        // like a dial touch (the command itself still runs).
        if (power_save_timer_) power_save_timer_->WakeUp();
        if (pet_mode_.load()) HandlePowerEvent(power_.OnActivity(NowMs()));

        static const char* const kScene[] = {"off", "home", "reader", "game", "parent", "sleeping"};
        const auto status = kid_app::UnpackStatus(status_bits_.load());
        kid_app::VoiceContext context;
        context.scene = CurrentVoiceScene();
        context.quiet_hours = quiet_hours_.load();
        context.low_battery = battery_.LowPower();
        context.clock_known = status.hour != kid_app::StatusBar::kNoTime &&
                              status.minute != kid_app::StatusBar::kNoTime;
        context.battery_known = battery_.Percent() >= 0;
        context.chat_active = state != kDeviceStateIdle;
        auto decision = kid_app::DecideVoiceCommand(command, context);
        const char* result = decision.result;
        if (decision.after_chat) {  // runs once XiaoZhi is idle, decided again then
            voice_deferred_ = command;
            voice_deferred_at_ms_ = NowMs();
            decision.proposal = kid_app::VoiceProposal::None;
        }

        switch (decision.action) {
            case kid_app::VoiceAction::None:
                break;
            case kid_app::VoiceAction::BeginSleep:
                // The same flow as PWR / the rail: sleeping picture, 3 s to cancel.
                HandleSleepAction(sleep_flow_.Request(NowMs()));
                break;
            case kid_app::VoiceAction::OpenReader:
                // The rail's Story item: a book, or the no-book picture (#75).
                if (!pet_service_.PostLocal(kid_app::PetAction::StartStory)) result = "rejected";
                break;
            case kid_app::VoiceAction::PageNext:
                if (!book_.Active() || book_.Asking()) {
                    result = "busy";  // still opening / answer the question first
                } else if (book_.Missing() != kid_app::BookMissing::None) {
                    result = "no_book";
                } else {
                    ApplyBookStep(book_.OnInput(kid_app::InputKind::DialDown, NowMs()), "voice");
                }
                break;
            case kid_app::VoiceAction::PagePrev:  // the dial's "up" in the reader
                if (!book_.Active() || book_.Asking()) {
                    result = "busy";
                } else if (book_.Missing() != kid_app::BookMissing::None) {
                    result = "no_book";
                } else {
                    ApplyBookStep(book_.OnInput(kid_app::InputKind::DialUp, NowMs()), "voice");
                }
                break;
            case kid_app::VoiceAction::GoHome:  // the dial's long press
                if (context.scene == kid_app::VoiceScene::Game) {
                    HandleGameInput(kid_app::InputKind::DialLong);
                } else if (book_.Active()) {
                    ApplyBookStep(book_.OnInput(kid_app::InputKind::DialLong, NowMs()), "voice");
                } else if (!pet_service_.PostLocal(kid_app::PetAction::StopStory)) {
                    result = "rejected";
                }
                break;
            case kid_app::VoiceAction::ShowTime:
                ESP_LOGI(TAG, "PET_VOICE_TIME hour=%u minute=%u", static_cast<unsigned>(status.hour),
                         static_cast<unsigned>(status.minute));
                break;
            case kid_app::VoiceAction::ShowBattery:
                ESP_LOGI(TAG, "PET_VOICE_BATTERY pct=%d charging=%d state=%s", battery_.Percent(),
                         battery_.External() ? 1 : 0, kid_app::BatteryStateName(battery_.State()));
                break;
        }
        // V8: proposals only; the activity decides for itself and the dial
        // confirms whatever needs confirming (routines wait for a press, the
        // food picker for the child's pick). A deferred one runs later.
        switch (decision.proposal) {
            case kid_app::VoiceProposal::None:
                break;
            case kid_app::VoiceProposal::CareFeed:  // the care menu's Feed card
                StartCare(kid_app::CareKind::Feed);
                break;
            case kid_app::VoiceProposal::Groom:  // the care menu's Brush card
                StartCare(kid_app::CareKind::Brush);
                break;
            case kid_app::VoiceProposal::OpenGame:
                OpenGameMenu(0);
                break;
            case kid_app::VoiceProposal::PlaySong:
                StartSound(SoundKind::Songs);  // quiet hours: lullabies only, quietly
                break;
            case kid_app::VoiceProposal::StartBrush:
                StartSound(SoundKind::Brush);
                break;
            case kid_app::VoiceProposal::StartWash:
                StartSound(SoundKind::Wash);
                break;
            case kid_app::VoiceProposal::AskNeeds:  // wired with the care state (V8-01)
            case kid_app::VoiceProposal::TellJoke:  // the server's TTS speaks it
                ESP_LOGI(TAG, "PET_VOICE_PROPOSAL name=%s generic=%d",
                         kid_app::VoiceProposalName(decision.proposal), decision.generic ? 1 : 0);
                break;
        }
        ESP_LOGI(TAG, "PET_VOICE_CMD name=%s result=%s action=%s hint=%s scene=%s proposal=%s "
                 "later=%d",
                 kid_app::VoiceCommandName(command), result,
                 kid_app::VoiceActionName(decision.action),
                 kid_app::VoiceHintName(decision.hint), kScene[static_cast<int>(context.scene)],
                 kid_app::VoiceProposalName(decision.proposal), later ? 1 : 0);
        if (seq)
            voice_reply_.store(static_cast<uint32_t>(seq) << 8 |
                               kid_app::VoiceResultCode(result));

        // Time / battery answers and refusals: a wave and the pet's hello
        // (the server's TTS says the digits; the status band already shows
        // them). Quiet hours: picture only, no sound.
        const bool answered = decision.action == kid_app::VoiceAction::ShowTime ||
                              decision.action == kid_app::VoiceAction::ShowBattery;
        const bool refused = decision.hint != kid_app::VoiceHint::None;
        if ((answered || refused) && context.scene == kid_app::VoiceScene::Home) {
            if (!decision.silent && decision.hint != kid_app::VoiceHint::QuietHours)
                PostPhrase(kid_app::PhraseContext::Greeting);
            RequestAnimation(kid_app::Animation::Greet, UnpackPetScene(last_pet_scene_.load()));
        } else if (refused && !decision.silent && decision.hint != kid_app::VoiceHint::QuietHours) {
            PostPhrase(kid_app::PhraseContext::Greeting);
        }
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
                // V8 parrot: holding the dial centre records; InputHub never sees
                // that key meanwhile (no 1 s long press, no parent combo).
                if (key == kid_app::InputKey::DialPress && parrot_holding_.load()) {
                    HandleParrotEdge(edge.down);
                    continue;
                }
                if (edge.down) input_hub_.Press(key, edge.at_ms);
                else input_hub_.Release(key, edge.at_ms);
            }
            input_hub_.Tick(NowMs());
            kid_app::InputEvent event{};
            while (input_hub_.Next(&event)) HandleInputEvent(event);
            UpdateParentMenu();
            UpdateBook();
            UpdateGame(state);
            UpdatePower(state);
            UpdateBattery(state);
            UpdateEnvTips(state);  // V4-03
            UpdateVoiceCommand(state);  // V6-02
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
                !GameActive() &&  // V4-06
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
    kid_app::BookMissing book_missing_ = kid_app::BookMissing::NoCard;  // with it
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
        if (packed) return UnpackPetScene(packed);
        // Already known to be empty: the first reader frame is the "no
        // book" picture, not the placeholder followed by a second refresh.
        if (book_outline_ready_.load() && book_outline_.pages == 0)
            return kid_app::BookMissingScene(book_missing_);
        return kid_app::BookScene({}, false);
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
            // V8 bedtime story: entering the story in the evening day phase
            // (RTC, offline) starts the quiet mode: low-contrast pages that
            // turn by themselves, no questions, then the normal sleep flow.
            // Any dial turn drops back to normal reading (BookStep.bedtime_off).
            const int phase = day_phase_.load();
            const bool bedtime =
                phase >= 0 && kid_app::BedtimeAvailable(static_cast<kid_app::DayPhase>(phase));
            book_.Open(&book_outline_, now, book_missing_, bedtime);
            book_shown_.store(0);
            ESP_LOGI(TAG,
                     "PET_BOOK_OPEN book=%s pages=%u grey=%u questions=%u kpb2=%d branching=%d "
                     "series=%u:%u/%u bedtime=%d",
                     story_book_, static_cast<unsigned>(book_outline_.pages),
                     static_cast<unsigned>(book_outline_.GreyPages()),
                     static_cast<unsigned>(book_outline_.QuestionTotal()),
                     book_outline_.kpb2 ? 1 : 0, book_outline_.branching ? 1 : 0,
                     static_cast<unsigned>(book_outline_.series_id),
                     static_cast<unsigned>(book_outline_.series_index),
                     static_cast<unsigned>(book_outline_.series_count),
                     book_.Bedtime() ? 1 : 0);
            // No book: the picture says why (card / book), a press or long
            // press goes home; nothing is read aloud or remembered.
            if (book_.Missing() != kid_app::BookMissing::None)
                ESP_LOGW(TAG, "PET_BOOK_MISSING reason=%s book=%s",
                         kid_app::BookMissingName(book_.Missing()), story_book_);
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
        if (step.bedtime_off) ESP_LOGI(TAG, "PET_BOOK_BEDTIME off=dial");
        if (book_.NextBookId() != 0 && step.publish)
            ESP_LOGI(TAG, "PET_BOOK_NEXT content=%u", static_cast<unsigned>(book_.NextBookId()));
        if (step.sleep) {
            // The bedtime story is over and TuanTuan yawned: the same flow as
            // PWR / the rail (sleeping picture, 3 s to cancel).
            ESP_LOGI(TAG, "PET_BOOK_BEDTIME sleep=1");
            HandleSleepAction(sleep_flow_.Request(NowMs()));
        }
        if (step.home) {
            ESP_LOGI(TAG, "PET_BOOK_HOME");
            if (!pet_service_.PostLocal(kid_app::PetAction::StopStory)) {
                ESP_LOGW(TAG, "PET_INPUT_REJECTED");
                EscapeStoryHome("rejected");  // a long press always gets home
            }
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
    // (the kid_app choice guard), then goes to the ledger like every other
    // memory (RecordMemory). Id and day go back to 0: the atomic Pet worker
    // assigns the next local id (shared replay window) and the RTC day.
    void RecordBookMemory(const kid_app::BookMemoryRequest& request) {
        kid_app::MemoryEvent event{};
        if (!kid_app::BookMemoryEvent(book_outline_, request, kid_app::kLocalEventBit, 0,
                                      &event)) {
            ESP_LOGW(TAG, "PET_BOOK_MEMORY_REJECTED kind=%u", static_cast<unsigned>(request.kind));
            return;
        }
        event.id = 0;
        const bool posted = RecordMemory(event);
        if (event.kind == kid_app::MemoryEventKind::PreferenceChosen)
            ESP_LOGI(TAG, "PET_BOOK_MEMORY kind=like like=%u:%u posted=%d",
                     static_cast<unsigned>(event.slot), static_cast<unsigned>(event.option),
                     posted ? 1 : 0);
        else
            ESP_LOGI(TAG, "PET_BOOK_MEMORY kind=finished book=%u posted=%d",
                     static_cast<unsigned>(event.option), posted ? 1 : 0);
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
    bool ShowBookGreyLocked(kid_app::ContentPackReader& content, uint16_t page, bool bedtime) {
        const size_t grey_bytes = kid_app::ContentPackReader::kGreyBytes;
        const size_t plane = kid_app::ContentPackReader::kMonoBytes;
        auto* grey = static_cast<uint8_t*>(
            heap_caps_malloc(grey_bytes + 2 * plane, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!grey) return false;
        uint8_t* ram24 = grey + grey_bytes;
        uint8_t* ram26 = ram24 + plane;
        const bool read = content.ReadGrey(page, grey, grey_bytes);
        if (read && bedtime) kid_app::BedtimeTone(grey, grey_bytes);  // V8 low contrast
        const bool okay = read &&
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
                okay = ShowBookGreyLocked(content, view.page, view.bedtime);
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
    kid_app::MemoryPhraseSelector memory_phrases_;  // V5-04: yesterday's book, a chosen like
    // Set by the pickup gesture just before its Pickup action; the Greeting
    // line that follows may then mention a like (not the rail's Chat item).
    std::atomic<bool> pickup_gesture_{false};
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
        LoadGamePack();  // V4-06: same card, same task
    }

    // Any task. Never asleep (good night only while falling asleep) or
    // napping; the player re-checks voice / Pet mode before and while it
    // plays. Overwrites a waiting option prompt, like a newer prompt would.
    // V8: `care` (count ids, care_phrases.h) are read after the line.
    void PostPhrase(kid_app::PhraseContext context, const uint16_t* care = nullptr,
                    size_t count = 0) {
        if (!pet_mode_.load() || napping_.load() || ParentMenuOpen() || GameActive() ||
            (sleep_active_.load() && context != kid_app::PhraseContext::Bedtime))
            return;
        kid_app::PromptRequest request{};
        request.phrase = static_cast<uint8_t>(1 + static_cast<uint8_t>(context));
        request.token = phrase_tokens_.fetch_add(1) + 1;
        for (size_t i = 0; care && i < count && i < kid_app::PromptRequest::kCareLines; ++i)
            request.care[request.care_count++] = care[i];
        PostPrompt(request);
    }

    // V8 reviewed care lines, in order (a session reaction, the diary). Like
    // PostPhrase, but also inside the care frame of the game.
    void PostCareLines(const uint16_t* ids, size_t count) {
        if (!ids || !count || !pet_mode_.load() || napping_.load() || ParentMenuOpen() ||
            sleep_active_.load())
            return;
        kid_app::PromptRequest request{};
        request.token = care_tokens_.fetch_add(1) + 1;
        for (size_t i = 0; i < count && i < kid_app::PromptRequest::kCareLines; ++i)
            if (ids[i]) request.care[request.care_count++] = ids[i];
        if (request.care_count) PostPrompt(request);
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
            const bool game = request.game != 0;  // V4-06 mini-game clip
            // V8: a request with only care lines plays care[0] now.
            const bool care = !phrase && !game && request.care_count != 0;
            const auto context =
                static_cast<kid_app::PhraseContext>(phrase ? request.phrase - 1 : 0);
            const bool bedtime = phrase && context == kid_app::PhraseContext::Bedtime;
            // V5-01: the parent-set temperament from the ledger's memory
            // (gentle until a parent sets one).
            if (phrase) phrase_planner_.SetTemperament(pet_service_.TemperamentNow());
            // Consumed even if the line is dropped below: one gesture, one chance.
            const bool pickup_line =
                phrase && context == kid_app::PhraseContext::Greeting &&
                pickup_gesture_.exchange(false);
            uint16_t phrase_id = phrase ? phrase_planner_.Next(context, NowMs()) : 0;
            if (phrase && !phrase_id) {
                ESP_LOGI(TAG, "PET_PHRASE_SKIPPED ctx=%s reason=gap",
                         kid_app::PhraseContextName(context));
                if (request.care_count && !uxQueueMessagesWaiting(prompt_queue_)) {
                    kid_app::PromptRequest rest{};  // V8: the care lines still play
                    rest.token = request.token;
                    rest.care_count = request.care_count;
                    rest.care = request.care;
                    xQueueSend(prompt_queue_, &rest, 0);
                }
                continue;
            }
            // V5-04: a memory line (yesterday's book at good morning, a chosen
            // like on a pickup gesture) when the ledger really has one; the
            // plain line above is the fallback. The pack holds no clips for
            // slot lines yet, so they only log (src=text); a recorded clip
            // would replace the plain line.
            if (phrase && (context == kid_app::PhraseContext::MorningRecap || pickup_line)) {
                const auto kind = context == kid_app::PhraseContext::MorningRecap
                                      ? kid_app::MemoryPhraseKind::MorningRecap
                                      : kid_app::MemoryPhraseKind::PickupLike;
                const auto memory = pet_service_.MemoryNow();
                const auto choice = memory_phrases_.Select(kind, memory, local_day_.load(),
                                                           quiet_hours_.load(), NowMs());
                if (choice.use) {
                    kid_app::SoundClip memory_clip;
                    const bool recorded = sound_bank_.LoadPhrase(choice.phrase_id, &memory_clip) &&
                                          memory_clip.source == kid_app::SoundClip::Source::Pack;
                    if (recorded) phrase_id = choice.phrase_id;
                    ESP_LOGI(TAG, "PET_MEMORY_PHRASE ctx=%s id=%u src=%s slot=%u value=%u",
                             kid_app::MemoryPhraseKindName(kind),
                             static_cast<unsigned>(choice.phrase_id), recorded ? "pack" : "text",
                             static_cast<unsigned>(choice.slot), static_cast<unsigned>(choice.value));
                } else {
                    ESP_LOGI(TAG, "PET_MEMORY_DECISION ctx=%s used=0 reason=%s",
                             kid_app::MemoryPhraseKindName(kind),
                             kid_app::MemoryPhraseReasonName(choice.reason));
                }
            }
            auto& audio = Application::GetInstance().GetAudioService();
            // Newest request wins: invalidate audio queued for an older prompt
            // (not the tail of the line a V8 continuation follows).
            if (!request.care_next) audio.CancelLocalStoryPcm();
            if (request.index == kPromptStop) continue;
            // Logs: PET_PROMPT_* index=<rail item>, PET_PHRASE_* id=<phrase id>.
            const char* kind = phrase ? "PHRASE" : "PROMPT";
            const char* key = phrase ? "id" : "index";
            unsigned what = phrase ? phrase_id : request.index;
            if (game) {  // V4-06: PET_GAME_* clip=<mini-game clip id>
                kind = "GAME";
                key = "clip";
                what = request.game;
            }
            if (care) {  // V8: PET_CARE_* id=<care line id>
                kind = "CARE";
                key = "id";
                what = request.care[0];
            }
            kid_app::SoundClip clip;
            kid_app::GameClip game_clip;
            if (game     ? !kid_app::LoadGameClip(GamePackNow(), request.game, &game_clip)
                : care   ? !sound_bank_.LoadCareLine(request.care[0], &clip)
                : phrase ? !sound_bank_.LoadPhrase(phrase_id, &clip)
                         : !sound_bank_.LoadOption(request.index, &clip)) {
                ESP_LOGW(TAG, "PET_%s_FAILED %s=%u reason=clip", kind, key, what);
                continue;
            }
            if (game) clip.samples = game_clip.samples;  // streamed by game_clip below
            const uint64_t session =
                (uint64_t{game ? 0x47414D45u : care ? 0x43415245u : phrase ? 0x50485253u
                                                                          : 0x50524F4Du} << 32) |
                request.token;
            if (game)
                ESP_LOGI(TAG, "PET_GAME_PLAY clip=%u src=%s token=%u samples=%u", what,
                         kid_app::GameClipSourceName(game_clip.source),
                         static_cast<unsigned>(request.token),
                         static_cast<unsigned>(clip.samples));
            else if (phrase)
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
                const size_t n = game ? game_clip.Copy(offset, chunk, 480)
                                      : clip.Copy(offset, chunk, 480);
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
            // V8: the next care line follows, unless something newer is waiting.
            const bool done = !state_lost && !interrupted && offset >= clip.samples;
            const uint8_t used = care ? 1 : 0;
            if (done && request.care_count > used && !uxQueueMessagesWaiting(prompt_queue_)) {
                kid_app::PromptRequest next{};
                next.token = request.token;
                next.care_next = true;
                for (uint8_t i = used; i < request.care_count; ++i)
                    next.care[next.care_count++] = request.care[i];
                xQueueSend(prompt_queue_, &next, 0);  // full = a newer request won
            }
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
        InitializePetTools();  // S5 voice commands reach pet_input from here on
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
        EndGame("morning", false);  // V4-06
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
        // V5-04 good morning, then the V8 lines (room, festival, habit).
        uint16_t morning[kid_app::kMaxDiaryLines] = {};
        const size_t lines = MorningCareLines(morning);
        PostPhrase(kid_app::PhraseContext::MorningRecap, morning, lines);
        ESP_LOGI(TAG, "PET_MORNING_BEGIN awake_s=%u phase=%s",
                 static_cast<unsigned>(
                     kid_app::SelfTestClock::Scaled(kid_app::PowerManager::kMorningAwakeMs) / 1000),
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
        if (GameActive()) conditions.busy = true;  // V4-06: so does a game in progress
        conditions.may_power_off = on_battery_.load() && !selftest_seen_.load() &&
                                   !battery_.External();  // V3-05: never while charging
        conditions.low_battery = battery_.LowPower();
        const int phase = day_phase_.load();
        conditions.phase_known = phase >= 0;
        if (conditions.phase_known) conditions.phase = static_cast<kid_app::DayPhase>(phase);
        HandlePowerEvent(power_.Tick(now, conditions));
    }

    // Plain power-on (no trial script, no alarm): open Pet mode as soon as
    // XiaoZhi is idle, so the child sees the pet, not the XiaoZhi screen.
    // BOOT long press still switches back and forth. Runs on pet_selftest.
    void EnterPetAtBoot() {
        for (int attempt = 0; attempt < kMorningEnterTries && !pet_mode_.load(); ++attempt) {
            RequestPetModeToggle();
            for (int poll = 0; poll < 20 && !pet_mode_.load(); ++poll)
                vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (pet_mode_.load()) {
            ESP_LOGI(TAG, "PET_BOOT_ENTER result=pet");
        } else {
            ESP_LOGW(TAG, "PET_BOOT_ENTER result=timeout state=%d",
                     static_cast<int>(Application::GetInstance().GetDeviceState()));
        }
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
        // A self-test time scale shortens the debounce (counted in samples).
        if (battery_sample_ms_ &&
            now - battery_sample_ms_ < kid_app::SelfTestClock::Scaled(kBatterySampleMs))
            return;
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
            pet &&
            now - battery_pet_since_ms_ >= kid_app::SelfTestClock::Scaled(kBatteryReactAfterPetMs) &&
            !sleep_flow_.Active() && !sleep_active_.load() && !napping_.load() &&
            !morning_shown_.load() && voice_face_.load() == 0 && state == kDeviceStateIdle &&
            kid_app::PetInputRouter::IsHomeRail(pet_event_mode_.load()) &&
            (!selftest_seen_.load() || battery_scripted_.load()) &&
            !ParentMenuOpen() && !GameActive();  // V3-06 / V4-06: not over the menu or a game
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

    // ---- V4-03 environment life tips ----
    // pet_boot_diag publishes the SHTC3 reading next to the status band each
    // heartbeat; pet_input owns the policy (a self-test "temp" step bypasses
    // the heartbeat). A tip is the drink / coat thought bubble plus the reviewed
    // Personality line (sound pack clip, else the chime): UI only, nothing
    // durable. Not in quiet hours (the parent plan). The status band shows
    // the raw sensor value; the policy subtracts a conservative board-heat
    // offset (EnvTipPolicy::kUsbHeatC10 / kBatteryHeatC10).
    kid_app::EnvTipPolicy env_tips_;           // pet_input only
    bool env_pet_ = false;                     // pet_input: Pet mode last loop
    uint32_t env_pet_since_ms_ = 0;            // pet_input: Pet mode opened
    bool env_held_logged_ = false;             // pet_input
    uint16_t env_seq_ = 0;                     // pet_boot_diag only
    std::atomic<uint32_t> env_sample_{0};      // seq << 17 | ok << 16 | int16 temp_c10
    static constexpr int kEnvNoInject = 0x7FFFFFFF;
    std::atomic<int> env_inject_c10_{kEnvNoInject};  // self-test reading
    std::atomic<bool> env_scripted_{false};          // a script set the temperature
    static constexpr uint32_t kEnvReactAfterPetMs = 10000;  // greet first

    // pet_input, every loop after UpdateBattery.
    void UpdateEnvTips(DeviceState state) {
        const uint32_t now = NowMs();
        const bool pet = pet_mode_.load();
        if (pet && !env_pet_) env_pet_since_ms_ = now;
        env_pet_ = pet;
        // The policy is time based: every loop it gets the newest reading
        // (the sensor's last one, republished each heartbeat; or the
        // self-test's, effective at once), so a hold is timed from when
        // the room really crossed, not from a 10 s publish tick.
        const uint32_t sample = env_sample_.load();
        const int injected = env_inject_c10_.load();
        const bool injecting = injected != kEnvNoInject;
        if (injecting || (sample >> 17) != 0) {  // 0: nothing published yet
            const bool ok = injecting || ((sample >> 16) & 1u) != 0;
            const int temp_c10 = injecting ? injected : static_cast<int16_t>(sample & 0xFFFFu);
            const auto before = env_tips_.Zone();
            // The board heats the sensor (more on USB power); a self-test
            // "temp" step states the room temperature itself.
            env_tips_.SetOffsetC10(injecting ? 0
                                   : battery_.External() ? kid_app::EnvTipPolicy::kUsbHeatC10
                                                         : kid_app::EnvTipPolicy::kBatteryHeatC10);
            const auto pending = env_tips_.Observe(ok, temp_c10, now);
            if (env_tips_.Zone() != before)
                ESP_LOGI(TAG, "PET_ENV zone=%s room_c10=%d src=%s",
                         kid_app::EnvZoneName(env_tips_.Zone()), env_tips_.RoomC10(),
                         injecting ? "selftest" : "shtc3");
            if (pending != kid_app::EnvTip::None)
                ESP_LOGI(TAG, "PET_ENV_TIP_PENDING kind=%s", kid_app::EnvTipName(pending));
        }
        // Only on the awake Pet home screen, like the battery reactions.
        const bool allowed =
            pet &&
            now - env_pet_since_ms_ >= kid_app::SelfTestClock::Scaled(kEnvReactAfterPetMs) &&
            !sleep_flow_.Active() && !sleep_active_.load() && !napping_.load() &&
            !morning_shown_.load() && voice_face_.load() == 0 && state == kDeviceStateIdle &&
            kid_app::PetInputRouter::IsHomeRail(pet_event_mode_.load()) &&
            (!selftest_seen_.load() || env_scripted_.load()) &&
            !ParentMenuOpen() && !GameActive();
        const bool quiet = quiet_hours_.load();
        const auto tip = env_tips_.Take(quiet, allowed, local_day_.load(), now);
        if (tip == kid_app::EnvTip::None) {
            // Say once why a due tip waits for the night to pass.
            const bool held = quiet && env_tips_.Pending() != kid_app::EnvTip::None;
            if (held && !env_held_logged_)
                ESP_LOGI(TAG, "PET_ENV_TIP_HELD reason=quiet kind=%s",
                         kid_app::EnvTipName(env_tips_.Pending()));
            env_held_logged_ = held;
            return;
        }
        env_held_logged_ = false;
        const bool hot = tip == kid_app::EnvTip::Hot;
        const bool acted = RequestHomeAnimation(hot ? kid_app::Animation::TipHot
                                                    : kid_app::Animation::TipCold);
        PostPhrase(hot ? kid_app::PhraseContext::HotTip : kid_app::PhraseContext::ColdTip);
        ESP_LOGI(TAG, "PET_ENV_TIP kind=%s acted=%d room_c10=%d today=%u", kid_app::EnvTipName(tip),
                 acted ? 1 : 0, env_tips_.RoomC10(), static_cast<unsigned>(env_tips_.TodayCount()));
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
    // Self-test "phase" step: the day phase (and quiet hours) a script forces
    // instead of the RTC's; kPhaseNone = the RTC decides.
    static constexpr int kPhaseNone = -2;
    std::atomic<int> phase_inject_{kPhaseNone};

    static const char* GestureName(kid_app::Gesture gesture) {
        switch (gesture) {
            case kid_app::Gesture::Pickup: return "pickup";
            case kid_app::Gesture::GentleShake: return "gentle";
            case kid_app::Gesture::HardShake: return "hard";
            case kid_app::Gesture::FaceDown: return "facedown";
            case kid_app::Gesture::FaceUp: return "faceup";
            case kid_app::Gesture::TiltLeft: return "tiltleft";
            case kid_app::Gesture::TiltRight: return "tiltright";
            case kid_app::Gesture::Level: return "level";
            case kid_app::Gesture::TiltUp: return "tiltup";
            case kid_app::Gesture::TiltDown: return "tiltdown";
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
        // V8: a bath, tickle or lullaby takes every gesture (the session
        // ignores hard shakes as rewards and never counts a resting board).
        if (care_motion_.load()) {
            care_gesture_.store(static_cast<uint8_t>(gesture));
            if (power_save_timer_) power_save_timer_->WakeUp();
            power_poke_.store(true);
            ESP_LOGI(TAG, "PET_GESTURE kind=%s src=%s acted=1 care=1", GestureName(gesture), source);
            return;
        }
        // V4-07: a running motion game takes tilt and face down/up (the
        // latest one; pet_input applies it). Everything else, shaking
        // included, is no part of the game: never a reward.
        if (motion_active_.load() &&
            (gesture == kid_app::Gesture::FaceDown || gesture == kid_app::Gesture::FaceUp ||
             gesture == kid_app::Gesture::TiltLeft || gesture == kid_app::Gesture::TiltRight ||
             gesture == kid_app::Gesture::Level ||
             gesture == kid_app::Gesture::TiltUp || gesture == kid_app::Gesture::TiltDown)) {  // V8 maze
            motion_gesture_.store(static_cast<uint8_t>(gesture));
            if (power_save_timer_) power_save_timer_->WakeUp();
            power_poke_.store(true);
            ESP_LOGI(TAG, "PET_GESTURE kind=%s src=%s acted=1 game=1", GestureName(gesture), source);
            return;
        }
        if (gesture == kid_app::Gesture::FaceDown) face_down_.store(true);
        // V8 shake dice / tree: a shake only rolls; the dial answer is the reward.
        if (play_shake_.load() && motion_enabled_.load() &&
            (gesture == kid_app::Gesture::GentleShake || gesture == kid_app::Gesture::HardShake)) {
            motion_gesture_.store(static_cast<uint8_t>(gesture));
            if (power_save_timer_) power_save_timer_->WakeUp();
            power_poke_.store(true);
            ESP_LOGI(TAG, "PET_GESTURE kind=%s src=%s acted=1 game=1", GestureName(gesture), source);
            return;
        }
        bool acted = false;
        // V3-06: parents can switch motion off; the parent menu ignores it.
        const bool motion_off = !motion_enabled_.load();
        if (pet_mode_.load() && !sleep_active_.load() && !motion_off && !ParentMenuOpen() &&
            !GameActive() &&  // V4-06: holding TuanTuan while playing is no gesture
            Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
            if (power_save_timer_) power_save_timer_->WakeUp();
            power_poke_.store(true);
            switch (gesture) {
                case kid_app::Gesture::Pickup:
                    pickup_gesture_.store(true);  // V5-04: the Greeting may mention a like
                    acted = kid_app::PetInputRouter::IsHomeRail(pet_event_mode_.load()) &&
                            pet_service_.PostLocal(kid_app::PetAction::Pickup);
                    if (!acted) pickup_gesture_.store(false);
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
                case kid_app::Gesture::TiltLeft:  // V4-07: only the ball game listens
                case kid_app::Gesture::TiltRight:
                case kid_app::Gesture::Level:
                case kid_app::Gesture::TiltUp:  // V8: only the maze listens
                case kid_app::Gesture::TiltDown:
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
        int phase = ok ? static_cast<int>(rhythm.PhaseAt(rtc)) : -1;
        bool quiet = ok && rhythm.QuietAt(rtc);
        if (const int forced = phase_inject_.load(); forced != kPhaseNone) {
            phase = forced;  // self-test: the script says what time of day it is
            quiet = forced == static_cast<int>(kid_app::DayPhase::Night);
        }
        // V5-02: today's memory counters roll over on the local civil day.
        local_day_.store(ok ? static_cast<uint32_t>(kid_app::EpochFromCivil(rtc) / 86400) : 0);
        local_minute_.store(kid_app::CareMinute(rtc, ok));  // V8, 0 = unknown
        UpdateHomeLook(rtc, ok, phase);
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
            live_hour_.store(rtc.hour);  // V8 clock game
            live_minute_.store(rtc.minute);
        } else {
            live_hour_.store(-1);
        }
        if (!env_read_once_ || now - env_read_ms_ >= kEnvReadMs) {
            env_read_once_ = true;
            env_read_ms_ = now;
            env_ok_ = ReadShtc3TempC10(&env_temp_c10_);
        }
        // V4-03: a self-test "temp" step stands in for the SHTC3.
        if (const int injected = env_inject_c10_.load(); injected != kEnvNoInject) {
            env_ok_ = true;
            env_temp_c10_ = injected;
        }
        if (++env_seq_ > 0x7FFF) env_seq_ = 1;  // 0: nothing published yet
        env_sample_.store((static_cast<uint32_t>(env_seq_) << 17) | (env_ok_ ? 1u << 16 : 0u) |
                          static_cast<uint16_t>(static_cast<int16_t>(env_temp_c10_)));
        in.temp_ok = env_ok_;
        in.temp_c10 = env_temp_c10_;
        live_temp_ok_.store(env_ok_);  // V8 weather game
        live_temp_c10_.store(env_temp_c10_);
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
        // D5: "a parent has a request" envelope on the home band.
        in.parent_request = server_sync_.WaitingRequests() > 0;
        // The server link needs pairing again (revoked / lost rotation / new address).
        in.link_repair = server_sync_.Link().state == kid_app::LinkState::NeedsRepair;
        if (in.battery_pct >= 0) {  // no PMIC reading: keep the last one
            sync_battery_pct_.store(static_cast<uint8_t>(in.battery_pct > 100 ? 100 : in.battery_pct));
            sync_charging_.store(in.charging);
        }
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
    // alarm), so readers copy them under parent_mutex_. Saved in the atomic
    // ledger (schema v3, one commit, never a pet interaction); the old
    // kid_parent/settings key is only read once by the migration. XiaoZhi's
    // own NVS settings are never touched. Opening, moving and leaving the
    // menu post no pet action.
    kid_app::ParentMenu parent_menu_;              // pet_input task only
    // D5 / §5.1 pages of the parent entry (pet_input task only): a request
    // from the parent panel waiting for confirmation, the server link page.
    kid_app::ParentPage parent_page_ = kid_app::ParentPage::Menu;
    kid_app::ConfirmDialog confirm_dialog_;
    uint32_t link_input_ms_ = 0;
    // Pairing again over a working link needs a second press (it replaces
    // the credentials): armed by the first press, shown as the "pair again" icon.
    bool repair_armed_ = false;
    uint32_t repair_armed_ms_ = 0;
    static constexpr uint32_t kRepairArmMs = 10000;
    uint32_t parent_link_shown_ = 0;  // packed LinkView last published
    kid_app::ParentSettings parent_settings_;      // guarded by parent_mutex_
    mutable std::mutex parent_mutex_;
    std::atomic<bool> parent_ready_{false};        // the ledger is running
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
    // flow) starts: starts the ledger (migrating once), which applies the
    // settings. Ledger refused: the defaults, the menu stays closed until a
    // later Pet-mode toggle starts it; NVS is left as it is.
    void LoadParentSettings() {
        const bool ready = StartPetLedger();
        ESP_LOGI(TAG, "PET_PARENT_LOAD result=%s", ready ? "loaded" : "error");
        if (!ready) ApplyParentSettings(kid_app::ParentSettings{}, "defaults");
    }

    // Caller holds scene_state_mutex_.
    void PublishParentSceneLocked() {
        kid_app::SceneModel scene{};
        scene.kind = kid_app::SceneKind::Parent;
        scene.parent = parent_menu_.View();
        scene.parent.page = parent_page_;
        scene.parent.link = server_sync_.Link();
        if (repair_armed_ && parent_page_ == kid_app::ParentPage::Link)
            scene.parent.link = kid_app::LinkView{kid_app::LinkState::NeedsRepair, 0};
        if (parent_page_ == kid_app::ParentPage::Request)
            scene.parent.request = confirm_dialog_.View();
        parent_link_shown_ = kid_app::PackLinkView(scene.parent.link);
        pet_mailbox_.Publish(scene);
    }

    void RepublishParent() {
        SceneStateLock lock(scene_state_mutex_);
        PublishParentSceneLocked();
    }

    void ShowParentPage(kid_app::ParentPage page) {
        parent_page_ = page;
        repair_armed_ = false;
        if (page != kid_app::ParentPage::Request) confirm_dialog_.Close();
        parent_menu_.Touch(NowMs());
        link_input_ms_ = NowMs();
        RepublishParent();
    }

    bool LinkBusy() const {
        const auto state = server_sync_.Link().state;
        return state == kid_app::LinkState::Connecting || state == kid_app::LinkState::ShowCode ||
               state == kid_app::LinkState::Saving;
    }

    // D5: the next request waiting for the parent, else the settings menu.
    void ShowNextRequestOrMenu() {
        kid_app::DeviceCommand request{};
        uint8_t more = 0;
        if (server_sync_.NextRequest(&request, &more)) {
            confirm_dialog_.Open(request, more, NowMs());
            ShowParentPage(kid_app::ParentPage::Request);
        } else {
            ShowParentPage(kid_app::ParentPage::Menu);
        }
    }

    // pet_input: the open parent entry takes every input, page by page.
    void HandleParentInput(kid_app::InputKind kind) {
        if (parent_page_ == kid_app::ParentPage::Request) {
            HandleRequestAction(confirm_dialog_.OnInput(kind, NowMs()));
            return;
        }
        if (parent_page_ == kid_app::ParentPage::Link) {
            link_input_ms_ = NowMs();
            if (kind == kid_app::InputKind::DialLong) {
                ShowParentPage(kid_app::ParentPage::Menu);
            } else if (kind == kid_app::InputKind::DialPress && !LinkBusy()) {
                // Linked already: the first press only asks "pair again?" (the
                // icon changes), the second one within kRepairArmMs does it.
                // Credentials change only once the new token is confirmed.
                const bool linked = server_sync_.Link().state == kid_app::LinkState::Paired;
                if (linked && !repair_armed_) {
                    repair_armed_ = true;
                    repair_armed_ms_ = NowMs();
                    ESP_LOGI(TAG, "PET_PARENT_PAIR_ARMED");
                    RepublishParent();
                    return;
                }
                repair_armed_ = false;
                server_sync_.BeginPairing();
                ESP_LOGI(TAG, "PET_PARENT_PAIR");
                RepublishParent();
            }
            return;
        }
        HandleParentAction(parent_menu_.OnInput(kind, NowMs()), "long");
    }

    // "连接服务器": the page starts pairing unless it is linked or busy.
    void OpenLinkPage() {
        const auto state = server_sync_.Link().state;
        if (state != kid_app::LinkState::Paired && !LinkBusy()) server_sync_.BeginPairing();
        ESP_LOGI(TAG, "PET_PARENT_LINK state=%u", static_cast<unsigned>(state));
        ShowParentPage(kid_app::ParentPage::Link);
    }

    // D5: confirm commits on the Pet task with the Parent source; reject is
    // stored by the network task. Either way the server hears it in the
    // next snapshot. A long press decides later (the request keeps waiting).
    void HandleRequestAction(kid_app::ConfirmAction action) {
        const auto& command = confirm_dialog_.Command();
        switch (action) {
            case kid_app::ConfirmAction::None:
                return;
            case kid_app::ConfirmAction::Redraw:
                RepublishParent();
                return;
            case kid_app::ConfirmAction::Confirm:
            case kid_app::ConfirmAction::Reject: {
                const bool confirm = action == kid_app::ConfirmAction::Confirm;
                const bool ok = confirm ? server_sync_.ConfirmRequest(command.id)
                                        : server_sync_.RejectRequest(command.id);
                ESP_LOGI(TAG, "PET_PARENT_REQUEST decision=%s kind=%u ok=%d",
                         confirm ? "confirm" : "reject", static_cast<unsigned>(command.kind),
                         ok ? 1 : 0);
                ShowNextRequestOrMenu();
                return;
            }
            case kid_app::ConfirmAction::Exit:
                ESP_LOGI(TAG, "PET_PARENT_REQUEST decision=later");
                ShowParentPage(kid_app::ParentPage::Menu);
                return;
        }
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
        EndGame("parent", false);  // V4-06
        parent_care_ = ParentCareNow();  // V8 page 2: habit and birthday
        parent_menu_.Open(ParentSettingsNow(), NowMs(), parent_care_);
        // D5: a request from the parent panel is shown first.
        parent_page_ = kid_app::ParentPage::Menu;
        link_input_ms_ = NowMs();
        kid_app::DeviceCommand request{};
        uint8_t more = 0;
        if (server_sync_.NextRequest(&request, &more)) {
            confirm_dialog_.Open(request, more, NowMs());
            parent_page_ = kid_app::ParentPage::Request;
        }
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
        confirm_dialog_.Close();
        parent_page_ = kid_app::ParentPage::Menu;
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
            case kid_app::ParentMenuAction::OpenLink:
                OpenLinkPage();
                return;
            case kid_app::ParentMenuAction::SavedCare:
                SaveParentCare(parent_menu_.Care());
                break;
            case kid_app::ParentMenuAction::Saved: {
                const auto next = parent_menu_.Settings();
                const auto saved = pet_service_.SaveParentSettings(next, pdMS_TO_TICKS(3000));
                if (saved != kid_app::CommitResult::Committed) {
                    // Keep running with what the ledger holds; show it again.
                    const auto kept = pet_service_.ParentNow();
                    if (kept != ParentSettingsNow()) ApplyParentSettings(kept, "ledger");
                    parent_menu_.Revert(ParentSettingsNow());
                    ESP_LOGW(TAG, "PET_PARENT_SAVE_FAILED item=%s result=%d", item,
                             static_cast<int>(saved));
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
        // V8 page 2: a habit / birthday the ledger refused shows what it holds.
        if (parent_care_failed_.exchange(false)) {
            parent_care_ = ParentCareNow();
            parent_menu_.RevertCare(parent_care_);
            ESP_LOGW(TAG, "PET_PARENT_SAVE_FAILED item=care");
            if (parent_page_ == kid_app::ParentPage::Menu) {
                SceneStateLock lock(scene_state_mutex_);
                PublishParentSceneLocked();
            }
        }
        if (!pet_mode_.load()) {
            CloseParentMenu("pet_off");
            return;
        }
        if (parent_page_ == kid_app::ParentPage::Request) {
            HandleRequestAction(confirm_dialog_.Tick(NowMs()));
        } else if (parent_page_ == kid_app::ParentPage::Link) {
            // The code stays on screen while the pairing runs.
            // Only a code on screen keeps the page open (an offline or stuck
            // pairing still times out like any other idle parent page).
            if (server_sync_.Link().state == kid_app::LinkState::ShowCode) link_input_ms_ = NowMs();
            if (NowMs() - link_input_ms_ >=
                kid_app::SelfTestClock::Scaled(kid_app::ParentMenu::kIdleExitMs)) {
                CloseParentMenu("idle");
                return;
            }
        } else {
            HandleParentAction(parent_menu_.Tick(NowMs()), "idle");
        }
        if (repair_armed_ && NowMs() - repair_armed_ms_ >= kRepairArmMs) {
            repair_armed_ = false;  // not confirmed in time
            RepublishParent();
        }
        // The server link changed (code shown, paired, failed): redraw.
        if (parent_menu_.IsOpen() && parent_page_ != kid_app::ParentPage::Request &&
            kid_app::PackLinkView(server_sync_.Link()) != parent_link_shown_)
            RepublishParent();
        input_hub_.SetAutoRepeat(parent_menu_.Editing());
    }

    // ---- V4-06 mini games (PRD 7) ----
    // The Game rail item starts a short game of reviewed rounds: from the
    // TF pack /sdcard/games/default.kpg when it is valid, else the built-in
    // set. pet_input owns game_. The game is UI only, like the parent menu:
    // no Pet action, last_pet_scene_ keeps the home picture and only Game
    // frames reach the panel while it runs (pet_mailbox_.game_only). It
    // ends by itself (rounds done, long press, 60 s without input) or for
    // sleep, a voice session, the parent menu, the morning picture or Pet
    // mode off. pet_prompt opens the pack right after the sound pack and
    // plays every game clip (PromptRequest::game) under the same volume
    // rules (parent cap, quiet ceiling) as the other pet sounds.
    kid_app::GameSession game_;                // pet_input only
    // V4-07: the Game rail item first opens game_menu_ (quiz / tilt ball /
    // peekaboo); the quiz is game_, the other two are motion_. game_mode_
    // says which one owns the dial and the screen.
    enum class GameMode : uint8_t { None, Menu, Quiz, Motion, Play, Care, Sound };
    kid_app::GameMenu game_menu_;              // pet_input only
    kid_app::MotionGameSession motion_;        // pet_input only
    GameMode game_mode_ = GameMode::None;      // pet_input only
    uint8_t motion_round_logged_ = 0;          // pet_input
    std::atomic<bool> motion_active_{false};   // pet_boot_diag / gesture sources read
    std::atomic<bool> motion_tilt_{false};     // the ball game: pet_boot_diag feeds tilt
    std::atomic<uint8_t> motion_gesture_{0};   // latest gesture for motion_, 0 = none
    kid_app::TiltTracker tilt_tracker_;        // pet_boot_diag only
    // V8 games (docs/V8_MINI_GAMES.md): the non-card ones (memory flip, maze,
    // shake dice / tree, wooden man, day and night) are play_; the card games
    // (shapes, counting, spot the difference, ...) are game_ with another pool.
    // Reward rules are the same: only the dial answer (or the asked-for motion).
    static kid_app::TiltConfig MazeTiltConfig() {
        kid_app::TiltConfig config{};
        config.four_way = true;  // axes and signs: calibrate on hardware
        return config;
    }
    kid_app::PlaySession play_;                 // pet_input only
    kid_app::TiltTracker maze_tracker_{MazeTiltConfig()};  // pet_boot_diag only
    std::atomic<bool> play_tilt_{false};        // the maze: pet_boot_diag feeds four-way tilt
    std::atomic<bool> play_shake_{false};       // dice / tree: shakes reach the game
    std::atomic<bool> play_window_{false};      // wooden man: pet_boot_diag queues IMU windows
    std::mutex play_window_mutex_;
    kid_app::MotionStats::Summary play_windows_[4];
    uint8_t play_window_count_ = 0;
    std::array<kid_app::GameRound, 3> live_pool_{};  // clock / weather rounds of this game
    std::atomic<int> live_hour_{-1};            // RTC hour, -1 = clock not set (pet_boot_diag)
    std::atomic<int> live_minute_{0};
    std::atomic<bool> live_temp_ok_{false};     // SHTC3 reading (pet_boot_diag)
    std::atomic<int> live_temp_c10_{0};
    uint8_t play_round_logged_ = 0;             // pet_input
    std::atomic<bool> game_active_{false};     // read by the other tasks
    bool game_requested_ = false;              // pet_input: rail press seen
    uint8_t game_round_logged_ = 0;            // pet_input
    std::atomic<uint32_t> game_seed_{0};       // self-test "game" step, 0 = random
    std::atomic<uint32_t> game_tokens_{0};
    // Owns the one boot-selected KPG1 file/reader throughout this boot;
    // never swaps it while the game/audio workers share the same pointer.
    kid_app::GameCourseLoader game_courses_;
    static constexpr uint8_t kGamePackPending = 0, kGamePackNone = 1, kGamePackReady = 2;
    std::atomic<uint8_t> game_pack_state_{kGamePackPending};

    bool GameActive() const { return game_active_.load(); }
    kid_app::GamePackReader* GamePackNow() {
        return game_pack_state_.load() == kGamePackReady ? game_courses_.Pack() : nullptr;
    }

    // pet_prompt, once at boot after the sound pack. Parent-owned TF file
    // /games/active.txt selects which preinstalled KPG1 to open; malformed,
    // missing or invalid content falls back to default.kpg, then builtin.
    // No runtime hot switch: GameClip and GameSession share this reader.
    void LoadGamePack() {
        const uint32_t started_ms = NowMs();
        const char* state = "no_sd";
        if (sound_sd_.load() == kSoundSdMounted) {
            const bool ok = game_courses_.LoadAtBoot("/sdcard/games");
            state = ok ? "ready" : "builtin";
        }
        game_pack_state_.store(game_courses_.Ready() ? kGamePackReady : kGamePackNone);
        ESP_LOGI(TAG, "PET_GAME_PACK state=%s active=%s selected=%s choice=%s rounds=%u clips=%u id=%u rev=%u ms=%u",
                 state,
                 game_courses_.LoadedName(),
                 kid_app::GameCourseResultName(game_courses_.State()),
                 kid_app::GameCourseChoiceName(game_courses_.Selection()),
                 static_cast<unsigned>(game_courses_.Rounds()),
                 static_cast<unsigned>(game_courses_.Clips()),
                 static_cast<unsigned>(game_courses_.ContentId()),
                 static_cast<unsigned>(game_courses_.Pack() ? game_courses_.Pack()->Revision() : 0),
                 static_cast<unsigned>(NowMs() - started_ms));
    }

    // pet_input. Quiet hours are for sleeping, a low battery for charging:
    // then the rail's Game item only makes TuanTuan say hi.
    bool GameStartAllowed() {
        const char* refused =
            quiet_hours_.load()   ? "quiet"
            : battery_.LowPower() ? "battery"
            : Application::GetInstance().GetDeviceState() != kDeviceStateIdle ? "voice"
                                                                               : nullptr;
        if (!refused) return true;
        ESP_LOGI(TAG, "PET_GAME_REFUSED reason=%s", refused);
        return false;
    }

    void PostGameClip(uint16_t clip) {
        if (!clip || !pet_mode_.load() || sleep_active_.load() || ParentMenuOpen()) return;
        kid_app::PromptRequest request{};
        request.game = clip;
        request.token = game_tokens_.fetch_add(1) + 1;
        PostPrompt(request);
    }

    // Caller holds scene_state_mutex_.
    void PublishGameSceneLocked() {
        kid_app::SceneModel scene{};
        if (game_mode_ == GameMode::Care) {  // V8 care activity / diary / album
            scene.kind = kid_app::SceneKind::Care;
            scene.care = care_.View();
            pet_mailbox_.Publish(scene);
            return;
        }
        scene.kind = kid_app::SceneKind::Game;
        scene.game = game_mode_ == GameMode::Menu     ? game_menu_.View()
                     : game_mode_ == GameMode::Motion ? motion_.View()
                     : game_mode_ == GameMode::Play   ? play_.View()  // V8
                     : game_mode_ == GameMode::Sound  ? PublishableSoundView()  // V8 sound
                     : mic_session_                   ? mic_game_.View()  // V4-08
                                                      : game_.View();
        pet_mailbox_.Publish(scene);
    }

    void LogGameRound() {
        const auto& round = game_.Current();
        const auto view = game_.View();
        game_round_logged_ = view.round;
        ESP_LOGI(TAG, "PET_GAME_ROUND n=%u/%u kind=%s options=%u answer=%u",
                 static_cast<unsigned>(view.round), static_cast<unsigned>(view.rounds),
                 kid_app::GameKindName(round.kind), static_cast<unsigned>(round.option_count),
                 static_cast<unsigned>(round.correct));
    }

    // pet_input, after the rail press (scene_state_mutex_ released): the
    // game menu comes first.
    void StartGame() {
        kid_app::MicGameKind mic_kind{};
        if (PickMicGame(&mic_kind)) {  // self-test `micgame` only: straight into the game
            StartMicGame(mic_kind);
            return;
        }
        OpenGameMenu(0);
    }

    void OpenGameMenu(uint8_t start) {
        const auto out = game_menu_.Start(NowMs(), start);
        if (out.finished) {  // an empty activity group: leave the game frame, never stick in it
            ESP_LOGW(TAG, "PET_GAME_MENU_EMPTY");
            EndGame("menu_empty");
            return;
        }
        {
            SceneStateLock lock(scene_state_mutex_);
            game_active_.store(true);
            game_mode_ = GameMode::Menu;
            pet_mailbox_.game_only.store(true);
            {
                SceneStateLock anim_lock(anim_mutex_);
                anim_director_.Reset(NowMs());
            }
            PublishGameSceneLocked();
        }
        ESP_LOGI(TAG, "PET_GAME_MENU highlight=%u", static_cast<unsigned>(start));
        PostGameClip(out.clip);
    }

    void HandleMenuOutput(const kid_app::GameMenuOutput& out) {
        PostGameClip(out.clip);
        if (out.finished) {
            EndGame(kid_app::GameEndName(game_menu_.End()));
            return;
        }
        if (out.chosen != kid_app::GameChoice::None) {
            ESP_LOGI(TAG, "PET_GAME_CHOICE game=%s", kid_app::GameChoiceName(out.chosen));
            if (out.chosen == kid_app::GameChoice::Quiz) {
                StartQuiz();
            } else if (out.chosen == kid_app::GameChoice::Candle ||
                       out.chosen == kid_app::GameChoice::Clap) {  // V4-08
                StartMicGame(out.chosen == kid_app::GameChoice::Candle
                                 ? kid_app::MicGameKind::Candle
                                 : kid_app::MicGameKind::ClapRhythm);
            } else if (out.chosen == kid_app::GameChoice::Ball ||
                       out.chosen == kid_app::GameChoice::Peekaboo) {
                if (!motion_enabled_.load()) {  // parents switched motion off
                    ESP_LOGI(TAG, "PET_GAME_REFUSED reason=motion_off");
                    OpenGameMenu(kid_app::ActivityIndexOf(out.chosen));
                } else {
                    StartMotion(out.chosen == kid_app::GameChoice::Ball
                                    ? kid_app::MotionKind::Ball
                                    : kid_app::MotionKind::Peekaboo);
                }
            } else if (kid_app::GameChoiceIsPlay(out.chosen)) {  // V8 games (play_content.h)
                const kid_app::PlayGame game = kid_app::PlayGameOf(out.chosen);
                const kid_app::Activity* row = kid_app::FindActivity(out.chosen);
                if (row && row->needs == kid_app::ActivityNeeds::Motion && !motion_enabled_.load()) {
                    ESP_LOGI(TAG, "PET_GAME_REFUSED reason=motion_off");
                    OpenGameMenu(kid_app::ActivityIndexOf(out.chosen));
                } else {
                    StartPlayGame(game);
                }
            } else if (out.chosen == kid_app::GameChoice::Parrot) {  // V8 sound
                StartSound(SoundKind::Parrot);
            } else if (out.chosen == kid_app::GameChoice::Songs) {
                StartSound(SoundKind::Songs);
            } else if (out.chosen == kid_app::GameChoice::ToothBrush) {
                StartSound(SoundKind::Brush);
            } else if (out.chosen == kid_app::GameChoice::HandWash) {
                StartSound(SoundKind::Wash);
            // V8 activity list (activity_list.h): sessions that append a row
            // add their dispatch branch here, before this fallback.
            } else if (CareKindOf(out.chosen) != kid_app::CareKind::None) {  // V8 session 1
                const auto* row = kid_app::FindActivity(out.chosen);
                if (row && row->needs == kid_app::ActivityNeeds::Motion && !motion_enabled_.load()) {
                    ESP_LOGI(TAG, "PET_GAME_REFUSED reason=motion_off");
                    if (row->group == kid_app::ActivityGroup::Care)
                        OpenCareMenu(kid_app::ActivityIndexOf(out.chosen));
                    else
                        OpenGameMenu(kid_app::ActivityIndexOf(out.chosen));
                } else {
                    StartCare(CareKindOf(out.chosen));
                }
            } else {
                // A row without a handler in this build: stay in the menu.
                ESP_LOGW(TAG, "PET_GAME_REFUSED reason=no_handler game=%s",
                         kid_app::GameChoiceName(out.chosen));
                OpenGameMenu(kid_app::ActivityIndexOf(out.chosen));
            }
            return;
        }
        if (!out.redraw) return;
        ESP_LOGI(TAG, "PET_GAME_MENU_SELECT index=%u",
                 static_cast<unsigned>(game_menu_.View().highlight));
        SceneStateLock lock(scene_state_mutex_);
        if (game_active_.load()) PublishGameSceneLocked();
    }

    void LogMotionRound() {
        const auto view = motion_.View();
        motion_round_logged_ = view.round;
        ESP_LOGI(TAG, "PET_MOTION_ROUND n=%u/%u kind=%s ball=%u hole=%u",
                 static_cast<unsigned>(view.round), static_cast<unsigned>(view.rounds),
                 kid_app::MotionKindName(motion_.Kind()), static_cast<unsigned>(view.ball),
                 static_cast<unsigned>(view.hole));
    }

    // pet_input, after the menu choice. Nothing is recorded: the IMU only
    // feeds window means to the pure game logic.
    void StartMotion(kid_app::MotionKind kind) {
        uint32_t seed = game_seed_.exchange(0);
        if (!seed) seed = esp_random();
        kid_app::GameOutput out{};
        if (!motion_.Start(kind, seed, kid_app::MotionGameSession::kDefaultRounds, NowMs(),
                           &out)) {
            ESP_LOGW(TAG, "PET_GAME_START_FAILED src=motion");
            EndGame("start_failed");
            return;
        }
        {
            SceneStateLock lock(scene_state_mutex_);
            game_mode_ = GameMode::Motion;
            motion_gesture_.store(0);
            motion_tilt_.store(kind == kid_app::MotionKind::Ball);
            motion_active_.store(true);
            PublishGameSceneLocked();
        }
        ESP_LOGI(TAG, "PET_GAME_START src=motion kind=%s id=%u seed=%u rounds=%u",
                 kid_app::MotionKindName(kind), static_cast<unsigned>(motion_.Result().game_id),
                 static_cast<unsigned>(seed), static_cast<unsigned>(motion_.Result().rounds));
        LogMotionRound();
        PostGameClip(out.clip);
    }

    void LogMotionEvent(const kid_app::GameView& before, const kid_app::GameOutput& out) {
        const auto view = motion_.View();
        if (out.right) {
            if (motion_.Kind() == kid_app::MotionKind::Ball)
                ESP_LOGI(TAG, "PET_MOTION_HOLE n=%u hole=%u correct=%u",
                         static_cast<unsigned>(view.round), static_cast<unsigned>(view.hole),
                         static_cast<unsigned>(motion_.Result().correct));
            else
                ESP_LOGI(TAG, "PET_MOTION_FOUND n=%u correct=%u",
                         static_cast<unsigned>(view.round),
                         static_cast<unsigned>(motion_.Result().correct));
        } else if (view.feedback == kid_app::GameFeedback::TryAgain &&
                   before.feedback != view.feedback) {
            ESP_LOGI(TAG, "PET_MOTION_TOO_SOON n=%u", static_cast<unsigned>(view.round));
        } else if (out.redraw && view.motion == 1) {
            ESP_LOGI(TAG, "PET_MOTION_STEP ball=%u hole=%u", static_cast<unsigned>(view.ball),
                     static_cast<unsigned>(view.hole));
        } else if (out.redraw && view.hidden) {
            ESP_LOGI(TAG, "PET_MOTION_HIDE n=%u", static_cast<unsigned>(view.round));
        }
    }

    void ApplyMotionOutput(const kid_app::GameOutput& out) {
        PostGameClip(out.clip);
        if (out.finished) {
            EndGame(kid_app::GameEndName(motion_.Result().end));
            return;
        }
        if (!out.redraw) return;
        if (motion_.View().round != motion_round_logged_) LogMotionRound();
        SceneStateLock lock(scene_state_mutex_);
        if (game_active_.load()) PublishGameSceneLocked();
    }

    // ---- V8 games ----
    void PushPlayWindow(const kid_app::MotionStats::Summary& window) {  // pet_boot_diag
        std::lock_guard<std::mutex> lock(play_window_mutex_);
        if (play_window_count_ < 4) play_windows_[play_window_count_++] = window;
    }

    void DrainPlayWindows() {  // pet_input
        kid_app::MotionStats::Summary windows[4];
        uint8_t count = 0;
        {
            std::lock_guard<std::mutex> lock(play_window_mutex_);
            count = play_window_count_;
            for (uint8_t i = 0; i < count; ++i) windows[i] = play_windows_[i];
            play_window_count_ = 0;
        }
        for (uint8_t i = 0; i < count && game_active_.load(); ++i)
            ApplyPlayOutput(play_.OnWindow(windows[i], NowMs()));
    }

    void ApplyPlayOutput(const kid_app::GameOutput& out) {
        PostGameClip(out.clip);
        if (out.right)
            ESP_LOGI(TAG, "PET_PLAY_RIGHT game=%s correct=%u",
                     kid_app::FindPlayGame(play_.Game())->name,
                     static_cast<unsigned>(play_.Result().correct));
        if (out.finished) {
            EndGame(kid_app::GameEndName(play_.Result().end));
            return;
        }
        if (!out.redraw) return;
        const kid_app::GameResult& result = play_.Result();
        const uint8_t round = static_cast<uint8_t>(result.played + 1);
        if (round != play_round_logged_) {
            play_round_logged_ = round;
            ESP_LOGI(TAG, "PET_PLAY_ROUND game=%s n=%u/%u",
                     kid_app::FindPlayGame(play_.Game())->name, static_cast<unsigned>(round),
                     static_cast<unsigned>(result.rounds));
        }
        SceneStateLock lock(scene_state_mutex_);
        if (game_active_.load()) PublishGameSceneLocked();
    }

    // pet_input, after the menu choice. Card games are a GameSession over a
    // bank (the clock and weather rounds come from the RTC and the SHTC3);
    // the others are PlaySession. Nothing is recorded.
    void StartPlayGame(kid_app::PlayGame game) {
        const kid_app::PlayGameInfo* info = kid_app::FindPlayGame(game);
        if (!info) {
            EndGame("start_failed");
            return;
        }
        uint32_t seed = game_seed_.exchange(0);
        if (!seed) seed = esp_random();
        kid_app::GameOutput out{};
        if (kid_app::IsCardGame(game)) {
            size_t count = 0;
            const kid_app::GameRound* pool = kid_app::PlayBank(game, &count);
            if (game == kid_app::PlayGame::Clock) {
                const int hour = live_hour_.load();
                count = kid_app::BuildClockPool(hour >= 0, static_cast<uint8_t>(hour < 0 ? 0 : hour),
                                                static_cast<uint8_t>(live_minute_.load()),
                                                live_pool_.data());
                pool = live_pool_.data();
            } else if (game == kid_app::PlayGame::Weather) {
                const int heat = on_battery_.load() ? kid_app::EnvTipPolicy::kBatteryHeatC10
                                                    : kid_app::EnvTipPolicy::kUsbHeatC10;
                count = kid_app::BuildWeatherPool(live_temp_ok_.load(),
                                                  live_temp_c10_.load() - heat, live_pool_.data());
                pool = live_pool_.data();
            }
            if (!pool || !game_.Start(pool, count, info->id, seed,
                                      kid_app::GameSession::kDefaultRounds, NowMs(), &out)) {
                ESP_LOGW(TAG, "PET_GAME_START_FAILED src=play game=%s", info->name);
                EndGame("start_failed");
                return;
            }
            {
                SceneStateLock lock(scene_state_mutex_);
                game_mode_ = GameMode::Quiz;
                PublishGameSceneLocked();
            }
            ESP_LOGI(TAG, "PET_GAME_START src=play game=%s id=%u seed=%u rounds=%u", info->name,
                     static_cast<unsigned>(info->id), static_cast<unsigned>(seed),
                     static_cast<unsigned>(game_.Result().rounds));
            LogGameRound();
            PostGameClip(out.clip);
            return;
        }
        if (!play_.Start(game, seed, NowMs(), &out)) {
            ESP_LOGW(TAG, "PET_GAME_START_FAILED src=play game=%s", info->name);
            EndGame("start_failed");
            return;
        }
        {
            SceneStateLock lock(scene_state_mutex_);
            game_mode_ = GameMode::Play;
            motion_gesture_.store(0);
            play_round_logged_ = 0;
            {
                std::lock_guard<std::mutex> window_lock(play_window_mutex_);
                play_window_count_ = 0;
            }
            play_tilt_.store(play_.WantsTilt());
            play_shake_.store(play_.WantsShake());
            play_window_.store(play_.WantsWindows());
            motion_active_.store(play_.WantsTilt() || play_.WantsShake() || play_.WantsFace());
            PublishGameSceneLocked();
        }
        ESP_LOGI(TAG, "PET_GAME_START src=play game=%s id=%u seed=%u rounds=%u", info->name,
                 static_cast<unsigned>(info->id), static_cast<unsigned>(seed),
                 static_cast<unsigned>(play_.Result().rounds));
        PostGameClip(out.clip);
    }

    // pet_input, after the menu choice (scene_state_mutex_ released).
    void StartQuiz() {
        kid_app::GamePackReader* pack = GamePackNow();
        const kid_app::GameRound* pool = pack ? pack->Rounds() : kid_app::BuiltinGameRounds();
        const size_t count = pack ? pack->RoundCount() : kid_app::BuiltinGameRoundCount();
        const uint16_t id = pack ? pack->ContentId() : kid_app::kBuiltinGameId;
        uint32_t seed = game_seed_.exchange(0);
        if (!seed) seed = esp_random();
        kid_app::GameOutput out{};
        if (!game_.Start(pool, count, id, seed, kid_app::GameSession::kDefaultRounds, NowMs(),
                         &out)) {
            ESP_LOGW(TAG, "PET_GAME_START_FAILED src=%s", pack ? "pack" : "builtin");
            EndGame("start_failed");
            return;
        }
        {
            SceneStateLock lock(scene_state_mutex_);
            game_active_.store(true);
            game_mode_ = GameMode::Quiz;
            pet_mailbox_.game_only.store(true);
            {
                SceneStateLock anim_lock(anim_mutex_);
                anim_director_.Reset(NowMs());
            }
            PublishGameSceneLocked();
        }
        ESP_LOGI(TAG, "PET_GAME_START src=%s id=%u seed=%u rounds=%u", pack ? "pack" : "builtin",
                 static_cast<unsigned>(id), static_cast<unsigned>(seed),
                 static_cast<unsigned>(game_.Result().rounds));
        LogGameRound();
        PostGameClip(out.clip);
    }

    void ApplyGameOutput(const kid_app::GameOutput& out) {
        PostGameClip(out.clip);
        if (out.finished) {
            EndGame(kid_app::GameEndName(game_.Result().end));
            return;
        }
        if (!out.redraw) return;
        if (game_.View().round != game_round_logged_) LogGameRound();
        SceneStateLock lock(scene_state_mutex_);
        if (game_active_.load()) PublishGameSceneLocked();
    }

    // pet_input: the dial while a game runs. BOOT and PWR keep their own
    // handlers (talking or sleeping ends the game).
    void HandleGameInput(kid_app::InputKind kind) {
        if (game_mode_ == GameMode::Quiz) {
            HandleQuizInput(kind);
            return;
        }
        kid_app::GameInput input = kid_app::GameInput::Press;
        switch (kind) {
            case kid_app::InputKind::DialUp: input = kid_app::GameInput::Up; break;
            case kid_app::InputKind::DialDown: input = kid_app::GameInput::Down; break;
            case kid_app::InputKind::DialPress: input = kid_app::GameInput::Press; break;
            case kid_app::InputKind::DialLong: input = kid_app::GameInput::Long; break;
            default: return;
        }
        if (game_mode_ == GameMode::Menu) HandleMenuOutput(game_menu_.Input(input, NowMs()));
        else if (game_mode_ == GameMode::Motion) ApplyMotionOutput(motion_.Input(input, NowMs()));
        else if (game_mode_ == GameMode::Play) ApplyPlayOutput(play_.Input(input, NowMs()));
        else if (game_mode_ == GameMode::Care) ApplyCareOutput(care_.Input(input, NowMs()));
        else if (game_mode_ == GameMode::Sound) HandleSoundInput(input);  // V8 sound
    }

    void HandleQuizInput(kid_app::InputKind kind) {
        kid_app::GameInput input = kid_app::GameInput::Press;
        switch (kind) {
            case kid_app::InputKind::DialUp: input = kid_app::GameInput::Up; break;
            case kid_app::InputKind::DialDown: input = kid_app::GameInput::Down; break;
            case kid_app::InputKind::DialPress: input = kid_app::GameInput::Press; break;
            case kid_app::InputKind::DialLong: input = kid_app::GameInput::Long; break;
            default: return;
        }
        if (mic_session_) {  // V4-08: the dial only ends these games (long press)
            ApplyMicOutput(mic_game_.Input(input, NowMs()));
            return;
        }
        const auto before = game_.View();
        const auto out = game_.Input(input, NowMs());
        if (input == kid_app::GameInput::Press && out.redraw) {
            static const char* const kFeedback[] = {"none", "right", "try_again", "reveal"};
            ESP_LOGI(TAG, "PET_GAME_ANSWER n=%u pick=%u right=%d feedback=%s correct=%u",
                     static_cast<unsigned>(before.round), static_cast<unsigned>(before.highlight),
                     out.right ? 1 : 0, kFeedback[static_cast<uint8_t>(game_.View().feedback) & 3],
                     static_cast<unsigned>(game_.Result().correct));
        } else if (out.redraw) {
            ESP_LOGI(TAG, "PET_GAME_SELECT index=%u", static_cast<unsigned>(game_.View().highlight));
        }
        ApplyGameOutput(out);
    }

    // pet_input, every loop.
    void UpdateGame(DeviceState state) {
        if (!game_active_.load()) return;
        if (!pet_mode_.load()) {
            EndGame("pet_off");
            return;
        }
        if (state != kDeviceStateIdle) {  // BOOT: TuanTuan listens instead
            EndGame("voice");
            return;
        }
        switch (game_mode_) {
            case GameMode::Menu:
                HandleMenuOutput(game_menu_.Tick(NowMs()));
                break;
            case GameMode::Quiz:
                if (mic_session_) UpdateMicGame(state);  // V4-08
                else ApplyGameOutput(game_.Tick(NowMs()));
                break;
            case GameMode::Motion: {
                const uint8_t pending = motion_gesture_.exchange(0);
                if (pending) {
                    const auto before = motion_.View();
                    const auto out =
                        motion_.OnGesture(static_cast<kid_app::Gesture>(pending), NowMs());
                    LogMotionEvent(before, out);
                    ApplyMotionOutput(out);
                }
                if (game_active_.load()) ApplyMotionOutput(motion_.Tick(NowMs()));
                break;
            }
            case GameMode::Play: {  // V8
                const uint8_t pending = motion_gesture_.exchange(0);
                if (pending)
                    ApplyPlayOutput(play_.OnGesture(static_cast<kid_app::Gesture>(pending), NowMs()));
                DrainPlayWindows();
                if (game_active_.load()) ApplyPlayOutput(play_.Tick(NowMs()));
                break;
            }
            case GameMode::Care: {  // V8: gestures and resting windows, then the clock
                const uint8_t pending = care_gesture_.exchange(0);
                if (pending)
                    ApplyCareOutput(care_.OnGesture(static_cast<kid_app::Gesture>(pending), NowMs()));
                const uint8_t answer = habit_answer_.exchange(0);  // the habit tick's result
                if (answer) ApplyCareOutput(care_.Confirm(answer == 1, NowMs()));
                const uint8_t window = care_window_.exchange(0);
                if (window && game_active_.load() && game_mode_ == GameMode::Care)
                    ApplyCareOutput(care_.OnWindow(window == 1, NowMs()));
                if (game_active_.load() && game_mode_ == GameMode::Care)
                    ApplyCareOutput(care_.Tick(NowMs()));
                break;
            }
            case GameMode::Sound:
                UpdateSound();  // V8
                break;
            case GameMode::None:
                break;
        }
    }

    // pet_input. Back to the pet's own (durable) home picture, as it is now;
    // publish_home=false when the caller shows its own picture right away
    // (sleep, parent menu, morning), so the panel skips a needless refresh.
    void EndGame(const char* reason, bool publish_home = true) {
        if (!game_active_.load()) return;
        DisarmMic();  // V4-08: the microphone is off as soon as a game ends, whatever the reason
        // Interrupt is a no-op for a game that already finished by itself.
        kid_app::GameResult result{};
        const char* mode = "quiz";
        switch (game_mode_) {
            case GameMode::Menu:
                game_menu_.Interrupt();
                result.end = game_menu_.End();
                mode = "menu";
                break;
            case GameMode::Motion:
                motion_.Interrupt();
                result = motion_.Result();
                mode = kid_app::MotionKindName(motion_.Kind());
                break;
            case GameMode::Play:  // V8
                play_.Interrupt();
                result = play_.Result();
                mode = "play";
                break;
            case GameMode::Care:  // V8: care records its own events (PostCare)
                care_.Interrupt();
                mode = kid_app::CareKindName(care_.Kind());
                break;
            case GameMode::Sound:
                StopSound();  // V8: wipes the parrot recording, stops songs
                mode = "sound";
                break;
            case GameMode::Quiz:
            case GameMode::None:
                if (mic_session_) {
                    mic_game_.Interrupt();
                    result = mic_game_.Result();
                    mode = "mic";
                } else {
                    game_.Interrupt();
                    result = game_.Result();
                }
                break;
        }
        mic_session_ = false;
        {
            SceneStateLock lock(scene_state_mutex_);
            game_active_.store(false);
            game_mode_ = GameMode::None;
            motion_active_.store(false);
            motion_tilt_.store(false);
            play_tilt_.store(false);
            play_shake_.store(false);
            play_window_.store(false);
            care_motion_.store(false);  // V8
            pet_mailbox_.game_only.store(false);
            if (publish_home && pet_mode_.load() && !sleep_active_.load())
                pet_mailbox_.Publish(WithVoiceFace(UnpackPetScene(last_pet_scene_.load())));
        }
        FinishCareFeed();  // V8: the meal the child chose, also if interrupted
        // ADR-012: only a finished game with a correct dial answer counts for
        // growth (GAME_COMPLETED, source Device, option = game id), through
        // the ledger like every other memory (RecordMemory). Id and day go
        // back to 0: the atomic Pet worker assigns the next local id and the
        // RTC day.
        kid_app::MemoryEvent memory{};
        const bool growth =
            kid_app::GameCompletedMemoryEvent(result, kid_app::kLocalEventBit, 0, &memory);
        bool posted = false;
        if (growth) {
            memory.id = 0;
            posted = RecordMemory(memory);
        }
        ESP_LOGI(TAG, "PET_GAME_END mode=%s end=%s reason=%s played=%u/%u correct=%u id=%u "
                 "growth=%d posted=%d",
                 mode, kid_app::GameEndName(result.end), reason, static_cast<unsigned>(result.played),
                 static_cast<unsigned>(result.rounds), static_cast<unsigned>(result.correct),
                 static_cast<unsigned>(result.game_id), growth ? 1 : 0, posted ? 1 : 0);
    }

    // ---- V8 care and growth (docs/V8_CARE_AND_GROWTH.md) ----
    // The home rail's Feed item opens the care menu (GameMenu on the Care
    // group of the activity list); its cards and the Play group's sticker
    // album run CareSession in the game frame (GameMode::Care, same guards,
    // same game_only gate). Sessions never write: a finished care hands one
    // CareEvent to the ledger (PostCare: source checked by CareStore).
    kid_app::CareSession care_;                   // pet_input only
    std::atomic<uint8_t> care_gesture_{0};        // latest gesture for care_, 0 = none
    std::atomic<uint8_t> care_window_{0};         // latest IMU window: 1 resting, 2 moving
    std::atomic<bool> care_motion_{false};        // care_ wants gestures and windows
    std::atomic<uint32_t> local_minute_{0};       // RTC care minute, 0 = unknown
    std::atomic<uint32_t> home_look_{0};          // PackLook for home pictures
    std::atomic<uint8_t> festival_{0};            // today's kid_app::Festival
    std::atomic<bool> sticker_news_{false};       // a care commit earned a sticker
    std::atomic<bool> feed_sticker_{false};       // announce it after the next yummy line
    std::atomic<uint8_t> habit_answer_{0};        // Pet worker: 1 tick committed, 2 refused
    std::atomic<bool> parent_care_failed_{false}; // Pet worker: page 2 save refused
    uint32_t look_failed_ms_ = 0;                 // kid_epd: last failed look refresh
    std::atomic<uint32_t> care_tokens_{0};
    kid_app::HomeLook look_shown_{};              // kid_epd only
    kid_app::ParentCare parent_care_{};           // pet_input: values the menu opened with
    bool care_requested_ = false;                 // pet_input: Feed rail press seen
    bool care_feed_pending_ = false;              // pet_input: a meal chosen, Feed not posted yet
    uint8_t festival_posted_ = 0;                 // pet_boot_diag: FestivalSeen sent

    static kid_app::CareKind CareKindOf(kid_app::GameChoice choice) {
        switch (choice) {
            case kid_app::GameChoice::Feed: return kid_app::CareKind::Feed;
            case kid_app::GameChoice::Bath: return kid_app::CareKind::Bath;
            case kid_app::GameChoice::Brush: return kid_app::CareKind::Brush;
            case kid_app::GameChoice::Tickle: return kid_app::CareKind::Tickle;
            case kid_app::GameChoice::Dress: return kid_app::CareKind::Dress;
            case kid_app::GameChoice::Habit: return kid_app::CareKind::Habit;
            case kid_app::GameChoice::Stickers: return kid_app::CareKind::Album;
            default: return kid_app::CareKind::None;
        }
    }

    kid_app::ParentCare ParentCareNow() const {
        const auto care = pet_service_.CareNow();
        kid_app::ParentCare out{};
        out.habit = static_cast<uint8_t>(care.GetHabit().habit);
        out.birth_month = care.BirthMonth();
        out.birth_day = care.BirthDay();
        return out;
    }

    // pet_input: the parent changed the habit or the birthday (page 2).
    void SaveParentCare(const kid_app::ParentCare& next) {
        bool posted = true;
        if (next.habit != parent_care_.habit) {
            kid_app::CareEvent e{};
            e.kind = kid_app::CareEventKind::HabitSet;
            e.source = kid_app::MemorySource::Parent;
            e.option = next.habit;
            posted = pet_service_.PostCare(e) && posted;
        }
        if (next.birth_month != parent_care_.birth_month || next.birth_day != parent_care_.birth_day) {
            kid_app::CareEvent e{};
            e.kind = kid_app::CareEventKind::BirthdaySet;
            e.source = kid_app::MemorySource::Parent;
            e.slot = next.birth_month;
            e.option = next.birth_day;
            posted = pet_service_.PostCare(e) && posted;
        }
        if (!posted) {
            parent_menu_.RevertCare(parent_care_);
            ESP_LOGW(TAG, "PET_PARENT_SAVE_FAILED item=care");
            return;
        }
        parent_care_ = next;
        ESP_LOGI(TAG, "PET_PARENT_SAVED item=care habit=%u birthday=%u-%u",
                 static_cast<unsigned>(next.habit), static_cast<unsigned>(next.birth_month),
                 static_cast<unsigned>(next.birth_day));
    }

    // pet_boot_diag (UpdateRhythm): needs from the RTC minute, today's
    // festival, the worn clothes. Clock unknown: a content pet, no festival.
    void UpdateHomeLook(const kid_app::CivilTime& rtc, bool clock_ok, int phase) {
        const auto care = pet_service_.CareNow();
        const uint32_t minute = kid_app::CareMinute(rtc, clock_ok);
        const auto day_phase = phase >= 0 ? static_cast<kid_app::DayPhase>(phase)
                                          : kid_app::DayPhase::Day;
        uint8_t level = 0;
        const auto need = kid_app::CareStore::TopNeed(care.Needs(minute, day_phase), &level);
        const auto festival = kid_app::FestivalOn(rtc, clock_ok, care.BirthMonth(), care.BirthDay());
        kid_app::HomeLook look{};
        for (uint8_t i = 0; i < kid_app::kWearSlots; ++i)
            look.worn[i] = care.Worn(static_cast<kid_app::WearSlot>(i));
        look.need = static_cast<uint8_t>(need);
        look.need_level = level;
        look.festival = static_cast<uint8_t>(festival);
        festival_.store(static_cast<uint8_t>(festival));
        const uint32_t bits = kid_app::PackLook(look);
        if (home_look_.exchange(bits) != bits)
            ESP_LOGI(TAG, "PET_CARE_LOOK worn=%u,%u,%u,%u,%u need=%u level=%u festival=%s",
                     static_cast<unsigned>(look.worn[0]), static_cast<unsigned>(look.worn[1]),
                     static_cast<unsigned>(look.worn[2]), static_cast<unsigned>(look.worn[3]),
                     static_cast<unsigned>(look.worn[4]),
                     static_cast<unsigned>(look.need), static_cast<unsigned>(level),
                     kid_app::FestivalName(festival));
        // The festival sticker, once: the device saw the day (source Device).
        // PostCare is refused until the ledger runs; then the next tick retries.
        const uint32_t bit = festival == kid_app::Festival::Birthday
                                 ? kid_app::StickerBit(kid_app::Sticker::Birthday)
                                 : kid_app::StickerBit(kid_app::Sticker::Festival);
        if (festival != kid_app::Festival::None && !(care.Stickers() & bit) &&
            festival_posted_ != static_cast<uint8_t>(festival)) {
            kid_app::CareEvent e{};
            e.kind = kid_app::CareEventKind::FestivalSeen;
            e.source = kid_app::MemorySource::Device;
            e.option = static_cast<uint16_t>(festival);
            if (pet_service_.PostCare(e)) festival_posted_ = static_cast<uint8_t>(festival);
        }
    }

    // kid_epd, idle on the home picture: draw a changed look (needs move
    // slowly, clothes on a commit) as one small pet-area refresh.
    void MaybeRefreshLook(const kid_app::SceneModel& displayed) {
        if (displayed.kind != kid_app::SceneKind::Home) return;
        if (kid_app::UnpackLook(home_look_.load()) == look_shown_) return;
        if (look_failed_ms_ && NowMs() - look_failed_ms_ < 60000) return;
        {
            SceneStateLock lock(anim_mutex_);
            if (anim_director_.Active()) return;  // never between two frames
        }
        if (xSemaphoreTake(pet_screen_mutex_, pdMS_TO_TICKS(1000)) != pdTRUE) return;
        bool okay = false;
        if (pet_mode_.load() && !sleep_active_.load()) okay = DrawPetFrameLocked(displayed, "look");
        xSemaphoreGive(pet_screen_mutex_);
        look_failed_ms_ = okay ? 0 : (NowMs() ? NowMs() : 1);
        ESP_LOGI(TAG, "PET_CARE_LOOK_REFRESH ok=%d", okay ? 1 : 0);
    }

    // pet_input, after the care frame closed: the durable V2 Feed for a meal
    // chosen in the food menu (PetCommitted plays the hearts and the line).
    void FinishCareFeed() {
        if (!care_feed_pending_) return;
        care_feed_pending_ = false;
        if (sticker_news_.exchange(false)) feed_sticker_.store(true);
        if (!pet_service_.PostLocal(kid_app::PetAction::Feed)) ESP_LOGW(TAG, "PET_INPUT_REJECTED");
    }

    // Rail Sleep in the evening (or at night) with nothing else going on.
    // Self-test scripts keep the V2 sleep path (no clock-dependent steps).
    bool BedtimeRitualAllowed() {
        if (selftest_seen_.load()) return false;
        const int phase = day_phase_.load();
        return (phase == static_cast<int>(kid_app::DayPhase::Evening) ||
                phase == static_cast<int>(kid_app::DayPhase::Night)) &&
               !battery_.LowPower() &&
               Application::GetInstance().GetDeviceState() == kDeviceStateIdle;
    }

    // Good morning after the V5-04 line: hello-less V8 lines (how the room
    // feels, today's festival, the habit reminder) into out[kMaxDiaryLines].
    // Temperature only: the board reads no humidity yet (comfy / hot / cold).
    size_t MorningCareLines(uint16_t* out) {
        const auto care = pet_service_.CareNow();
        const auto status = NewestStatus(kid_app::SceneKind::Home);
        const bool temp_ok = status.temp_c != kid_app::StatusBar::kNoTemp;
        const auto feel = kid_app::FeelOf(static_cast<int16_t>(status.temp_c * 10), 50, temp_ok);
        kid_app::CivilTime rtc{};
        bool stopped = true;
        const bool clock_ok = RtcRead(&rtc, &stopped) && !stopped;
        const auto festival = kid_app::FestivalOn(rtc, clock_ok, care.BirthMonth(), care.BirthDay());
        festival_.store(static_cast<uint8_t>(festival));
        uint16_t ids[kid_app::kMaxDiaryLines] = {};
        const size_t n = kid_app::ComposeMorning(pet_service_.TemperamentNow(), feel, festival,
                                                 care.GetHabit().habit, ids,
                                                 kid_app::kMaxDiaryLines);
        // ids[0] is the V8 hello: the V5-04 good-morning line says it already.
        size_t count = 0;
        for (size_t i = 1; i < n; ++i) out[count++] = ids[i];
        ESP_LOGI(TAG, "PET_CARE_MORNING lines=%u feel=%u festival=%s", static_cast<unsigned>(count),
                 static_cast<unsigned>(feel),
                 kid_app::FestivalName(static_cast<kid_app::Festival>(festival_.load())));
        return count;
    }

    void OpenCareMenu(uint8_t start) {
        const auto out = game_menu_.Start(NowMs(), start, kid_app::ActivityGroup::Care);
        if (out.finished) {  // an empty activity group: leave the game frame, never stick in it
            ESP_LOGW(TAG, "PET_GAME_MENU_EMPTY");
            EndGame("menu_empty");
            return;
        }
        {
            SceneStateLock lock(scene_state_mutex_);
            game_active_.store(true);
            game_mode_ = GameMode::Menu;
            pet_mailbox_.game_only.store(true);
            {
                SceneStateLock anim_lock(anim_mutex_);
                anim_director_.Reset(NowMs());
            }
            PublishGameSceneLocked();
        }
        ESP_LOGI(TAG, "PET_CARE_MENU highlight=%u", static_cast<unsigned>(start));
        PostGameClip(out.clip);
    }

    // pet_input: from the care / play menu or the bedtime ritual.
    void StartCare(kid_app::CareKind kind) {
        const auto care = pet_service_.CareNow();
        const auto memory = pet_service_.MemoryNow();
        const uint32_t day = local_day_.load();
        kid_app::CareContext ctx{};
        ctx.temperament = pet_service_.TemperamentNow();
        ctx.stickers = care.Stickers();
        for (uint8_t i = 0; i < kid_app::kWearSlots; ++i)
            ctx.worn[i] = care.Worn(static_cast<kid_app::WearSlot>(i));
        ctx.habit = care.GetHabit().habit;
        ctx.habit_done_today = day && care.GetHabit().last_day == day;
        ctx.clock_known = local_minute_.load() != 0;
        ctx.diary = kid_app::MakeDiaryPage(care, memory, day);
        kid_app::CareOutput out{};
        if (!care_.Start(kind, ctx, NowMs(), &out)) {
            ESP_LOGW(TAG, "PET_CARE_START_FAILED kind=%s", kid_app::CareKindName(kind));
            EndGame("start_failed");
            return;
        }
        {
            SceneStateLock lock(scene_state_mutex_);
            game_active_.store(true);
            game_mode_ = GameMode::Care;
            pet_mailbox_.game_only.store(true);
            care_gesture_.store(0);
            care_window_.store(0);
            habit_answer_.store(0);
            care_motion_.store(care_.WantsMotion());
            {
                SceneStateLock anim_lock(anim_mutex_);
                anim_director_.Reset(NowMs());
            }
            PublishGameSceneLocked();
        }
        ESP_LOGI(TAG, "PET_CARE_START kind=%s stickers=0x%x habit=%s", kid_app::CareKindName(kind),
                 static_cast<unsigned>(ctx.stickers), kid_app::HabitName(ctx.habit));
        if (kind == kid_app::CareKind::Diary) {  // TuanTuan reads today's page
            uint16_t ids[kid_app::kMaxDiaryLines] = {};
            const size_t n = kid_app::ComposeDiary(ctx.diary, ctx.temperament, ids,
                                                   kid_app::kMaxDiaryLines);
            PostCareLines(ids, n);
            ESP_LOGI(TAG, "PET_CARE_DIARY lines=%u food=%u books=%u games=%u cares=%u habit=%d",
                     static_cast<unsigned>(n), static_cast<unsigned>(ctx.diary.last_food),
                     static_cast<unsigned>(ctx.diary.books), static_cast<unsigned>(ctx.diary.games),
                     static_cast<unsigned>(ctx.diary.cares), ctx.diary.habit_done ? 1 : 0);
        } else {
            PostCareClip(out.clip);
        }
    }

    void PostCareClip(uint16_t clip) {
        if (!clip) return;
        if (clip >= kid_app::kGameToneBase) {  // a built-in tone
            PostGameClip(clip);
            return;
        }
        PostCareLines(&clip, 1);
    }

    void ApplyCareOutput(const kid_app::CareOutput& out) {
        PostCareClip(out.clip);
        if (out.record) {
            kid_app::CareEvent e = out.event;
            e.id = 0;       // the Pet worker assigns the next local id
            e.minute = 0;   // and the RTC minute
            const bool posted = pet_service_.PostCare(e);
            // Feeding is also the V2 durable Feed (interaction count, today's
            // feeds, hearts and the yummy line): posted once the care picture
            // is gone (FinishCareFeed), so they play on the home screen.
            if (e.kind == kid_app::CareEventKind::Fed) care_feed_pending_ = true;
            ESP_LOGI(TAG, "PET_CARE_RECORD kind=%u src=%u slot=%u option=%u posted=%d",
                     static_cast<unsigned>(e.kind), static_cast<unsigned>(e.source),
                     static_cast<unsigned>(e.slot), static_cast<unsigned>(e.option), posted ? 1 : 0);
        }
        if (out.finished) {
            const auto kind = care_.Kind();
            ESP_LOGI(TAG, "PET_CARE_END kind=%s next=%s sleep=%d", kid_app::CareKindName(kind),
                     kid_app::CareKindName(out.next), out.sleep ? 1 : 0);
            if (out.next != kid_app::CareKind::None) {  // bedtime: tidy -> diary -> lullaby
                StartCare(out.next);
                return;
            }
            const bool bedtime = out.sleep && BedtimeRitualAllowed();
            EndGame("care", !bedtime);  // also posts a chosen meal (FinishCareFeed)
            if (bedtime) {
                HandleSleepAction(sleep_flow_.Request(NowMs()));
                return;
            }
            if (sticker_news_.exchange(false)) {
                const uint16_t news = kid_app::CarePhraseId(kid_app::CareLine::StickerNew, 0,
                                                            pet_service_.TemperamentNow());
                PostCareLines(&news, 1);
            }
            return;
        }
        if (!out.redraw) return;
        SceneStateLock lock(scene_state_mutex_);
        if (game_active_.load() && game_mode_ == GameMode::Care) PublishGameSceneLocked();
    }

    // ---- V8 sound activities: parrot, songs, tooth brushing, hand washing ----
    // Inside the V4-06 game frame like the mic games: the same SceneKind::Game
    // scene (GameView `sound` subtype, kid_app/sound_view.h), the same
    // GameActive() guards, no new SceneKind. The rules live in kid_app
    // (parrot_session / song_player / routine_timer, host-tested); this is
    // only the wiring. Opened from the activity list (GameChoice::Parrot /
    // Songs / ToothBrush / HandWash) or by the voice proposals; BOOT (talk)
    // and PWR (sleep) end them like any game (UpdateGame / HandleSleepAction).
    //
    // PRIVACY (parrot): the child's voice exists only in parrot_buffer_ (RAM,
    // PSRAM, allocated when the parrot opens, wiped and freed when it ends)
    // and in the 20 ms staging block of the playback task (scrubbed after
    // every hand-over). The raw frame tap (scripts/v8_mic_frame_tap_patch.py)
    // is armed only while the child holds the dial centre; nothing is written
    // to flash or TF, logged or sent (only sample COUNTS are logged).
    enum class SoundKind : uint8_t { None, Parrot, Songs, Brush, Wash };
    SoundKind sound_kind_ = SoundKind::None;          // pet_input
    kid_app::ParrotSession parrot_;                   // under parrot_mutex_
    kid_app::SongPlayer songs_;                       // pet_input only
    kid_app::RoutineTimer routine_;                   // pet_input only
    kid_app::FreertosSongWorker song_worker_;
    kid_app::GameView sound_published_{};             // pet_input
    std::mutex parrot_mutex_;
    int16_t* parrot_buffer_ = nullptr;                // pet_input (alloc / free)
    std::atomic<bool> parrot_holding_{false};         // the dial centre edges belong to the parrot
    std::atomic<uint32_t> parrot_play_gen_{0};
    bool parrot_up_pending_ = false;                  // pet_input: debounce of the release
    uint32_t parrot_up_at_ms_ = 0;
    uint32_t parrot_takes_ = 0;
    std::atomic<uint8_t> song_volume_{100};

    static bool SongAudioBegin(uint64_t token, void* context) {
        return StoryAudioBegin(token, context);
    }
    static bool SongAudioPush(uint64_t token, const int16_t* pcm, size_t count, void* context) {
        auto* self = static_cast<WaveshareEsp32s3ePaper3inch97*>(context);
        if (!self || !self->pet_mode_.load() || self->sleep_active_.load() ||
            Application::GetInstance().GetDeviceState() != kDeviceStateIdle)
            return false;
        // Parent cap and quiet ceiling, then the player's own volume (fade-out,
        // low volume for lullabies in quiet hours).
        const uint32_t pct = static_cast<uint32_t>(self->StoryVolumePct()) * self->song_volume_.load() / 100;
        if (count > self->story_scaled_.size()) return false;
        for (size_t i = 0; i < count; ++i) self->story_scaled_[i] = pcm[i];
        kid_app::ScalePcm(self->story_scaled_.data(), count, static_cast<uint8_t>(pct));
        return Application::GetInstance().GetAudioService().TryQueueLocalStoryPcm(
            token, self->story_scaled_.data(), count);
    }
    static void SongAudioCancel(void*) {
        Application::GetInstance().GetAudioService().CancelLocalStoryPcm();
    }
    static void ParrotFrameSink(void* context, const int16_t* mono, size_t samples) {
        auto* self = static_cast<WaveshareEsp32s3ePaper3inch97*>(context);
        std::lock_guard<std::mutex> lock(self->parrot_mutex_);
        self->parrot_.Feed(mono, samples);
    }

    kid_app::ParrotContext ParrotCtx() {
        kid_app::ParrotContext c;
        c.quiet_hours = quiet_hours_.load();
        c.low_battery = battery_.LowPower();
        c.voice_active = Application::GetInstance().GetDeviceState() != kDeviceStateIdle;
        return c;
    }

    kid_app::GameView SoundViewNow() {
        const uint32_t now = NowMs();
        switch (sound_kind_) {
            case SoundKind::Parrot: {
                std::lock_guard<std::mutex> lock(parrot_mutex_);
                return kid_app::ParrotGameView(parrot_.View());
            }
            case SoundKind::Songs: return kid_app::SongGameView(songs_.View(now));
            case SoundKind::Brush:
            case SoundKind::Wash: return kid_app::RoutineGameView(routine_.View(now));
            case SoundKind::None: break;
        }
        return kid_app::GameView{};
    }

    // Caller holds scene_state_mutex_ (PublishGameSceneLocked).
    kid_app::GameView PublishableSoundView() {
        sound_published_ = SoundViewNow();
        return sound_published_;
    }

    // pet_input, every loop while a sound activity runs: publish when the
    // picture really changed (the planner then budgets the partial refresh).
    void PublishSoundIfChanged() {
        if (!game_active_.load() || game_mode_ != GameMode::Sound) return;
        const kid_app::GameView now_view = SoundViewNow();
        if (now_view == sound_published_) return;
        SceneStateLock lock(scene_state_mutex_);
        if (game_active_.load()) PublishGameSceneLocked();
    }

    // The tones the routine cues use until the `routines` voice pack is read
    // by pet_prompt (tools/sound_extras.py; docs/V8_SOUND_AND_ROUTINES.md).
    static uint16_t RoutineCueTone(kid_app::RoutineCue cue) {
        return cue == kid_app::RoutineCue::Start    ? kid_app::kGameTonePrompt
               : cue == kid_app::RoutineCue::Middle ? kid_app::kMicClipBeat
                                                    : kid_app::GameArtTone(static_cast<uint16_t>(kid_app::GameArt::Star));
    }

    // pet_input. Songs on the card: every /sdcard/books/song_*.kpb and lullaby_*.kpb.
    void ScanSongs(kid_app::SongCatalog* catalog) {
        catalog->Clear();
        DIR* dir = opendir("/sdcard/books");
        if (!dir) return;
        while (const dirent* entry = readdir(dir)) {
            const size_t n = std::strlen(entry->d_name);
            if (n < 5 || n >= kid_app::kSongIdBytes + 4) continue;
            const char* ext = entry->d_name + n - 4;
            if (!(ext[0] == '.' && (ext[1] | 0x20) == 'k' && (ext[2] | 0x20) == 'p' && (ext[3] | 0x20) == 'b'))
                continue;
            char stem[kid_app::kSongIdBytes + 1] = {};
            for (size_t i = 0; i + 4 < n; ++i) {
                const char c = entry->d_name[i];
                stem[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;  // FAT may shout
            }
            catalog->Offer(stem);
        }
        closedir(dir);
    }

    void StartSound(SoundKind kind) {
        const uint32_t now = NowMs();
        kid_app::SongOutput song_out{};
        kid_app::ParrotOutput parrot_out{};
        kid_app::RoutineOutput routine_out{};
        bool ok = false;
        const char* name = "none";
        switch (kind) {
            case SoundKind::Parrot: {
                name = "parrot";
                // PSRAM only: 160 KB is too much for internal RAM.
                parrot_buffer_ = static_cast<int16_t*>(heap_caps_calloc(
                    kid_app::ParrotSession::kMaxSamples, sizeof(int16_t),
                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                if (!parrot_buffer_) {
                    ESP_LOGW(TAG, "PET_SOUND_REFUSED kind=parrot reason=no_psram");
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(parrot_mutex_);
                    parrot_.Bind(parrot_buffer_, kid_app::ParrotSession::kMaxSamples);
                    ok = parrot_.Open(now, ParrotCtx(), &parrot_out);
                    if (!ok) parrot_.Bind(nullptr, 0);
                }
                if (!ok) {
                    heap_caps_free(parrot_buffer_);
                    parrot_buffer_ = nullptr;
                    ESP_LOGI(TAG, "PET_SOUND_REFUSED kind=parrot reason=%s",
                             kid_app::ParrotHintName(parrot_out.hint));
                } else {
                    parrot_holding_.store(true);
                    parrot_up_pending_ = false;
                }
                break;
            }
            case SoundKind::Songs: {
                name = "songs";
                kid_app::SongCatalog catalog;
                const bool mounted = sound_sd_.load() == kSoundSdMounted;
                if (mounted) ScanSongs(&catalog);
                kid_app::SongContext context;
                context.quiet_hours = quiet_hours_.load();
                context.low_battery = battery_.LowPower();
                context.voice_active = Application::GetInstance().GetDeviceState() != kDeviceStateIdle;
                context.card_mounted = mounted;
                ok = songs_.Open(now, context, catalog, &song_out);
                if (ok && !song_worker_.Ready() &&
                    !song_worker_.Start("/sdcard/books", {&SongAudioBegin, &SongAudioPush, &SongAudioCancel}, this))
                    ESP_LOGW(TAG, "PET_SONG_AUDIO_UNAVAILABLE");
                song_volume_.store(songs_.VolumePct(now));
                if (ok)
                    ESP_LOGI(TAG, "PET_SONGS_OPEN count=%u quiet=%d card=%d",
                             static_cast<unsigned>(songs_.View(now).count), context.quiet_hours ? 1 : 0,
                             mounted ? 1 : 0);
                break;
            }
            case SoundKind::Brush:
            case SoundKind::Wash:
                name = kind == SoundKind::Brush ? "brush" : "wash";
                if (battery_.LowPower()) {
                    ESP_LOGI(TAG, "PET_SOUND_REFUSED kind=%s reason=low_battery", name);
                    break;
                }
                ok = routine_.Open(kind == SoundKind::Brush ? kid_app::RoutineKind::Brush
                                                            : kid_app::RoutineKind::Wash,
                                   now, quiet_hours_.load(), &routine_out);
                break;
            case SoundKind::None:
                break;
        }
        if (!ok) {
            EndGame("start_failed");
            return;
        }
        {
            SceneStateLock lock(scene_state_mutex_);
            sound_kind_ = kind;
            game_mode_ = GameMode::Sound;
            game_active_.store(true);
            pet_mailbox_.game_only.store(true);
            {
                SceneStateLock anim_lock(anim_mutex_);
                anim_director_.Reset(NowMs());
            }
            PublishGameSceneLocked();
        }
        ESP_LOGI(TAG, "PET_SOUND_START kind=%s", name);
    }

    void StopSound() {
        switch (sound_kind_) {
            case SoundKind::Parrot: {
                parrot_holding_.store(false);
                parrot_play_gen_.fetch_add(1);  // the playback task stops at its next block
                Application::GetInstance().GetAudioService().SetMicFrameTap(false, nullptr, nullptr);
                Application::GetInstance().GetAudioService().CancelLocalStoryPcm();
                {
                    std::lock_guard<std::mutex> lock(parrot_mutex_);
                    parrot_.Interrupt();
                    parrot_.Bind(nullptr, 0);  // wipes the buffer once more
                }
                if (parrot_buffer_) heap_caps_free(parrot_buffer_);
                parrot_buffer_ = nullptr;
                parrot_up_pending_ = false;
                break;
            }
            case SoundKind::Songs:
                songs_.Interrupt();
                if (song_worker_.Ready()) song_worker_.Close();
                Application::GetInstance().GetAudioService().CancelLocalStoryPcm();
                break;
            case SoundKind::Brush:
            case SoundKind::Wash:
                routine_.Interrupt();
                break;
            case SoundKind::None:
                break;
        }
        sound_kind_ = SoundKind::None;
        sound_published_ = kid_app::GameView{};
    }

    // ---- parrot ----
    // pet_input: the raw dial-centre edge (InputTaskLoop does not give it to
    // InputHub while the parrot is open, so its 1 s "home" long press and the
    // parent combo cannot fire while the child talks).
    void HandleParrotEdge(bool down) {
        const uint32_t now = NowMs();
        if (down) {
            if (parrot_up_pending_) {  // contact chatter: the same hold continues
                parrot_up_pending_ = false;
                return;
            }
            kid_app::ParrotOutput out;
            {
                std::lock_guard<std::mutex> lock(parrot_mutex_);
                out = parrot_.Hold(true, now, ParrotCtx(), parrot_takes_++);
            }
            ApplyParrotOutput(out);
        } else {
            parrot_up_pending_ = true;
            parrot_up_at_ms_ = now + 120;  // the hub's bounce window
        }
    }

    void UpdateParrot() {
        const uint32_t now = NowMs();
        kid_app::ParrotOutput out;
        if (parrot_up_pending_ && static_cast<int32_t>(now - parrot_up_at_ms_) >= 0) {
            parrot_up_pending_ = false;
            std::lock_guard<std::mutex> lock(parrot_mutex_);
            out = parrot_.Hold(false, now, ParrotCtx(), 0);
        } else {
            std::lock_guard<std::mutex> lock(parrot_mutex_);
            out = parrot_.Tick(now);
        }
        ApplyParrotOutput(out);
    }

    void ApplyParrotOutput(const kid_app::ParrotOutput& out) {
        auto& audio = Application::GetInstance().GetAudioService();
        if (out.start_mic) {
            audio.SetMicFrameTap(true, &ParrotFrameSink, this);
            ESP_LOGI(TAG, "PET_PARROT_REC armed=1");
        }
        if (out.stop_mic) {
            audio.SetMicFrameTap(false, nullptr, nullptr);
            size_t samples = 0;
            {
                std::lock_guard<std::mutex> lock(parrot_mutex_);
                samples = parrot_.Recorded();
            }
            ESP_LOGI(TAG, "PET_PARROT_REC armed=0 samples=%u", static_cast<unsigned>(samples));
        }
        if (out.hint != kid_app::ParrotHint::None)
            ESP_LOGI(TAG, "PET_PARROT_HINT hint=%s", kid_app::ParrotHintName(out.hint));
        if (out.play) StartParrotPlayback();
        if (out.leave) EndGame("parrot_done");
    }

    // One short task per take: Render -> software volume -> the single
    // AudioService PCM queue (like the prompts); the staging block is
    // scrubbed after every hand-over and at the end.
    void StartParrotPlayback() {
        parrot_play_gen_.fetch_add(1);
        if (xTaskCreate([](void* context) {
                auto* self = static_cast<WaveshareEsp32s3ePaper3inch97*>(context);
                self->ParrotPlayLoop();
                vTaskDelete(nullptr);
            }, "pet_parrot", 4096, this, 2, nullptr) != pdPASS) {
            std::lock_guard<std::mutex> lock(parrot_mutex_);
            parrot_.PlaybackRefused();
            ESP_LOGW(TAG, "PET_PARROT_PLAY_FAILED reason=task");
        }
    }

    void ParrotPlayLoop() {
        const uint32_t gen = parrot_play_gen_.load();
        auto& audio = Application::GetInstance().GetAudioService();
        const uint64_t session = (uint64_t{0x50415252u} << 32) | gen;  // "PARR"
        int16_t chunk[480];
        size_t total = 0;
        {
            std::lock_guard<std::mutex> lock(parrot_mutex_);
            total = parrot_.PlaybackSamples();
        }
        bool started = false;
        for (int i = 0; i < 80 && !started; ++i) {  // older audio drains first
            if (parrot_play_gen_.load() != gen || !PromptAllowed(false)) break;
            started = audio.BeginLocalStoryPcm(session);
            if (!started) vTaskDelay(pdMS_TO_TICKS(10));
        }
        size_t offset = 0;
        bool failed = !started;
        while (started && offset < total) {
            if (parrot_play_gen_.load() != gen) {
                failed = true;
                break;
            }
            size_t n = 0;
            {
                std::lock_guard<std::mutex> lock(parrot_mutex_);
                n = parrot_.Render(offset, chunk, 480);  // wipes with the last block
            }
            if (n == 0) {
                failed = offset < total;
                break;
            }
            const uint8_t volume = PromptVolumePct();
            kid_app::ScalePcm(chunk, n, volume);
            int stalls = 0;
            while (!audio.TryQueueLocalStoryPcm(session, chunk, n)) {
                if (parrot_play_gen_.load() != gen || !PromptAllowed(false) || ++stalls > 50) {
                    failed = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            kid_app::ParrotSession::Scrub(chunk, n);
            if (failed) break;
            offset += n;
        }
        kid_app::ParrotSession::Scrub(chunk, 480);
        if (failed && parrot_play_gen_.load() == gen) {
            std::lock_guard<std::mutex> lock(parrot_mutex_);
            parrot_.PlaybackRefused();  // refused / stuck: wipe, ready again
        }
        ESP_LOGI(TAG, "PET_PARROT_PLAY samples=%u done=%d", static_cast<unsigned>(total), failed ? 0 : 1);
    }

    // ---- songs ----
    void ApplySongOutput(const kid_app::SongOutput& out) {
        if (out.hint != kid_app::SongHint::None)
            ESP_LOGI(TAG, "PET_SONGS_HINT hint=%s", kid_app::SongHintName(out.hint));
        if (out.stop_audio) {
            if (song_worker_.Ready()) song_worker_.Stop();
            Application::GetInstance().GetAudioService().CancelLocalStoryPcm();
        }
        if (out.load) {
            const char* id = songs_.Chosen().id;
            const bool opened = song_worker_.Ready() && song_worker_.Open(id);
            ESP_LOGI(TAG, "PET_SONGS_LOAD id=%s ok=%d", id, opened ? 1 : 0);
            if (!opened) ApplySongOutput(songs_.Loaded(0, NowMs()));
        }
        if (out.play_page) {
            ESP_LOGI(TAG, "PET_SONGS_PLAY page=%u/%u bedtime=%d", static_cast<unsigned>(out.play_page),
                     static_cast<unsigned>(songs_.Pages()), songs_.Bedtime() ? 1 : 0);
            if (song_worker_.Ready()) song_worker_.Play(out.play_page);
        }
        if (out.sleep_now) {
            ESP_LOGI(TAG, "PET_SONGS_TIMER_DONE");
            HandleSleepAction(sleep_flow_.Request(NowMs()));  // the one sleep flow (3 s to cancel)
        }
        if (out.leave && game_active_.load()) EndGame("songs_done");
    }

    void UpdateSongs() {
        const uint32_t now = NowMs();
        if (quiet_hours_.load() != songs_.Quiet()) ApplySongOutput(songs_.SetQuiet(quiet_hours_.load(), now));
        if (!game_active_.load() || sound_kind_ != SoundKind::Songs) return;
        uint16_t value = 0;
        switch (song_worker_.TakeEvent(&value)) {
            case kid_app::FreertosSongWorker::Event::Opened:
                ApplySongOutput(songs_.Loaded(value > 255 ? 255 : static_cast<uint8_t>(value), now));
                break;
            case kid_app::FreertosSongWorker::Event::OpenFailed:
                ApplySongOutput(songs_.Loaded(0, now));
                break;
            case kid_app::FreertosSongWorker::Event::PageDone:
                ApplySongOutput(songs_.PageFinished(now));
                break;
            case kid_app::FreertosSongWorker::Event::None:
                break;
        }
        if (!game_active_.load() || sound_kind_ != SoundKind::Songs) return;
        song_volume_.store(songs_.VolumePct(now));
        ApplySongOutput(songs_.Tick(now));
    }

    // ---- brushing / washing ----
    void ApplyRoutineOutput(const kid_app::RoutineOutput& out) {
        if (out.cue != kid_app::RoutineCue::None) PostGameClip(RoutineCueTone(out.cue));
        if (out.completed) {
            // One Device-source care interaction (the dial press started it).
            // The care store (V8-01) takes this record; until it is wired
            // it is only logged. Voice / Network never produce one.
            kid_app::RoutineCareRecord record;
            if (routine_.TakeRecord(&record))
                ESP_LOGI(TAG, "PET_CARE_ROUTINE kind=%s seconds=%u source=device posted=0",
                         kid_app::RoutineKindName(record.kind),
                         static_cast<unsigned>(record.seconds));
        }
        if (out.leave && game_active_.load()) EndGame("routine_done");
    }

    // pet_input: the dial while a sound activity runs.
    void HandleSoundInput(kid_app::GameInput input) {
        const uint32_t now = NowMs();
        switch (sound_kind_) {
            case SoundKind::Parrot: {
                kid_app::ParrotOutput out;
                {
                    std::lock_guard<std::mutex> lock(parrot_mutex_);
                    out = parrot_.Input(input, now);
                }
                ApplyParrotOutput(out);
                break;
            }
            case SoundKind::Songs:
                ApplySongOutput(songs_.Input(input, now));
                break;
            case SoundKind::Brush:
            case SoundKind::Wash:
                ApplyRoutineOutput(routine_.Input(input, now));
                break;
            case SoundKind::None:
                break;
        }
        PublishSoundIfChanged();
    }

    // pet_input, every loop (UpdateGame has already ended the activity for a
    // voice session or Pet mode off).
    void UpdateSound() {
        switch (sound_kind_) {
            case SoundKind::Parrot: UpdateParrot(); break;
            case SoundKind::Songs: UpdateSongs(); break;
            case SoundKind::Brush:
            case SoundKind::Wash:
                ApplyRoutineOutput(routine_.Tick(NowMs()));
                break;
            case SoundKind::None:
                break;
        }
        PublishSoundIfChanged();
    }

    // ---- V4-08 microphone games (blow the candles, clap the rhythm back) ----
    // Inside the V4-06 game frame (same scene, same GameActive() guards, same
    // growth memory rule). PRIVACY: the only thing taken from the microphone
    // is one RMS number per 10 ms frame, handed to MicClassifier by the
    // AudioInputTask (SetMicEnvelopeTap, scripts/v4_mic_tap_patch.py); no
    // audio is copied, recorded, stored or sent anywhere. The tap is armed
    // only while a mic game asks for it (MicWanted()), never in a voice
    // session (UpdateGame ends the game; the AudioService also clears the
    // tap when voice processing starts), and it is off as soon as the game
    // ends. A host self-test can inject synthetic loudness instead
    // (`envelope` step): the real tap then stays off.
    kid_app::MicGameSession mic_game_;      // pet_input only
    bool mic_session_ = false;              // pet_input: the running game is a mic game
    bool mic_armed_ = false;                // pet_input: tap/injection on
    std::atomic<uint8_t> mic_force_{0};     // self-test "micgame": 1 candle, 2 clap
    kid_app::MicClassifier mic_classifier_;  // under mic_mutex_
    std::mutex mic_mutex_;
    std::atomic<bool> mic_blow_{false};     // latest decision (audio task writes)
    std::atomic<uint32_t> mic_claps_{0};    // claps since pet_input last looked
    std::atomic<bool> mic_inject_{false};   // self-test drives the "microphone"
    std::atomic<uint8_t> mic_synth_kind_{0};
    std::atomic<uint32_t> mic_synth_start_ms_{0}, mic_synth_until_ms_{0};
    uint32_t mic_synth_clock_ms_ = 0;       // pet_input: next synthetic frame time
    bool mic_heard_blow_ = false;           // pet_input: log edges only
    uint8_t mic_lit_logged_ = 0xFF;
    uint8_t mic_claps_logged_ = 0xFF;

    static void MicSink(void* context, uint16_t rms) {
        static_cast<WaveshareEsp32s3ePaper3inch97*>(context)->OnMicFrame(rms);
    }

    // Audio input task (or pet_input when injecting). One number in, a
    // decision out; the number is not kept.
    void OnMicFrame(uint16_t rms) {
        std::lock_guard<std::mutex> lock(mic_mutex_);
        const kid_app::MicDecision decision = mic_classifier_.Feed(rms);
        mic_blow_.store(decision.blowing);
        if (decision.clap) mic_claps_.fetch_add(1);
    }

    // pet_input, when the Game item starts a game. Real children get the
    // quiz most of the time and a mic game every other round; a script that
    // names a seed (`game <seed>`) keeps the quiz, `micgame` forces a mic game.
    bool PickMicGame(kid_app::MicGameKind* kind) {
        const uint8_t forced = mic_force_.exchange(0);
        if (forced) {
            *kind = forced == 1 ? kid_app::MicGameKind::Candle : kid_app::MicGameKind::ClapRhythm;
            return true;
        }
        return false;  // children pick in the game menu (V4-07); no rotation any more
    }

    void StartMicGame(kid_app::MicGameKind kind) {
        kid_app::GameOutput out{};
        if (!mic_game_.Start(kind, NowMs(), &out)) {
            ESP_LOGW(TAG, "PET_MIC_GAME_START_FAILED kind=%s", kid_app::MicGameKindName(kind));
            EndGame("start_failed");
            return;
        }
        {
            SceneStateLock lock(scene_state_mutex_);
            mic_session_ = true;
            game_mode_ = GameMode::Quiz;  // the quiz dispatch routes mic games too
            game_active_.store(true);
            pet_mailbox_.game_only.store(true);
            {
                SceneStateLock anim_lock(anim_mutex_);
                anim_director_.Reset(NowMs());
            }
            PublishGameSceneLocked();
        }
        mic_lit_logged_ = 0xFF;
        mic_claps_logged_ = 0xFF;
        ESP_LOGI(TAG, "PET_GAME_START src=mic id=%u kind=%s rounds=%u",
                 static_cast<unsigned>(mic_game_.Result().game_id), kid_app::MicGameKindName(kind),
                 static_cast<unsigned>(mic_game_.Result().rounds));
        PostGameClip(out.clip);
    }

    void ArmMic() {
        if (mic_armed_) return;
        mic_armed_ = true;
        {
            std::lock_guard<std::mutex> lock(mic_mutex_);
            mic_classifier_.Reset();  // learns the room again before it decides
        }
        mic_blow_.store(false);
        mic_claps_.store(0);
        mic_heard_blow_ = false;
        mic_synth_clock_ms_ = 0;
        const bool inject = mic_inject_.load();
        if (!inject)
            Application::GetInstance().GetAudioService().SetMicEnvelopeTap(true, &MicSink, this);
        ESP_LOGI(TAG, "PET_MIC_TAP armed=1 src=%s", inject ? "script" : "audio");
    }

    void DisarmMic() {
        if (!mic_armed_) return;
        mic_armed_ = false;
        Application::GetInstance().GetAudioService().SetMicEnvelopeTap(false, nullptr, nullptr);
        mic_blow_.store(false);
        mic_claps_.store(0);
        ESP_LOGI(TAG, "PET_MIC_TAP armed=0");
    }

    // pet_input, every loop while a mic game runs (never in a voice session:
    // UpdateGame has already ended the game then).
    void UpdateMicGame(DeviceState state) {
        const bool want = state == kDeviceStateIdle && mic_game_.MicWanted();
        if (want) ArmMic();
        else DisarmMic();
        const uint32_t now = NowMs();
        if (mic_armed_ && mic_inject_.load()) {
            // Synthetic loudness, one frame per 10 ms of real time.
            if (!mic_synth_clock_ms_ || now - mic_synth_clock_ms_ > 200) mic_synth_clock_ms_ = now;
            const auto kind = static_cast<kid_app::MicSynth>(mic_synth_kind_.load());
            const uint32_t start = mic_synth_start_ms_.load(), until = mic_synth_until_ms_.load();
            while (mic_synth_clock_ms_ + kid_app::MicClassifier::kFrameMs <= now) {
                const bool live = mic_synth_clock_ms_ >= start && mic_synth_clock_ms_ < until;
                OnMicFrame(kid_app::SynthEnvelopeRms(live ? kind : kid_app::MicSynth::Silence,
                                                     live ? mic_synth_clock_ms_ - start : 0));
                mic_synth_clock_ms_ += kid_app::MicClassifier::kFrameMs;
            }
        }
        kid_app::MicDecision decision{};
        const uint32_t claps = mic_claps_.exchange(0);
        if (want) {
            decision.blowing = mic_blow_.load();
            decision.clap = claps > 0;
            if (decision.blowing != mic_heard_blow_) {
                mic_heard_blow_ = decision.blowing;
                if (mic_heard_blow_) ESP_LOGI(TAG, "PET_MIC_HEARD kind=blow");
            }
            if (decision.clap) ESP_LOGI(TAG, "PET_MIC_HEARD kind=clap");
        }
        ApplyMicOutput(mic_game_.Tick(now, decision));
    }

    void ApplyMicOutput(const kid_app::GameOutput& out) {
        PostGameClip(out.clip);
        if (out.finished) {
            EndGame(kid_app::GameEndName(mic_game_.Result().end));
            return;
        }
        if (!out.redraw) return;
        const auto view = mic_game_.View();
        if (mic_game_.Kind() == kid_app::MicGameKind::Candle) {
            if (mic_game_.LitCandles() != mic_lit_logged_) {
                mic_lit_logged_ = mic_game_.LitCandles();
                ESP_LOGI(TAG, "PET_MIC_CANDLE lit=%u", static_cast<unsigned>(mic_lit_logged_));
            }
        } else if (mic_game_.ClapsHeard() != mic_claps_logged_ ||
                   view.feedback != kid_app::GameFeedback::None) {
            mic_claps_logged_ = mic_game_.ClapsHeard();
            static const char* const kFeedback[] = {"none", "right", "try_again", "reveal"};
            ESP_LOGI(TAG, "PET_MIC_CLAPS round=%u claps=%u feedback=%s right=%d",
                     static_cast<unsigned>(mic_game_.Round() + 1),
                     static_cast<unsigned>(mic_claps_logged_),
                     kFeedback[static_cast<uint8_t>(view.feedback) & 3], out.right ? 1 : 0);
        }
        SceneStateLock lock(scene_state_mutex_);
        if (game_active_.load()) PublishGameSceneLocked();
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
            EnterPetAtBoot();
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
            // Trials keep starting outside Pet mode: their "pet" step enters it.
            EnterPetAtBoot();
            return;
        }
        // PowerManager never powers off by itself during a trial.
        selftest_seen_.store(true);
        // TEST ONLY time scale ("timescale" steps): real time again however
        // the script ends (done or abort); a reboot is real time anyway.
        struct RealTimeAtEnd {
            ~RealTimeAtEnd() {
                if (!kid_app::SelfTestClock::Scaling()) return;
                kid_app::SelfTestClock::SetForSelfTest(kid_app::SelfTestClock::kRealPct);
                ESP_LOGI(TAG, "PET_SELFTEST_TIMESCALE pct=100 reason=end");
            }
        } real_time_at_end;
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
                case kid_app::SelfTestOp::VoiceCmd:
                    // V6-02: as if the server's fixed-command matcher had sent it.
                    ESP_LOGI(TAG, "PET_SELFTEST_VCMD name=%s", step.name);
                    RequestVoiceCommand(step.name);
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
                case kid_app::SelfTestOp::Temp: {
                    // V4-03: SHTC3 readings as if from the sensor (status
                    // band and life-tip policy); "off" hands back the real one.
                    const bool off = step.ms == kid_app::SelfTestScript::kTempOff;
                    env_scripted_.store(true);
                    env_inject_c10_.store(off ? kEnvNoInject : static_cast<int>(step.ms) * 10);
                    ESP_LOGI(TAG, "PET_SELFTEST_TEMP c=%u off=%d", static_cast<unsigned>(step.ms),
                             off ? 1 : 0);
                    break;
                }
                case kid_app::SelfTestOp::Phase: {
                    // Day phase / quiet hours as the script says (the reader's
                    // bedtime story and the sleep ritual follow the phase).
                    const bool off = step.ms == kid_app::SelfTestScript::kPhaseOff;
                    phase_inject_.store(off ? kPhaseNone : static_cast<int>(step.ms));
                    rhythm_refresh_.store(true);
                    ESP_LOGI(TAG, "PET_SELFTEST_PHASE phase=%s off=%d",
                             off ? "rtc" : PhaseName(static_cast<int>(step.ms)), off ? 1 : 0);
                    break;
                }
                case kid_app::SelfTestOp::Volume:
                    prompt_volume_pct_.store(static_cast<uint8_t>(step.ms));
                    ESP_LOGI(TAG, "PET_SELFTEST_VOLUME pct=%u", static_cast<unsigned>(step.ms));
                    break;
                case kid_app::SelfTestOp::Mark:
                    ESP_LOGI(TAG, "PET_SELFTEST_MARK name=%s", step.name);
                    break;
                case kid_app::SelfTestOp::TimeScale:
                    // The parser keeps it within 10..100 %; see selftest_clock.h
                    // for which timers follow it (refresh and audio never do).
                    kid_app::SelfTestClock::SetForSelfTest(step.ms);
                    ESP_LOGI(TAG, "PET_SELFTEST_TIMESCALE pct=%u",
                             static_cast<unsigned>(kid_app::SelfTestClock::Pct()));
                    break;
                case kid_app::SelfTestOp::TimerWait:
                    // Waits for a scaled feature timer: as long as it is now.
                    vTaskDelay(pdMS_TO_TICKS(kid_app::SelfTestClock::Scaled(step.ms)));
                    break;
                case kid_app::SelfTestOp::WaitIdle: {
                    // Instead of a fixed boot wait: XiaoZhi idle (Wi-Fi up,
                    // activated) for 1 s in a row, at most step.ms. Not ready
                    // in time: go on, the pet toggle retries or aborts.
                    const uint32_t start = NowMs();
                    uint32_t idle_since = 0;
                    bool ready = false;
                    while (!ready && NowMs() - start < step.ms) {
                        const uint32_t now = NowMs();
                        if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle)
                            idle_since = 0;
                        else if (!idle_since)
                            idle_since = now ? now : 1;
                        else if (now - idle_since >= kid_app::SelfTestScript::kIdleSettleMs)
                            ready = true;
                        if (!ready) vTaskDelay(pdMS_TO_TICKS(100));
                    }
                    ESP_LOGI(TAG, "PET_SELFTEST_IDLE ready=%d ms=%u", ready ? 1 : 0,
                             static_cast<unsigned>(NowMs() - start));
                    break;
                }
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
                case kid_app::SelfTestOp::Game:
                    // V4-06: the next mini game plays these rounds.
                    game_seed_.store(step.ms);
                    ESP_LOGI(TAG, "PET_SELFTEST_GAME seed=%u", static_cast<unsigned>(step.ms));
                    break;
                case kid_app::SelfTestOp::MicGame:
                    // V4-08: the next game is this microphone game, and the
                    // microphone is the script's from now on (synthetic
                    // loudness, never the real tap).
                    mic_inject_.store(true);
                    mic_force_.store(std::strcmp(step.name, "clap") == 0 ? 2 : 1);
                    ESP_LOGI(TAG, "PET_SELFTEST_MICGAME kind=%s", step.name);
                    break;
                case kid_app::SelfTestOp::Envelope: {
                    kid_app::MicSynth synth{};
                    if (!kid_app::MicSynthFromName(step.name, &synth)) break;
                    const uint32_t now = NowMs();
                    mic_inject_.store(true);
                    mic_synth_kind_.store(static_cast<uint8_t>(synth));
                    mic_synth_start_ms_.store(now);
                    mic_synth_until_ms_.store(now + step.ms);
                    ESP_LOGI(TAG, "PET_SELFTEST_ENVELOPE kind=%s ms=%u", step.name,
                             static_cast<unsigned>(step.ms));
                    vTaskDelay(pdMS_TO_TICKS(step.ms));
                    break;
                }
                case kid_app::SelfTestOp::Hold:
                    // Same raw-edge queue as the physical buttons.
                    PostKeyEdge(step.key, true);
                    vTaskDelay(pdMS_TO_TICKS(step.ms));
                    PostKeyEdge(step.key, false);
                    vTaskDelay(pdMS_TO_TICKS(step.gap_ms));
                    break;
            }
        }
        mic_inject_.store(false);  // V4-08: the real tap again (only a mic game arms it)
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
                // V8 lullaby: resting windows (same noise floor as the detector).
                if (care_motion_.load())
                    care_window_.store(s.max_dev_mg < kid_app::GestureConfig{}.still_dev_mg ? 1 : 2);
                // V4-07: the ball game rolls with the board's tilt.
                if (motion_tilt_.load() && motion_enabled_.load()) {
                    const auto tilt = tilt_tracker_.OnWindow(s);
                    if (tilt != kid_app::Gesture::None) HandleGesture(tilt, "imu");
                } else {
                    tilt_tracker_.Reset();
                }
                // V8: the maze tilts four ways, the wooden man reads window means.
                if (play_tilt_.load() && motion_enabled_.load()) {
                    const auto tilt = maze_tracker_.OnWindow(s);
                    if (tilt != kid_app::Gesture::None) HandleGesture(tilt, "imu");
                } else {
                    maze_tracker_.Reset();
                }
                if (play_window_.load() && motion_enabled_.load()) PushPlayWindow(s);
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
        '#include "kid_app/freertos_atomic_pet_service.h"\n'
        '#include "kid_app/ledger_persist_policy.h"\n'
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
        '#include "kid_app/voice_command.h"\n'
        '#include "kid_app/selftest_script.h"\n'
        '#include "kid_app/selftest_clock.h"\n'
        '#include "kid_app/sleep_flow.h"\n'
        '#include "kid_app/prompt_scheduler.h"\n'
        '#include "kid_app/prompt_clips.h"\n'
        '#include "kid_app/sound_bank.h"\n'
        '#include "kid_app/pet_phrases.h"\n'
        '#include "kid_app/memory_phrases.h"\n'
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
        '#include "kid_app/env_tips.h"\n'
        '#include "kid_app/parent_menu.h"\n'
        '#include "kid_app/parent_requests.h"\n'
        '#include "kid_app/freertos_server_sync_worker.h"\n'
        '#include "esp_app_desc.h"\n'
        '#include "kid_app/mini_game.h"\n'
        '#include "kid_app/mic_game.h"\n'
        '#include "kid_app/mic_envelope.h"\n'
        '#include "kid_app/game_pack_reader.h"\n'
        '#include "kid_app/game_course_loader.h"\n'
        '#include "kid_app/motion_game.h"\n'
        '#include "kid_app/play_game.h"\n'
        '#include "kid_app/play_content.h"\n'
        '#include "kid_app/activity_list.h"\n'
        '#include "kid_app/care_session.h"\n'
        '#include "kid_app/care_calendar.h"\n'
        '#include "kid_app/care_phrases.h"\n'
        '#include "kid_app/memory_store.h"\n'
        '#include "esp_random.h"\n'
        '#include <mutex>\n'
        '#include <ctime>\n'
        '#include <esp_timer.h>\n'
        '#include <freertos/queue.h>\n'
        '#include "kid_app/waveshare_story_sd.h"\n'
        '#include "kid_app/freertos_story_pcm_worker.h"\n'
        '#include "kid_app/book_reader.h"\n'
        '#include "kid_app/memory_store.h"\n'
        '#include "kid_app/sound_view.h"\n'
        '#include "kid_app/parrot_session.h"\n'
        '#include "kid_app/song_player.h"\n'
        '#include "kid_app/routine_timer.h"\n'
        '#include "kid_app/freertos_song_worker.h"\n'
        '#include "esp_heap_caps.h"\n'
        '#include <dirent.h>\n'
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
            if (GameActive()) return;  // V4-06: no story or feeding mid-game
            const auto decision = kid_app::PetInputRouter::Decide(
                pet_event_mode_.load(), kid_app::PetClick::Double);
            if (decision.valid && !pet_service_.PostLocal(decision.action))
                ESP_LOGW(TAG, "PET_INPUT_REJECTED");
        });
        boot_button_.OnMultipleClick([this]() {
            if (!pet_mode_.load() || DialPressHeld() || ParentMenuOpen()) return;
            if (GameActive()) return;  // V4-06
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


# Story / sound / game packs are named like "forest_friends.kpb". ESP-IDF's
# default FatFs has no long file names (8.3 only), so fopen() of any such
# pack failed on the board ("Approved story .kpb absent") while the reader
# placeholder hid it. Same setting as Waveshare's own SD example.
SD_LONG_FILENAMES = ("CONFIG_FATFS_LFN_HEAP=y", "CONFIG_FATFS_MAX_LFN=255")

# Build-time default OTA URL: our self-hosted pet server (CT 108). A runtime
# value saved in NVS (Wi-Fi config portal, advanced options) still overrides it.
PET_OTA_URL = 'CONFIG_OTA_URL="http://192.168.1.110:8780/xiaozhi/ota/"'

# Build-time default PetLearning server origin (https only; the firmware refuses
# plain http). NVS kid_srv/origin still overrides it when set.
PET_SERVER_ORIGIN = 'CONFIG_KID_PET_SERVER_ORIGIN="https://pet.birdshao.cn:8443"'


def patch_board_config(text: str) -> str:
    """Append the Pet SD and OTA URL settings to the board's build (config.json)."""
    config = json.loads(text)
    builds = config.get("builds") or []
    if len(builds) != 1:
        raise RuntimeError(f"Expected one board build in config.json, found {len(builds)}")
    append = builds[0].setdefault("sdkconfig_append", [])
    for line in (*SD_LONG_FILENAMES, PET_OTA_URL, PET_SERVER_ORIGIN):
        key = line.split("=", 1)[0]
        if any(item.split("=", 1)[0] == key for item in append):
            raise RuntimeError(f"Upstream board config already sets {key}")
        append.append(line)
    return json.dumps(config, indent=4, ensure_ascii=False) + "\n"


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
    from scripts import v4_mic_tap_patch  # V4-08: loudness-only microphone tap
    from scripts import v8_mic_frame_tap_patch  # V8 parrot: raw frames, RAM-only buffer

    cmake = source / "main/CMakeLists.txt"
    header = source / BOARD_REL / "custom_lcd_display.h"
    impl = source / BOARD_REL / "custom_lcd_display.cc"
    board = source / BOARD_REL / "waveshare-s3-epaper-3.97.cc"
    board_config = source / BOARD_REL / "config.json"
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
        board_config: patch_board_config(board_config.read_text(encoding="utf-8")),
        audio_h: v8_mic_frame_tap_patch.patch_audio_h(v4_mic_tap_patch.patch_audio_h(
            patch_audio_h(audio_h.read_text(encoding="utf-8")))),
        audio_cc: v8_mic_frame_tap_patch.patch_audio_cc(v4_mic_tap_patch.patch_audio_cc(
            patch_audio_cc(audio_cc.read_text(encoding="utf-8")))),
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
