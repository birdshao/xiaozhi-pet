#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace kid_app {

// KPB1: deterministic offline picture book. This is a read-only FAT/SD
// abstraction; the host owns SD mount, AudioPort, PSRAM and EPD updates.
// Do not call Open() or other file IO from an audio callback / ISR.
class StoryPackFile {
public:
    virtual ~StoryPackFile() = default;
    virtual uint64_t Size() const = 0;
    virtual bool ReadAt(uint32_t offset, void* out, size_t size) = 0;
};

struct StoryPageInfo {
    uint32_t index = 0;
    uint32_t image_offset = 0, image_bytes = 0, image_crc = 0;
    uint32_t pcm_offset = 0, pcm_bytes = 0, pcm_crc = 0;
    uint32_t sample_rate = 0;
    uint16_t channels = 0, bits = 0;
    uint32_t text_offset = 0, text_bytes = 0, text_crc = 0;
};

class StoryPackReader {
public:
    static constexpr uint32_t kWidth = 800;
    static constexpr uint32_t kHeight = 480;
    static constexpr uint32_t kImageBytes = kWidth * kHeight / 8;
    static constexpr uint32_t kMaxBytes = 64u * 1024u * 1024u;
    static constexpr size_t kMaxPages = 32;

    // CRC verification scans every asset, in fixed 512-byte chunks, before
    // marking the package available. This is corruption detection, NOT
    // cryptographic signature verification or parental authorization.
    bool Open(StoryPackFile& file);
    bool Ready() const { return file_ != nullptr; }
    uint16_t PageCount() const { return count_; }
    bool Page(uint16_t number, StoryPageInfo* out) const;

    // Caller provides PSRAM-allocated (or other valid) 48 KiB destination.
    bool ReadImage(uint16_t page, uint8_t* target, size_t capacity);
    // Streams packed signed little-endian PCM16 audio in bounded blocks.
    // byte_offset and length must be 2-byte aligned, as samples are 16-bit.
    bool ReadPcm(uint16_t page, uint32_t byte_offset,
                 void* target, size_t bytes);
    // UTF-8 read only for optional host diagnostics; call with enough buffer.
    bool ReadText(uint16_t page, char* target, size_t capacity);

private:
    static uint16_t U16(const uint8_t* data);
    static uint32_t U32(const uint8_t* data);
    static uint32_t Checksum(const uint8_t* data, size_t bytes);
    static uint32_t AdvanceChecksum(uint32_t prior, const uint8_t* data,
                                    size_t bytes);
    bool Verified(uint32_t offset, uint32_t bytes, uint32_t crc);

    StoryPackFile* file_ = nullptr;
    uint16_t count_ = 0;
    std::array<StoryPageInfo, kMaxPages> pages_{};
};

}  // namespace kid_app
