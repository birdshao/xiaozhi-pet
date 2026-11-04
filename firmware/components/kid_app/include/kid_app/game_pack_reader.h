#pragma once

#include "kid_app/mini_game.h"
#include "kid_app/sound_pack_reader.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace kid_app {

// KPG1: a reviewed mini-game pack on the TF card (V4-06,
// docs/V4_MINI_GAMES.md, tools/game_pack.py). A sibling of KPS1 with its own
// magic, not a KPB2 package: rounds (GameArt pictures, the right answer,
// clip ids) and 24 kHz mono PCM16 clips (questions, option names and the
// optional session lines 2..5). Opened once; the round table stays in RAM
// and clips are streamed from the card.
class GamePackReader {
public:
    static constexpr uint32_t kSampleRate = 24000;
    static constexpr size_t kMaxRounds = GameSession::kMaxPool;
    static constexpr size_t kMaxClips = 128;
    static constexpr uint32_t kMaxClipBytes = 10u * kSampleRate * 2u;  // 10 s
    static constexpr uint32_t kMaxBytes = SoundPackReader::kMaxBytes;   // 8 MiB
    static constexpr uint32_t kHeaderBytes = 32;
    static constexpr uint32_t kRoundBytes = 24;
    static constexpr uint32_t kClipBytes = 16;
    static constexpr uint16_t kMinContentId = 100;  // 1..99: built-in sets

    // Checks header, tables, every round rule and every clip CRC before the
    // pack counts as ready; anything wrong rejects the whole pack.
    // Corruption detection only, not a signature. Not for an audio callback.
    bool Open(StoryPackFile& file);
    void Close();
    bool Ready() const { return file_ != nullptr; }
    uint16_t ContentId() const { return content_id_; }
    uint32_t Revision() const { return revision_; }
    size_t RoundCount() const { return round_count_; }
    const GameRound* Rounds() const { return rounds_.data(); }
    size_t ClipCount() const { return clip_count_; }
    bool FindClip(uint16_t id, SoundClipInfo* out) const;
    // Signed little-endian PCM16; offset and length are 2-byte aligned.
    bool ReadPcm(const SoundClipInfo& clip, uint32_t byte_offset, void* target, size_t bytes);

private:
    bool Load(StoryPackFile& file);

    StoryPackFile* file_ = nullptr;
    uint16_t content_id_ = 0;
    uint32_t revision_ = 0;
    uint16_t round_count_ = 0;
    uint16_t clip_count_ = 0;
    std::array<GameRound, kMaxRounds> rounds_{};
    std::array<SoundClipInfo, kMaxClips> clips_{};
};

// One game sound, whatever its source: the pack's recording when it has
// one for this id, else the built-in tone (session lines and the built-in
// set). Same Copy() contract as SoundClip.
class GameClip {
public:
    enum class Source : uint8_t { None, Pack, Tone };

    Source source = Source::None;
    size_t samples = 0;
    size_t Copy(size_t offset, int16_t* out, size_t max);

private:
    friend bool LoadGameClip(GamePackReader* pack, uint16_t id, GameClip* out);
    uint16_t id_ = 0;
    GamePackReader* pack_ = nullptr;
    SoundClipInfo info_{};
};

// pack may be nullptr or not Ready(). False when neither has the id.
bool LoadGameClip(GamePackReader* pack, uint16_t id, GameClip* out);
const char* GameClipSourceName(GameClip::Source source);

}  // namespace kid_app
