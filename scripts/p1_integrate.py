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
    text = once(text, '#include "lcd_display.h"', '#include "lcd_display.h"\n#include "kid_app/scene_model.h"', "display header include")
    text = once(text, "    void EPD_Sleep();", """    void EPD_Sleep();

    // P1 manual-only pet display prototype; no voice/LLM dependency.
    bool ShowPetPreview(kid_app::Emotion emotion);
    void ExitPetPreview();
""", "display header public")
    text = once(text, "    uint8_t* buffer = NULL;", "    uint8_t* buffer = NULL;\n    bool pet_preview_enabled_ = false; // LVGL lock protected", "display header state")
    return text


DISPLAY_IMPL = r'''
bool CustomEpdDisplay::ShowPetPreview(kid_app::Emotion emotion) {
    // Do not use the FreeRTOS task stack for a 48KiB mono framebuffer.
    void* memory = heap_caps_malloc(sizeof(kid_app::MonoFrame),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!memory) {
        ESP_LOGE(TAG, "Pet framebuffer PSRAM allocation failed");
        return false;
    }

    auto* frame = new (memory) kid_app::MonoFrame();
    kid_app::SceneComposer::Render(
        kid_app::SceneModel{kid_app::SceneKind::Home, emotion, 1}, *frame);

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
    // Demonstration only: worker prevents the ePaper BUSY wait from blocking
    // the button callback or XiaoZhi main event loop.
    std::atomic<bool> pet_demo_busy_{false};
    std::atomic<bool> pet_demo_mode_{false};
    std::atomic<unsigned> pet_demo_pose_{0};

    void RequestPetPreview(bool toggle_mode) {
        if (pet_demo_busy_.exchange(true)) return;
        if (!pet_demo_mode_.load() &&
            Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
            pet_demo_busy_.store(false);
            return;
        }
        struct Job {
            WaveshareEsp32s3ePaper3inch97* board;
            bool toggle;
        };
        auto* job = new (std::nothrow) Job{this, toggle_mode};
        if (!job) {
            pet_demo_busy_.store(false);
            return;
        }
        BaseType_t result = xTaskCreate(
            [](void* ctx) {
                auto* arg = static_cast<Job*>(ctx);
                auto* self = arg->board;
                bool toggle = arg->toggle;
                delete arg;

                if (toggle && self->pet_demo_mode_.load()) {
                    self->display_->ExitPetPreview();
                    self->pet_demo_mode_.store(false);
                    ESP_LOGI(TAG, "PET_PREVIEW_OFF");
                } else {
                    unsigned value = toggle ? 1u : (self->pet_demo_pose_.load() + 1u) % 4u;
                    kid_app::Emotion emotion = kid_app::Emotion::Neutral;
                    switch (value) {
                        case 1: emotion = kid_app::Emotion::Happy; break;
                        case 2: emotion = kid_app::Emotion::Curious; break;
                        case 3: emotion = kid_app::Emotion::Sleepy; break;
                        default: emotion = kid_app::Emotion::Neutral; break;
                    }
                    if (self->display_->ShowPetPreview(emotion)) {
                        self->pet_demo_pose_.store(value);
                        self->pet_demo_mode_.store(true);
                        ESP_LOGI(TAG, "PET_PREVIEW_ON pose=%u", value);
                    } else {
                        ESP_LOGW(TAG, "PET_PREVIEW_FAILED");
                    }
                }
                self->pet_demo_busy_.store(false);
                vTaskDelete(nullptr);
            }, "pet_demo", 6144, job, 2, nullptr);
        if (result != pdPASS) {
            delete job;
            pet_demo_busy_.store(false);
            ESP_LOGE(TAG, "Could not launch pet display task");
        }
    }
'''


def patch_board(text: str) -> str:
    text = once(text, "#include <stdio.h>", "#include <stdio.h>\n#include <atomic>\n#include <new>", "board headers")
    text = once(text, "    void InitializeButtons() {", BOARD_WORKER.rstrip() + "\n\n    void InitializeButtons() {", "board worker")
    text = once(text, "            app.ToggleChatState();",
        """            if (pet_demo_mode_.load()) {
                RequestPetPreview(false); // BOOT click cycles pet expressions.
                return;
            }
            app.ToggleChatState();""",
        "board click hook")
    text = once(text, "        pwr_button_.OnClick([this]() {",
        """        // Hold BOOT while idle to enter/leave pet preview; otherwise normal
        // XiaoZhi voice click and power button retain their behavior.
        boot_button_.OnLongPress([this]() { RequestPetPreview(true); });

        pwr_button_.OnClick([this]() {""", "board long press")
    return text


def run(source: Path) -> None:
    git_head = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    if git_head != MANIFEST["firmware"]["commit"]:
        raise RuntimeError(f"Wrong firmware commit: {git_head}")
    dest = source / "components" / "kid_app"
    if dest.exists():
        raise RuntimeError(f"Target component already exists: {dest}")

    cmake = source / "main/CMakeLists.txt"
    header = source / BOARD_REL / "custom_lcd_display.h"
    impl = source / BOARD_REL / "custom_lcd_display.cc"
    board = source / BOARD_REL / "waveshare-s3-epaper-3.97.cc"
    # Calculate all changes before writing anything so an anchor mismatch
    # cannot leave half a patched source tree.
    patches = {
        cmake: patch_main_cmake(cmake.read_text(encoding="utf-8")),
        header: patch_display_h(header.read_text(encoding="utf-8")),
        impl: patch_display_cc(impl.read_text(encoding="utf-8")),
        board: patch_board(board.read_text(encoding="utf-8")),
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
