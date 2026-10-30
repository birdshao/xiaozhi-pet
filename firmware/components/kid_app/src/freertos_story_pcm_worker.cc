#include "kid_app/freertos_story_pcm_worker.h"

#include "esp_log.h"
#include <algorithm>
#include <array>

namespace kid_app {
namespace {
constexpr char kTag[] = "kid_story_audio";
constexpr uint32_t kAudioStackBytes = 6144;
constexpr size_t kFrameSamples = 480; // 20ms at 24kHz.
}

bool FreertosStoryPcmWorker::Start(
        const char* book_dir, const char* book_id,
        StoryPcmHooks hooks, void* context) {
    if (task_ || queue_ || !hooks.begin || !hooks.push || !hooks.cancel ||
        !file_.OpenBook(book_dir, book_id)) return false;
    if (!reader_.Open(file_)) {
        file_.Close();
        return false;
    }
    queue_ = xQueueCreate(1, sizeof(uint16_t));
    if (!queue_) { file_.Close(); return false; }
    hooks_ = hooks;
    context_ = context;
    if (xTaskCreate(&FreertosStoryPcmWorker::TaskEntry, "kid_story_pcm",
                    kAudioStackBytes, this, 2, &task_) != pdPASS) {
        vQueueDelete(queue_);
        queue_ = nullptr;
        file_.Close();
        return false;
    }
    return true;
}

bool FreertosStoryPcmWorker::RequestPage(uint16_t page) {
    return queue_ && xQueueOverwrite(queue_, &page) == pdPASS;
}

void FreertosStoryPcmWorker::TaskEntry(void* context) {
    static_cast<FreertosStoryPcmWorker*>(context)->Run();
}

void FreertosStoryPcmWorker::Run() {
    uint16_t page = 0;
    uint16_t queued = 0;
    bool preloaded = false;
    std::array<int16_t,kFrameSamples> samples{};
    for (;;) {
        if (preloaded) {
            page = queued;
            preloaded = false;
        } else if (xQueueReceive(queue_,&page,portMAX_DELAY) != pdPASS) {
            continue;
        }
        // Invalidate the previous page's queued buffers in AudioService.
        hooks_.cancel(context_);
        if (!page) continue;
        StoryPageInfo info{};
        if (!reader_.Page(page,&info) || info.sample_rate != 24000 ||
            info.channels != 1 || info.bits != 16 || !info.pcm_bytes) {
            ESP_LOGW(kTag, "Unsupported story audio page");
            continue;
        }
        if (++next_token_ == 0) ++next_token_;
        const uint64_t token = next_token_;
        if (!hooks_.begin(token,context_)) continue;
        uint32_t offset = 0;
        unsigned failed = 0;
        bool read_error = false;
        while (offset < info.pcm_bytes) {
            // Prioritize the latest requested page or stop immediately.
            if (xQueueReceive(queue_,&queued,0) == pdPASS) {
                preloaded = true;
                break;
            }
            const size_t bytes = std::min<size_t>(
                samples.size() * sizeof(int16_t),info.pcm_bytes-offset);
            if (!reader_.ReadPcm(page,offset,samples.data(),bytes)) {
                read_error = true;
                break;
            }
            if (!hooks_.push(token,samples.data(),bytes/sizeof(int16_t),context_)) {
                if (++failed >= 150) { // cease after <=1.5s of busy/voice.
                    ESP_LOGW(kTag,"Audio queue stayed unavailable; canceling page");
                    read_error = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            failed = 0;
            offset += static_cast<uint32_t>(bytes);
        }
        // On natural completion, DO NOT cancel: allow final queued 20ms
        // frames to drain. A new page, stop, or voice state will cancel.
        if (read_error) hooks_.cancel(context_);
    }
}

}  // namespace kid_app
