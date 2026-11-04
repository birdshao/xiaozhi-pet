#pragma once

#include "kid_app/content_pack_reader.h"

#include <cstddef>
#include <cstdint>

namespace kid_app {

class MonoFrame;

struct StoryPageInfo {
    uint32_t index = 0;
    uint32_t image_offset = 0, image_bytes = 0, image_crc = 0;
    uint32_t pcm_offset = 0, pcm_bytes = 0, pcm_crc = 0;
    uint32_t sample_rate = 0;
    uint16_t channels = 0, bits = 0;
    uint32_t text_offset = 0, text_bytes = 0, text_crc = 0;
};

// Offline picture book on SD: the P4 1-bit page + narration view used by the
// board's Reader and PCM worker. Opens KPB1 cards and KPB2 books (whose
// pages then show their 1-bit fallback frame). The host owns SD mount,
// AudioPort, PSRAM and EPD updates. Do not call Open() or other file IO
// from an audio callback / ISR.
class StoryPackReader {
public:
    static constexpr uint32_t kWidth = ContentPackReader::kWidth;
    static constexpr uint32_t kHeight = ContentPackReader::kHeight;
    static constexpr uint32_t kImageBytes = ContentPackReader::kMonoBytes;
    static constexpr uint32_t kMaxBytes = ContentPackReader::kMaxBytes;
    static constexpr size_t kMaxPages = ContentPackReader::kMaxPages;

    // CRC verification scans every asset, in fixed 512-byte chunks, before
    // marking the package available. This is corruption detection, NOT
    // cryptographic signature verification or parental authorization.
    bool Open(StoryPackFile& file) { return content_.Open(file); }
    bool Ready() const { return content_.Ready(); }
    uint16_t PageCount() const { return content_.PageCount(); }
    bool Page(uint16_t number, StoryPageInfo* out) const;

    // Caller provides PSRAM-allocated (or other valid) 48 KiB destination.
    bool ReadImage(uint16_t page, uint8_t* target, size_t capacity) {
        return content_.ReadMono(page, target, capacity);
    }
    // Direct EPD frame import: caller owns MonoFrame in PSRAM.
    bool ReadFrame(uint16_t page, MonoFrame& frame);
    // Streams packed signed little-endian PCM16 audio in bounded blocks.
    // byte_offset and length must be 2-byte aligned, as samples are 16-bit.
    bool ReadPcm(uint16_t page, uint32_t byte_offset, void* target,
                 size_t bytes) {
        return content_.ReadPagePcm(page, byte_offset, target, bytes);
    }
    // UTF-8 read only for optional host diagnostics; call with enough buffer.
    bool ReadText(uint16_t page, char* target, size_t capacity) {
        return content_.ReadPageText(page, target, capacity);
    }

    // V4-05: grey pages, questions and title of the same validated package.
    ContentPackReader& Content() { return content_; }
    const ContentPackReader& Content() const { return content_; }

private:
    ContentPackReader content_{};
};

}  // namespace kid_app
