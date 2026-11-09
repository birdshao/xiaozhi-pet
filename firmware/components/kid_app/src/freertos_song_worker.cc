#include "kid_app/freertos_song_worker.h"

#include "esp_log.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace kid_app {
namespace {
constexpr char kTag[] = "kid_song_audio";
constexpr uint32_t kStackBytes = 8192;  // the synthesiser does a little floating point
constexpr size_t kFrameSamples = 480;  // 20 ms at 24 kHz
}  // namespace

bool FreertosSongWorker::Start(const char* book_dir, StoryPcmHooks hooks, void* context) {
    if (task_ || queue_ || !book_dir || !hooks.begin || !hooks.push || !hooks.cancel) return false;
    const size_t n = std::strlen(book_dir);
    if (n == 0 || n >= sizeof(book_dir_)) return false;
    std::memcpy(book_dir_, book_dir, n + 1);
    hooks_ = hooks;
    context_ = context;
    queue_ = xQueueCreate(1, sizeof(Request));
    if (!queue_) return false;
    if (xTaskCreate(&FreertosSongWorker::TaskEntry, "kid_song_pcm", kStackBytes, this, 2,
                    &task_) != pdPASS) {
        vQueueDelete(queue_);
        queue_ = nullptr;
        task_ = nullptr;
        return false;
    }
    return true;
}

bool FreertosSongWorker::Send(const Request& request) {
    return queue_ && xQueueOverwrite(queue_, &request) == pdPASS;
}

bool FreertosSongWorker::Open(const char* id) {
    if (!id || std::strlen(id) >= sizeof(Request::id)) return false;
    Request request{};
    request.cmd = Cmd::Open;
    std::strncpy(request.id, id, sizeof(request.id) - 1);
    event_.store(0);
    return Send(request);
}

bool FreertosSongWorker::OpenBuiltin(uint8_t index) {
    if (index >= BuiltinMelodyCount()) return false;
    Request request{};
    request.cmd = Cmd::Open;
    request.builtin = static_cast<uint8_t>(index + 1);
    event_.store(0);
    return Send(request);
}

bool FreertosSongWorker::Play(uint16_t page) {
    Request request{};
    request.cmd = Cmd::Play;
    request.page = page;
    return Send(request);
}

bool FreertosSongWorker::Stop() {
    Request request{};
    request.cmd = Cmd::Stop;
    return Send(request);
}

bool FreertosSongWorker::Close() {
    Request request{};
    request.cmd = Cmd::Close;
    return Send(request);
}

FreertosSongWorker::Event FreertosSongWorker::TakeEvent(uint16_t* value) {
    const uint32_t word = event_.exchange(0);
    if (!word) return Event::None;
    const uint32_t raw = word - 1;
    if (value) *value = static_cast<uint16_t>(raw & 0xFFFF);
    return static_cast<Event>(raw >> 16);
}

void FreertosSongWorker::Post(Event event, uint16_t value) {
    event_.store(((static_cast<uint32_t>(event) << 16) | value) + 1);
}

void FreertosSongWorker::TaskEntry(void* context) {
    static_cast<FreertosSongWorker*>(context)->Run();
}

void FreertosSongWorker::Run() {
    Request request{};
    Request queued{};
    bool preloaded = false;
    std::array<int16_t, kFrameSamples> samples{};
    for (;;) {
        if (preloaded) {
            request = queued;
            preloaded = false;
        } else if (xQueueReceive(queue_, &request, portMAX_DELAY) != pdPASS) {
            continue;
        }
        // Whatever the command, the previous page's queued audio is invalid.
        hooks_.cancel(context_);
        if (request.cmd == Cmd::Stop) continue;
        if (request.cmd == Cmd::Close) {
            if (open_) file_.Close();
            open_ = false;
            builtin_ = 0;
            continue;
        }
        if (request.cmd == Cmd::Open) {
            if (open_) file_.Close();
            open_ = false;
            builtin_ = 0;
            if (request.builtin) {  // synthesised: one page, nothing to read
                builtin_ = request.builtin;
                Post(Event::Opened, 1);
                continue;
            }
            if (file_.OpenBook(book_dir_, request.id) && reader_.Open(file_)) {
                open_ = true;
                Post(Event::Opened, reader_.PageCount());
            } else {
                file_.Close();
                Post(Event::OpenFailed, 0);
                ESP_LOGW(kTag, "SONG_OPEN_FAILED id=%.40s", request.id);
            }
            continue;
        }
        // Cmd::Play
        StoryPageInfo info{};
        bool playable;
        if (builtin_) {
            const BuiltinMelody& tune = BuiltinMelodyAt(builtin_ - 1);
            // Full level: the board's song volume (fade, quiet hours) scales it.
            playable = request.page == 1 && synth_.Start(tune.melody, tune.timbre, 100);
            info.pcm_bytes = static_cast<uint32_t>(MelodySynth::TotalSamples(tune.melody) * sizeof(int16_t));
        } else {
            playable = open_ && request.page && reader_.Page(request.page, &info) &&
                       info.sample_rate == 24000 && info.channels == 1 && info.bits == 16 &&
                       info.pcm_bytes != 0;
        }
        if (!playable || info.pcm_bytes == 0) {
            ESP_LOGW(kTag, "SONG_PAGE_UNPLAYABLE page=%u", static_cast<unsigned>(request.page));
            Post(Event::PageDone, request.page);  // skip it: never stuck on a bad page
            continue;
        }
        if (++next_token_ == 0) ++next_token_;
        const uint64_t token = next_token_;
        bool begun = false;
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            if (xQueueReceive(queue_, &queued, 0) == pdPASS) {
                preloaded = true;
                break;  // a newer command always wins
            }
            if (hooks_.begin(token, context_)) {
                begun = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (!begun) continue;
        const uint32_t total = info.pcm_bytes;
        uint32_t offset = 0;
        unsigned failed = 0;
        bool aborted = false;
        bool rendered = false;  // synthesised chunk waiting for a free queue slot
        while (offset < total) {
            if (xQueueReceive(queue_, &queued, 0) == pdPASS) {
                preloaded = true;
                aborted = true;
                break;
            }
            const size_t bytes = std::min<size_t>(samples.size() * sizeof(int16_t), total - offset);
            bool got;
            if (builtin_) {  // sequential; a retry after a full queue reuses the chunk
                got = rendered || synth_.Render(samples.data(), bytes / sizeof(int16_t)) ==
                                      bytes / sizeof(int16_t);
                rendered = got;
            } else {
                got = reader_.ReadPcm(request.page, offset, samples.data(), bytes);
            }
            if (!got) {
                hooks_.cancel(context_);
                ESP_LOGW(kTag, "SONG_READ_FAILED page=%u", static_cast<unsigned>(request.page));
                aborted = true;
                Post(Event::PageDone, request.page);  // move on rather than hang
                break;
            }
            if (!hooks_.push(token, samples.data(), bytes / sizeof(int16_t), context_)) {
                if (++failed >= 150) {  // <= 1.5 s of busy / voice: give up this page
                    ESP_LOGW(kTag, "SONG_QUEUE_STUCK page=%u", static_cast<unsigned>(request.page));
                    hooks_.cancel(context_);
                    aborted = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            failed = 0;
            rendered = false;
            offset += static_cast<uint32_t>(bytes);
        }
        if (!aborted) Post(Event::PageDone, request.page);
    }
}

}  // namespace kid_app
