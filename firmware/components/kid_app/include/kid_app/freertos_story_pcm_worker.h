#pragma once

#include "kid_app/story_pack_file_port.h"
#include "kid_app/story_pack_reader.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <cstddef>
#include <cstdint>

namespace kid_app {

// Runs only on the dedicated low-priority SD audio task. The host supplies
// callbacks into the single existing XiaoZhi AudioService PCM output queue.
// Do not write ES8311, I2S or touch LCD/LVGL from this task.
struct StoryPcmHooks {
    bool (*begin)(uint64_t token, void* ctx) = nullptr;
    bool (*push)(uint64_t token, const int16_t* samples, size_t count, void* ctx) = nullptr;
    void (*cancel)(void* ctx) = nullptr;
};

class FreertosStoryPcmWorker {
public:
    bool Start(const char* book_dir, const char* book_id,
               StoryPcmHooks hooks, void* context);
    // Non-blocking last-wins page change. Zero means cancel/stop.
    bool RequestPage(uint16_t page);
    bool Stop() { return RequestPage(0); }
    bool Ready() const { return task_ != nullptr; }

private:
    static void TaskEntry(void* ctx);
    void Run();

    StoryPackFilePort file_{};
    StoryPackReader reader_{};
    StoryPcmHooks hooks_{};
    void* context_ = nullptr;
    QueueHandle_t queue_ = nullptr;
    TaskHandle_t task_ = nullptr;
    uint64_t next_token_ = 0;
};

}  // namespace kid_app
