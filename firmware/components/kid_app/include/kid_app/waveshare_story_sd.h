#pragma once

#include "kid_app/story_pack_file_port.h"
#include "kid_app/story_pack_reader.h"

#include <cstdint>

#include "sdmmc_cmd.h"

namespace kid_app {

// Waveshare ESP32-S3-ePaper-3.97 official 4-bit SDMMC mapping.
// Call Initialize() only on the Pet mode worker, never in a button ISR or
// the EPD/audio callbacks. Missing SD or missing book does not block Pet mode.
class WaveshareStorySd {
public:
    WaveshareStorySd() = default;
    WaveshareStorySd(const WaveshareStorySd&) = delete;
    WaveshareStorySd& operator=(const WaveshareStorySd&) = delete;

    // Read-only app intent. Does not format a card or write any story files.
    bool Initialize(const char* book_id);
    bool Ready() const { return ready_; }
    StoryPackReader* Reader() { return ready_ ? &reader_ : nullptr; }

private:
    bool mounted_ = false;
    bool ready_ = false;
    sdmmc_card_t* card_ = nullptr;
    StoryPackFilePort file_{};
    StoryPackReader reader_{};
};

}  // namespace kid_app
