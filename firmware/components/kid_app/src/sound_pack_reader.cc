#include "kid_app/sound_pack_reader.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace kid_app {
namespace {
// Table CRC-32 (zlib polynomial): a full pack is checked at open, so the
// bitwise loop of the story reader would be ~8x slower on the ESP32-S3.
constexpr std::array<uint32_t, 256> MakeCrcTable() {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ ((c & 1u) ? 0xEDB88320u : 0u);
        table[i] = c;
    }
    return table;
}
constexpr std::array<uint32_t, 256> kCrcTable = MakeCrcTable();

uint16_t U16(const uint8_t* d) { return static_cast<uint16_t>(d[0] | (d[1] << 8)); }
uint32_t U32(const uint8_t* d) {
    return uint32_t(d[0]) | (uint32_t(d[1]) << 8) | (uint32_t(d[2]) << 16) |
           (uint32_t(d[3]) << 24);
}
uint32_t Align4(uint32_t v) { return (v + 3u) & ~3u; }

bool ValidPackId(const char* value) {
    if (!value) return false;
    size_t length = 0;
    for (; length < 64 && value[length]; ++length) {
        const char c = value[length];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              (length > 0 && (c == '_' || c == '-'))))
            return false;
    }
    return length >= 3 && length < 64;
}
}  // namespace

uint32_t SoundPackReader::Crc32(const uint8_t* data, size_t bytes, uint32_t prior) {
    uint32_t crc = ~prior;
    for (size_t i = 0; i < bytes; ++i) crc = kCrcTable[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

bool SoundPackReader::Verified(StoryPackFile& file, const SoundClipInfo& clip) const {
    std::array<uint8_t, 512> chunk{};
    uint32_t crc = 0, done = 0;
    while (done < clip.bytes) {
        const size_t n = std::min<size_t>(chunk.size(), clip.bytes - done);
        if (!file.ReadAt(clip.offset + done, chunk.data(), n)) return false;
        crc = Crc32(chunk.data(), n, crc);
        done += static_cast<uint32_t>(n);
    }
    return crc == clip.crc;
}

void SoundPackReader::Close() {
    file_ = nullptr;
    count_ = 0;
    revision_ = 0;
    clips_ = {};
}

bool SoundPackReader::Open(StoryPackFile& file) {
    Close();
    std::array<uint8_t, kHeaderBytes> head{};
    if (file.Size() < head.size() || file.Size() > kMaxBytes ||
        !file.ReadAt(0, head.data(), head.size()))
        return false;
    if (std::memcmp(head.data(), "KPS1", 4) != 0 || U16(head.data() + 4) != 1 ||
        U32(head.data() + 8) != kSampleRate || Crc32(head.data(), 28) != U32(head.data() + 28))
        return false;
    const uint16_t count = U16(head.data() + 6);
    const uint32_t data_offset = U32(head.data() + 16);
    const uint32_t total = U32(head.data() + 20);
    if (count == 0 || count > kMaxClips || data_offset != kHeaderBytes + count * kRecordBytes ||
        total != file.Size() || total < data_offset)
        return false;

    std::array<SoundClipInfo, kMaxClips> candidate{};
    uint32_t table_crc = 0, previous_end = data_offset;
    for (uint32_t i = 0; i < count; ++i) {
        std::array<uint8_t, kRecordBytes> r{};
        if (!file.ReadAt(kHeaderBytes + i * kRecordBytes, r.data(), r.size())) return false;
        table_crc = Crc32(r.data(), r.size(), table_crc);
        SoundClipInfo c{};
        c.id = U16(r.data());
        c.offset = U32(r.data() + 4);
        c.bytes = U32(r.data() + 8);
        c.crc = U32(r.data() + 12);
        // Ids ascend (unique, binary-searchable); clips follow each other
        // 4-byte aligned with nothing hidden in between or after.
        if (c.id == 0 || c.id > 999 || (i > 0 && c.id <= candidate[i - 1].id) ||
            U16(r.data() + 2) != 0 || c.bytes < 2 || c.bytes > kMaxClipBytes ||
            (c.bytes & 1u) || c.offset != Align4(previous_end) ||
            uint64_t(c.offset) + c.bytes > total)
            return false;
        candidate[i] = c;
        previous_end = c.offset + c.bytes;
    }
    if (previous_end != total || table_crc != U32(head.data() + 24)) return false;
    // No clip is trusted until every byte of it was checked.
    for (uint32_t i = 0; i < count; ++i)
        if (!Verified(file, candidate[i])) return false;
    clips_ = candidate;
    count_ = count;
    revision_ = U32(head.data() + 12);
    file_ = &file;
    return true;
}

bool SoundPackReader::Find(uint16_t id, SoundClipInfo* out) const {
    if (!Ready() || !out) return false;
    const auto end = clips_.begin() + count_;
    const auto it = std::lower_bound(clips_.begin(), end, id,
                                     [](const SoundClipInfo& c, uint16_t v) { return c.id < v; });
    if (it == end || it->id != id) return false;
    *out = *it;
    return true;
}

bool SoundPackReader::ReadPcm(const SoundClipInfo& clip, uint32_t position, void* target,
                              size_t bytes) {
    if (!Ready() || !target || bytes == 0 || (position & 1u) || (bytes & 1u) ||
        uint64_t(position) + bytes > clip.bytes)
        return false;
    return file_->ReadAt(clip.offset + position, target, bytes);
}

bool SoundPackFilePort::Open(const char* base, const char* pack_id, const char* extension) {
    Close();
    if (!base || !ValidPackId(pack_id) || !extension ||
        (std::strcmp(extension, "kps") != 0 && std::strcmp(extension, "kpg") != 0))
        return false;
    const size_t root_len = std::strlen(base);
    if (root_len < 2 || root_len > 150 || base[0] != '/' || std::strstr(base, "..") ||
        std::strchr(base, '\\'))
        return false;
    char full[256]{};
    const int n = std::snprintf(full, sizeof(full), "%s/%s.%s", base, pack_id, extension);
    if (n < 1 || static_cast<size_t>(n) >= sizeof(full)) return false;
    FILE* fp = std::fopen(full, "rb");
    if (!fp) return false;
    if (std::fseek(fp, 0, SEEK_END) != 0) {
        std::fclose(fp);
        return false;
    }
    const long size = std::ftell(fp);
    if (size < static_cast<long>(SoundPackReader::kHeaderBytes) ||
        static_cast<uint64_t>(size) > SoundPackReader::kMaxBytes) {
        std::fclose(fp);
        return false;
    }
    file_ = fp;
    size_ = static_cast<uint64_t>(size);
    return true;
}

void SoundPackFilePort::Close() {
    if (file_) std::fclose(file_);
    file_ = nullptr;
    size_ = 0;
}

bool SoundPackFilePort::ReadAt(uint32_t offset, void* out, size_t count) {
    if (!file_ || !out || count == 0 || uint64_t(offset) + count > size_ ||
        offset > static_cast<uint32_t>(std::numeric_limits<long>::max()))
        return false;
    return std::fseek(file_, static_cast<long>(offset), SEEK_SET) == 0 &&
           std::fread(out, 1, count, file_) == count;
}

}  // namespace kid_app
