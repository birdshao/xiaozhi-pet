// V4-06 KPG1 game packs: the golden pack made by tools/game_pack.py
// (tests/fixtures/kpg1, synthetic tones, not child content), clip fallback
// to built-in tones, and rejection of damaged packs.
// argv[1]: the fixture directory (holds golden_game.kpg).
#include "kid_app/game_pack_reader.h"

#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

using namespace kid_app;

namespace {

class MemoryFile final : public StoryPackFile {
public:
    explicit MemoryFile(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {}
    uint64_t Size() const override { return bytes_.size(); }
    bool ReadAt(uint32_t offset, void* out, size_t size) override {
        if (uint64_t(offset) + size > bytes_.size()) return false;
        std::memcpy(out, bytes_.data() + offset, size);
        return true;
    }
    std::vector<uint8_t> bytes_;
};

std::vector<uint8_t> ReadAll(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    assert(in && "golden_game.kpg missing");
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool Opens(std::vector<uint8_t> bytes) {
    MemoryFile file(std::move(bytes));
    GamePackReader reader;
    const bool ok = reader.Open(file);
    assert(ok == reader.Ready());
    return ok;
}

uint16_t U16At(const std::vector<uint8_t>& b, size_t at) {
    return static_cast<uint16_t>(b[at] | (b[at + 1] << 8));
}

}  // namespace

int main(int argc, char** argv) {
    assert(argc > 1);
    const std::string dir = argv[1];
    const auto golden = ReadAll(dir + "/golden_game.kpg");

    MemoryFile file(golden);
    GamePackReader pack;
    assert(pack.Open(file));
    assert(pack.ContentId() == 900 && pack.Revision() == 1);
    assert(pack.RoundCount() == 5 && pack.ClipCount() == 18);
    // Round 1: which animal? (listen) cat / dog / bird, cat is right.
    const GameRound& r = pack.Rounds()[0];
    assert(r.kind == GameKind::AnimalSound && r.option_count == 3 && r.correct == 0);
    assert(r.question_art == static_cast<uint16_t>(GameArt::Listen));
    assert(r.option_art[0] == static_cast<uint16_t>(GameArt::Cat) &&
           r.option_art[1] == static_cast<uint16_t>(GameArt::Dog) &&
           r.option_art[2] == static_cast<uint16_t>(GameArt::Bird));
    assert(r.prompt_clip == 16 && r.option_clip[0] == 17 && r.option_clip[2] == 19);
    // Round 2: two cards only.
    assert(pack.Rounds()[1].kind == GameKind::BigSmall && pack.Rounds()[1].option_count == 2 &&
           pack.Rounds()[1].option_art[2] == 0);
    // Round 5 reuses the cat recording of round 1.
    assert(pack.Rounds()[4].kind == GameKind::EnglishWord &&
           pack.Rounds()[4].option_clip[1] == 17);
    for (size_t i = 0; i < pack.RoundCount(); ++i) assert(GameRoundValid(pack.Rounds()[i]));

    // Clips: the pack's own recordings first, built-in tones otherwise.
    GameClip clip;
    assert(LoadGameClip(&pack, 17, &clip) && clip.source == GameClip::Source::Pack);
    SoundClipInfo info{};
    assert(pack.FindClip(17, &info) && clip.samples == info.Samples());
    std::vector<int16_t> pcm(clip.samples);
    size_t got = 0;
    while (got < clip.samples) {
        const size_t n = clip.Copy(got, pcm.data() + got, 480);
        assert(n > 0);
        got += n;
    }
    assert(std::memcmp(pcm.data(), golden.data() + info.offset, info.bytes) == 0);
    assert(LoadGameClip(&pack, kGameClipRight, &clip) &&
           clip.source == GameClip::Source::Pack);  // recorded "right"
    assert(LoadGameClip(&pack, kGameClipBye, &clip) && clip.source == GameClip::Source::Tone);
    assert(std::strcmp(GameClipSourceName(clip.source), "tone") == 0);
    assert(LoadGameClip(nullptr, GameArtTone(static_cast<uint16_t>(GameArt::Cat)), &clip) &&
           clip.source == GameClip::Source::Tone);
    assert(!LoadGameClip(&pack, 998, &clip) && !LoadGameClip(nullptr, 17, &clip));
    assert(!LoadGameClip(&pack, 0, &clip));
    int16_t chunk[8];
    assert(!pack.ReadPcm(info, 1, chunk, 4) && !pack.ReadPcm(info, 0, chunk, 3));
    assert(!pack.ReadPcm(info, info.bytes - 2, chunk, 4));

    // A game from the pack plays like the built-in one.
    {
        GameSession s;
        GameOutput out{};
        assert(s.Start(pack.Rounds(), pack.RoundCount(), pack.ContentId(), 12, 5, 0, &out));
        assert(s.Result().rounds == 5 && out.clip >= 16 && out.clip <= 999);
    }

    // Damaged packs are rejected as a whole.
    {
        const uint16_t rounds = U16At(golden, 6);
        const size_t round0 = 32, clip_table = 32 + 24 * rounds;
        auto flipped = [&](size_t at) {
            auto b = golden;
            b[at] ^= 0x01;
            return b;
        };
        assert(!Opens(flipped(0)));                     // magic
        assert(!Opens(flipped(10)));                    // header CRC
        assert(!Opens(flipped(round0 + 2)));            // table CRC
        assert(!Opens(flipped(clip_table + 4)));        // clip offset (table CRC)
        assert(!Opens(flipped(golden.size() - 1)));     // clip CRC
        auto truncated = golden;
        truncated.pop_back();
        assert(!Opens(truncated));
        auto longer = golden;
        longer.insert(longer.end(), 4, 0);
        assert(!Opens(longer));
        assert(!Opens({}));
        assert(Opens(golden));
        GamePackReader reader;
        MemoryFile bad(flipped(golden.size() - 1));
        assert(!reader.Open(bad) && reader.RoundCount() == 0 && !reader.FindClip(17, &info));
    }

    // SD path rules (only where the fixture lives on a POSIX path, e.g. CI).
    if (dir[0] == '/') {
        SoundPackFilePort port;
        assert(port.Open(dir.c_str(), "golden_game", "kpg"));
        GamePackReader from_card;
        assert(from_card.Open(port) && from_card.RoundCount() == 5);
        assert(!port.Open(dir.c_str(), "golden_game", "exe"));
        assert(!port.Open(dir.c_str(), "golden_game"));  // no .kps of that name
    }

    std::cout << "game pack reader tests passed\n";
    return 0;
}
