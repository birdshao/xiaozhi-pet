#pragma once

#include "kid_app/story_pack_file_port.h"
#include "kid_app/story_pack_reader.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <atomic>
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

// V4-05: besides page narration, a question's prompt or one option's name
// from the same book (question and option are 0-based).
enum class StoryClip : uint8_t { Page, Prompt, Option };

class FreertosStoryPcmWorker {
public:
    bool Start(const char* book_dir, const char* book_id,
               StoryPcmHooks hooks, void* context);
    // V9 bookshelf: the next requests play from another book of the same
    // folder. Non-blocking; the worker task reopens it before its next
    // request (and stops what is playing). Start() may be given no book.
    bool RequestBook(const char* book_id);
    // Non-blocking last-wins page change. Zero means cancel/stop.
    bool RequestPage(uint16_t page);
    bool Stop() { return RequestPage(0); }
    // Non-blocking last-wins clip, like RequestPage (page 0 = stop).
    bool RequestClip(StoryClip clip, uint16_t page, uint8_t question = 0,
                     uint8_t option = 0);
    bool Ready() const { return task_ != nullptr; }

private:
    struct Request {
        uint16_t page = 0;
        StoryClip clip = StoryClip::Page;
        uint8_t question = 0;
        uint8_t option = 0;
    };

    static void TaskEntry(void* ctx);
    void Run();
    // Length of the clip in bytes (0 = not playable) and its reader.
    uint32_t ClipBytes(const Request& request, ContentQuestion* question);
    bool ReadClip(const Request& request, const ContentQuestion& question,
                  uint32_t offset, void* target, size_t bytes);

    StoryPackFilePort file_{};
    StoryPackReader reader_{};
    StoryPcmHooks hooks_{};
    void* context_ = nullptr;
    QueueHandle_t queue_ = nullptr;
    TaskHandle_t task_ = nullptr;
    uint64_t next_token_ = 0;
    char book_dir_[64]{};
    // RequestBook writer (odd sequence while writing) and the task reader.
    char pending_book_[64]{};
    std::atomic<uint32_t> pending_seq_{0};
    std::atomic<bool> book_pending_{false};
};

}  // namespace kid_app
