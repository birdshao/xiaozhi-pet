#pragma once

#include "kid_app/freertos_story_pcm_worker.h"
#include "kid_app/melody_synth.h"
#include "kid_app/story_pack_file_port.h"
#include "kid_app/story_pack_reader.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace kid_app {

// V8-02: streams the pages of a song (an ordinary KPB book, see
// song_player.h) through the same single AudioService PCM queue as the story
// narration (StoryPcmHooks). One low-priority task, created once; songs are
// opened and closed on request (a different book each time, unlike
// FreertosStoryPcmWorker which is bound to one book). The task only reads the
// TF card and pushes PCM; it never touches the display, ES8311 or I2S.
//
// Commands are last-wins (a queue of one) and answered by one pending event
// the board polls (pet_input): Opened(pages), OpenFailed, PageDone(page).
class FreertosSongWorker {
public:
    enum class Event : uint8_t { None, Opened, OpenFailed, PageDone };

    bool Start(const char* book_dir, StoryPcmHooks hooks, void* context);
    bool Ready() const { return task_ != nullptr; }
    // Opens <book_dir>/<id>.kpb (CRC-checks every page: can take seconds).
    bool Open(const char* id);
    // V9-3A: "opens" built-in melody `index` (melody_synth.h BuiltinMelodyAt):
    // nothing is read from the card, the book has one page (page 1) that is
    // synthesised and streamed through the same PCM queue.
    bool OpenBuiltin(uint8_t index);
    // Streams one page (1-based) of the open book; the previous page is cancelled.
    bool Play(uint16_t page);
    // Cancels the audio, keeps the book open.
    bool Stop();
    // Cancels the audio and closes the book.
    bool Close();
    // Pops the pending event; `value` is the page count (Opened) or the page.
    Event TakeEvent(uint16_t* value);

private:
    enum class Cmd : uint8_t { Open, Play, Stop, Close };
    struct Request {
        Cmd cmd = Cmd::Stop;
        uint16_t page = 0;
        uint8_t builtin = 0;  // Open: built-in melody index + 1, 0 = a card book
        char id[48] = {};
    };

    static void TaskEntry(void* ctx);
    void Run();
    void Post(Event event, uint16_t value);
    bool Send(const Request& request);

    char book_dir_[48] = {};
    StoryPackFilePort file_{};
    StoryPackReader reader_{};
    bool open_ = false;
    uint8_t builtin_ = 0;  // task only: open built-in melody + 1, 0 = a card book
    MelodySynth synth_{};
    StoryPcmHooks hooks_{};
    void* context_ = nullptr;
    QueueHandle_t queue_ = nullptr;
    TaskHandle_t task_ = nullptr;
    uint64_t next_token_ = 0x53474E00u;  // "SGN" tokens: never collide with story / prompt sessions
    std::atomic<uint32_t> event_{0};     // (event << 16 | value) + 1, 0 = none
};

}  // namespace kid_app
