#include "kid_app/freertos_story_pcm_worker.h"

#include "esp_log.h"
#include <algorithm>
#include <array>
#include <cstring>

namespace kid_app {
namespace {
constexpr char kTag[] = "kid_story_audio";
constexpr uint32_t kAudioStackBytes = 6144;
constexpr size_t kFrameSamples = 480; // 20ms at 24kHz.
}

bool FreertosStoryPcmWorker::Start(
        const char* book_dir, const char* book_id,
        StoryPcmHooks hooks, void* context) {
    if (task_ || queue_ || !hooks.begin || !hooks.push || !hooks.cancel || !book_dir ||
        std::strlen(book_dir) >= sizeof(book_dir_))
        return false;
    std::strcpy(book_dir_, book_dir);
    // V9: no book yet is fine (the shelf picks one later via RequestBook).
    const bool with_book = book_id && *book_id;
    if (with_book) {
        if (!file_.OpenBook(book_dir, book_id)) return false;
        if (!reader_.Open(file_)) {
            file_.Close();
            return false;
        }
    }
    queue_ = xQueueCreate(1, sizeof(Request));
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

bool FreertosStoryPcmWorker::RequestBook(const char* book_id) {
    if (!queue_ || !book_id || !*book_id || std::strlen(book_id) >= sizeof(pending_book_))
        return false;
    pending_seq_.fetch_add(1);  // odd: being written
    std::strcpy(pending_book_, book_id);
    pending_seq_.fetch_add(1);  // even: complete
    book_pending_.store(true);
    // Wake the task with a stop; the switch is applied before any request.
    Request request{};
    return xQueueOverwrite(queue_, &request) == pdPASS;
}

bool FreertosStoryPcmWorker::RequestPage(uint16_t page) {
    return RequestClip(StoryClip::Page, page);
}

bool FreertosStoryPcmWorker::RequestClip(StoryClip clip, uint16_t page,
                                         uint8_t question, uint8_t option) {
    Request request{};
    request.page = page;
    request.clip = clip;
    request.question = question;
    request.option = option;
    return queue_ && xQueueOverwrite(queue_, &request) == pdPASS;
}

void FreertosStoryPcmWorker::TaskEntry(void* context) {
    static_cast<FreertosStoryPcmWorker*>(context)->Run();
}

uint32_t FreertosStoryPcmWorker::ClipBytes(const Request& request,
                                           ContentQuestion* question) {
    if (request.clip == StoryClip::Page) {
        StoryPageInfo info{};
        if (!reader_.Page(request.page, &info) || info.sample_rate != 24000 ||
            info.channels != 1 || info.bits != 16)
            return 0;
        return info.pcm_bytes;
    }
    // KPB2 prompts and option names are 24 kHz mono PCM16 by format (V4-04).
    if (!reader_.Content().Question(request.page, request.question, question))
        return 0;
    if (request.clip == StoryClip::Prompt) return question->prompt_pcm.bytes;
    if (request.option >= question->option_count) return 0;
    return question->options[request.option].pcm.bytes;
}

bool FreertosStoryPcmWorker::ReadClip(const Request& request,
                                      const ContentQuestion& question,
                                      uint32_t offset, void* target, size_t bytes) {
    switch (request.clip) {
        case StoryClip::Page:
            return reader_.ReadPcm(request.page, offset, target, bytes);
        case StoryClip::Prompt:
            return reader_.Content().ReadPromptPcm(question, offset, target, bytes);
        case StoryClip::Option:
            return reader_.Content().ReadOptionPcm(question, request.option, offset,
                                                   target, bytes);
    }
    return false;
}

void FreertosStoryPcmWorker::Run() {
    Request request{};
    Request queued{};
    bool preloaded = false;
    std::array<int16_t,kFrameSamples> samples{};
    for (;;) {
        if (preloaded) {
            request = queued;
            preloaded = false;
        } else if (xQueueReceive(queue_,&request,portMAX_DELAY) != pdPASS) {
            continue;
        }
        // Invalidate the previous clip's queued buffers in AudioService.
        hooks_.cancel(context_);
        if (book_pending_.exchange(false)) {  // V9 bookshelf: another book
            char name[sizeof(pending_book_)] = {};
            for (unsigned tries = 0; tries < 50; ++tries) {  // a consistent copy
                const uint32_t before = pending_seq_.load();
                if (before & 1u) {
                    vTaskDelay(1);
                    continue;
                }
                std::memcpy(name, pending_book_, sizeof(name));
                name[sizeof(name) - 1] = '\0';
                if (pending_seq_.load() == before) break;
                name[0] = '\0';
            }
            if (!name[0] || !file_.OpenBook(book_dir_, name) || !reader_.Open(file_)) {
                file_.Close();
                reader_.Content().Close();
                ESP_LOGW(kTag, "Story audio book unavailable: %s", name);
            }
        }
        if (!request.page) continue;
        ContentQuestion question{};
        const uint32_t total = ClipBytes(request, &question);
        if (!total) {
            ESP_LOGW(kTag, "Unsupported story audio clip");
            continue;
        }
        if (++next_token_ == 0) ++next_token_;
        const uint64_t token = next_token_;
        // A previous clip may still have up to two queued 20 ms frames.
        // Wait for the single audio output task to drain them. Never lose
        // the newly requested clip simply because Begin was momentarily busy.
        bool begun = false;
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            if (xQueueReceive(queue_, &queued, 0) == pdPASS) {
                preloaded = true;
                break;  // A newer clip/stop always wins.
            }
            if (hooks_.begin(token, context_)) {
                begun = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (!begun) continue;
        uint32_t offset = 0;
        unsigned failed = 0;
        bool read_error = false;
        while (offset < total) {
            // Prioritize the latest requested clip or stop immediately.
            if (xQueueReceive(queue_,&queued,0) == pdPASS) {
                preloaded = true;
                break;
            }
            const size_t bytes = std::min<size_t>(
                samples.size() * sizeof(int16_t),total-offset);
            if (!ReadClip(request,question,offset,samples.data(),bytes)) {
                read_error = true;
                break;
            }
            if (!hooks_.push(token,samples.data(),bytes/sizeof(int16_t),context_)) {
                if (++failed >= 150) { // cease after <=1.5s of busy/voice.
                    ESP_LOGW(kTag,"Audio queue stayed unavailable; canceling clip");
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
        // frames to drain. A new clip, stop, or voice state will cancel.
        if (read_error) hooks_.cancel(context_);
    }
}

}  // namespace kid_app
