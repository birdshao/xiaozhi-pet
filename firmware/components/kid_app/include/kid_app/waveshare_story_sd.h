#pragma once

#include "kid_app/scene_model.h"
#include "kid_app/story_pack_file_port.h"
#include "kid_app/story_pack_reader.h"

#include <cstdint>


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
    // The card itself is mounted at /sdcard (also when no book is on it).
    bool Mounted() const { return mounted_; }
    StoryPackReader* Reader() { return ready_ ? &reader_ : nullptr; }
    // Why Initialize() did not produce a book: NoCard (mount failed),
    // NoBook (card mounted, no such .kpb) or BadBook (file does not
    // validate). None once Ready().
    BookMissing Missing() const { return missing_; }
    // V8 series: is the book with this content id on the card? Looked up by
    // the catalog's card name /books/c<content_id>.kpb (existence only).
    bool ContentBookPresent(uint16_t content_id) const;

private:
    bool mounted_ = false;
    bool ready_ = false;
    BookMissing missing_ = BookMissing::NoCard;
    StoryPackFilePort file_{};
    StoryPackReader reader_{};
};

}  // namespace kid_app
