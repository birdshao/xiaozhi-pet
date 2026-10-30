#pragma once

#include "kid_app/story_pack_reader.h"

#include <cstdio>
#include <cstdint>

namespace kid_app {

// Read-only file adapter for an ALREADY MOUNTED ESP VFS/FAT SD card.
// This class deliberately does not assume SD SPI pins, mount or AudioPort.
// Accepts only ASCII book IDs (not arbitrary relative paths).
class StoryPackFilePort final : public StoryPackFile {
public:
    ~StoryPackFilePort() override { Close(); }
    StoryPackFilePort() = default;
    StoryPackFilePort(const StoryPackFilePort&) = delete;
    StoryPackFilePort& operator=(const StoryPackFilePort&) = delete;

    bool OpenBook(const char* mounted_books_directory, const char* book_id);
    void Close();
    uint64_t Size() const override { return size_; }
    bool ReadAt(uint32_t offset, void* out, size_t size) override;

private:
    FILE* file_ = nullptr;
    uint64_t size_ = 0;
};

}  // namespace kid_app
