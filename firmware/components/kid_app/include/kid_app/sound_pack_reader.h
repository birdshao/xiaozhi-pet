#pragma once

#include "kid_app/story_pack_reader.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace kid_app {

struct SoundClipInfo {
    uint16_t id = 0;
    uint32_t offset = 0, bytes = 0, crc = 0;
    size_t Samples() const { return bytes / 2; }
};

// KPS1: reviewed sound pack on the TF card (V2-08). Option names and pet
// phrases as 24 kHz mono PCM16 clips, keyed by the clip ids of
// tools/sound_pack.py (1..5 option prompts, 100..999 Personality phrase
// ids). Separate from the KPB story books; it shares only the read-only
// StoryPackFile port. Layout: docs/V2_SOUND_PACK.md.
class SoundPackReader {
public:
    static constexpr uint32_t kSampleRate = 24000;
    static constexpr size_t kMaxClips = 96;
    static constexpr uint32_t kMaxClipBytes = 6u * kSampleRate * 2u;  // 6 s
    static constexpr uint32_t kMaxBytes = 8u * 1024u * 1024u;
    static constexpr uint32_t kHeaderBytes = 32;
    static constexpr uint32_t kRecordBytes = 16;

    // Checks the header, table and every clip CRC (512-byte chunks) before
    // the pack counts as ready: a damaged pack is rejected as a whole.
    // Corruption detection only, not a signature. Not for an audio callback.
    bool Open(StoryPackFile& file);
    void Close();
    bool Ready() const { return file_ != nullptr; }
    uint16_t ClipCount() const { return count_; }
    uint32_t Revision() const { return revision_; }
    bool Find(uint16_t id, SoundClipInfo* out) const;
    // Signed little-endian PCM16; offset and length are 2-byte aligned.
    bool ReadPcm(const SoundClipInfo& clip, uint32_t byte_offset, void* target,
                 size_t bytes);

    static uint32_t Crc32(const uint8_t* data, size_t bytes, uint32_t prior = 0);

private:
    bool Verified(StoryPackFile& file, const SoundClipInfo& clip) const;

    StoryPackFile* file_ = nullptr;
    uint16_t count_ = 0;
    uint32_t revision_ = 0;
    std::array<SoundClipInfo, kMaxClips> clips_{};
};

// Read-only adapter for <mounted dir>/<pack_id>.kps on an already mounted
// FAT volume. Same id rule as story books: [a-z0-9][a-z0-9_-]{2,62}.
class SoundPackFilePort final : public StoryPackFile {
public:
    SoundPackFilePort() = default;
    ~SoundPackFilePort() override { Close(); }
    SoundPackFilePort(const SoundPackFilePort&) = delete;
    SoundPackFilePort& operator=(const SoundPackFilePort&) = delete;

    // extension: "kps" (sound pack) or "kpg" (V4-06 game pack).
    bool Open(const char* mounted_directory, const char* pack_id, const char* extension = "kps");
    void Close();
    uint64_t Size() const override { return size_; }
    bool ReadAt(uint32_t offset, void* out, size_t size) override;

private:
    FILE* file_ = nullptr;
    uint64_t size_ = 0;
};

}  // namespace kid_app
