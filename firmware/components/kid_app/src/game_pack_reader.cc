#include "kid_app/game_pack_reader.h"

#include <algorithm>
#include <cstring>

namespace kid_app {
namespace {
uint16_t U16(const uint8_t* d) { return static_cast<uint16_t>(d[0] | (d[1] << 8)); }
uint32_t U32(const uint8_t* d) {
    return uint32_t(d[0]) | (uint32_t(d[1]) << 8) | (uint32_t(d[2]) << 16) |
           (uint32_t(d[3]) << 24);
}
uint32_t Align4(uint32_t v) { return (v + 3u) & ~3u; }

bool SessionClip(uint16_t id) { return id >= kGameClipRight && id <= kGameClipBye; }
bool RoundClip(uint16_t id) { return id >= kGameRoundClipMin && id <= kGameRoundClipMax; }

bool ClipVerified(StoryPackFile& file, const SoundClipInfo& clip) {
    std::array<uint8_t, 512> chunk{};
    uint32_t crc = 0, done = 0;
    while (done < clip.bytes) {
        const size_t n = std::min<size_t>(chunk.size(), clip.bytes - done);
        if (!file.ReadAt(clip.offset + done, chunk.data(), n)) return false;
        crc = SoundPackReader::Crc32(chunk.data(), n, crc);
        done += static_cast<uint32_t>(n);
    }
    return crc == clip.crc;
}
}  // namespace

void GamePackReader::Close() {
    file_ = nullptr;
    content_id_ = 0;
    revision_ = 0;
    round_count_ = 0;
    clip_count_ = 0;
    rounds_ = {};
    clips_ = {};
}

bool GamePackReader::Open(StoryPackFile& file) {
    Close();
    if (Load(file)) return true;
    Close();
    return false;
}

// Fills the tables in place (no 3 KiB of copies on the caller's stack);
// Open() clears them again on failure.
bool GamePackReader::Load(StoryPackFile& file) {
    std::array<uint8_t, kHeaderBytes> head{};
    if (file.Size() < head.size() || file.Size() > kMaxBytes ||
        !file.ReadAt(0, head.data(), head.size()))
        return false;
    if (std::memcmp(head.data(), "KPG1", 4) != 0 || U16(head.data() + 4) != 1 ||
        U32(head.data() + 16) != kSampleRate ||
        SoundPackReader::Crc32(head.data(), 28) != U32(head.data() + 28))
        return false;
    const uint16_t rounds = U16(head.data() + 6);
    const uint16_t clips = U16(head.data() + 8);
    const uint16_t content_id = U16(head.data() + 10);
    const uint32_t revision = U32(head.data() + 12);
    const uint32_t total = U32(head.data() + 20);
    if (rounds == 0 || rounds > kMaxRounds || clips == 0 || clips > kMaxClips ||
        content_id < kMinContentId || revision == 0 || total != file.Size())
        return false;
    const uint32_t clip_table = kHeaderBytes + rounds * kRoundBytes;
    const uint32_t data_offset = clip_table + clips * kClipBytes;
    if (total < data_offset) return false;

    // Rounds: same rules as tools/game_pack.py; round clips are 16..999.
    auto& round_table = rounds_;
    uint32_t table_crc = 0;
    for (uint32_t i = 0; i < rounds; ++i) {
        std::array<uint8_t, kRoundBytes> r{};
        if (!file.ReadAt(kHeaderBytes + i * kRoundBytes, r.data(), r.size())) return false;
        table_crc = SoundPackReader::Crc32(r.data(), r.size(), table_crc);
        GameRound g{};
        g.kind = static_cast<GameKind>(r[0]);
        g.option_count = r[1];
        if (r[2] == 0 || r[3] != 0 || U32(r.data() + 20) != 0) return false;
        g.correct = static_cast<uint8_t>(r[2] - 1);
        g.question_art = U16(r.data() + 4);
        for (int k = 0; k < 3; ++k) g.option_art[k] = U16(r.data() + 6 + 2 * k);
        g.prompt_clip = U16(r.data() + 12);
        for (int k = 0; k < 3; ++k) g.option_clip[k] = U16(r.data() + 14 + 2 * k);
        if (!GameKindValid(r[0]) || !GameRoundValid(g) || !RoundClip(g.prompt_clip)) return false;
        for (uint8_t k = 0; k < g.option_count; ++k)
            if (!RoundClip(g.option_clip[k])) return false;
        round_table[i] = g;
    }

    // Clips: ascending ids, back to back (4-byte aligned), nothing hidden.
    auto& clip_table_v = clips_;
    uint32_t previous_end = data_offset;
    for (uint32_t i = 0; i < clips; ++i) {
        std::array<uint8_t, kClipBytes> r{};
        if (!file.ReadAt(clip_table + i * kClipBytes, r.data(), r.size())) return false;
        table_crc = SoundPackReader::Crc32(r.data(), r.size(), table_crc);
        SoundClipInfo c{};
        c.id = U16(r.data());
        c.offset = U32(r.data() + 4);
        c.bytes = U32(r.data() + 8);
        c.crc = U32(r.data() + 12);
        if (!(SessionClip(c.id) || RoundClip(c.id)) ||
            (i > 0 && c.id <= clip_table_v[i - 1].id) || U16(r.data() + 2) != 0 ||
            c.bytes < 2 || c.bytes > kMaxClipBytes || (c.bytes & 1u) ||
            c.offset != Align4(previous_end) || uint64_t(c.offset) + c.bytes > total)
            return false;
        uint8_t pad[3] = {};
        const size_t gap = c.offset - previous_end;
        if (gap && (!file.ReadAt(previous_end, pad, gap) || pad[0] || pad[1] || pad[2]))
            return false;
        clip_table_v[i] = c;
        previous_end = c.offset + c.bytes;
    }
    if (previous_end != total || table_crc != U32(head.data() + 24)) return false;

    // Every round clip exists; every round-range clip is used by a round.
    auto has_clip = [&](uint16_t id) {
        const auto end = clip_table_v.begin() + clips;
        const auto it = std::lower_bound(clip_table_v.begin(), end, id,
                                         [](const SoundClipInfo& c, uint16_t v) { return c.id < v; });
        return it != end && it->id == id;
    };
    for (uint32_t i = 0; i < rounds; ++i) {
        const GameRound& g = round_table[i];
        if (!has_clip(g.prompt_clip)) return false;
        for (uint8_t k = 0; k < g.option_count; ++k)
            if (!has_clip(g.option_clip[k])) return false;
    }
    for (uint32_t i = 0; i < clips; ++i) {
        const uint16_t id = clip_table_v[i].id;
        if (!RoundClip(id)) continue;
        bool used = false;
        for (uint32_t j = 0; j < rounds && !used; ++j) {
            const GameRound& g = round_table[j];
            used = g.prompt_clip == id || g.option_clip[0] == id || g.option_clip[1] == id ||
                   g.option_clip[2] == id;
        }
        if (!used) return false;
    }
    // No clip is trusted until every byte of it was checked.
    for (uint32_t i = 0; i < clips; ++i)
        if (!ClipVerified(file, clip_table_v[i])) return false;

    round_count_ = rounds;
    clip_count_ = clips;
    content_id_ = content_id;
    revision_ = revision;
    file_ = &file;
    return true;
}

bool GamePackReader::FindClip(uint16_t id, SoundClipInfo* out) const {
    if (!Ready() || !out) return false;
    const auto end = clips_.begin() + clip_count_;
    const auto it = std::lower_bound(clips_.begin(), end, id,
                                     [](const SoundClipInfo& c, uint16_t v) { return c.id < v; });
    if (it == end || it->id != id) return false;
    *out = *it;
    return true;
}

bool GamePackReader::ReadPcm(const SoundClipInfo& clip, uint32_t position, void* target,
                             size_t bytes) {
    if (!Ready() || !target || bytes == 0 || (position & 1u) || (bytes & 1u) ||
        uint64_t(position) + bytes > clip.bytes)
        return false;
    return file_->ReadAt(clip.offset + position, target, bytes);
}

bool LoadGameClip(GamePackReader* pack, uint16_t id, GameClip* out) {
    if (!out || id == 0) return false;
    *out = GameClip{};
    out->id_ = id;
    if (pack && pack->FindClip(id, &out->info_)) {
        out->source = GameClip::Source::Pack;
        out->pack_ = pack;
        out->samples = out->info_.Samples();
        return true;
    }
    const size_t tone = GameToneSamples(id);
    if (!tone) return false;
    out->source = GameClip::Source::Tone;
    out->samples = tone;
    return true;
}

size_t GameClip::Copy(size_t offset, int16_t* out, size_t max) {
    if (!out || offset >= samples) return 0;
    const size_t n = std::min(max, samples - offset);
    switch (source) {
        case Source::Tone:
            return RenderGameTone(id_, offset, out, n);
        case Source::Pack:
            // KPG1 stores little-endian PCM16, the ESP32-S3's own order.
            return pack_ && pack_->ReadPcm(info_, static_cast<uint32_t>(offset * 2), out, n * 2)
                       ? n
                       : 0;
        case Source::None:
            break;
    }
    return 0;
}

const char* GameClipSourceName(GameClip::Source source) {
    switch (source) {
        case GameClip::Source::Pack: return "pack";
        case GameClip::Source::Tone: return "tone";
        case GameClip::Source::None: break;
    }
    return "none";
}

}  // namespace kid_app
