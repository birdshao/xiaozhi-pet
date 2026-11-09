// V9 1E (G4): ids of the spoken game rules and the sound bank lookup.
// KPS1 id = 600 + game id (21..39); the games ask for a rule through a clip
// id in the game clip space (kGameRuleClipBase + game id).
#include "kid_app/care_phrases.h"
#include "kid_app/game_rules.h"
#include "kid_app/mini_game.h"
#include "kid_app/personality.h"
#include "kid_app/sound_bank.h"
#include "kid_app/sound_pack_reader.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <vector>

using namespace kid_app;

namespace {

class MemoryFile final : public StoryPackFile {
public:
    explicit MemoryFile(std::vector<uint8_t> b) : bytes(std::move(b)) {}
    uint64_t Size() const override { return bytes.size(); }
    bool ReadAt(uint32_t pos, void* dst, size_t size) override {
        if (!dst || uint64_t(pos) + size > bytes.size()) return false;
        std::memcpy(dst, bytes.data() + pos, size);
        return true;
    }
    std::vector<uint8_t> bytes;
};

void Put16(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    b[at] = uint8_t(v);
    b[at + 1] = uint8_t(v >> 8);
}
void Put32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + i] = uint8_t(v >> (8 * i));
}

// Same layout as tools/sound_pack.py encode().
std::vector<uint8_t> Encode(const std::map<uint16_t, std::vector<int16_t>>& clips) {
    const uint32_t count = static_cast<uint32_t>(clips.size());
    const uint32_t data = 32 + 16 * count;
    std::vector<uint8_t> b(data, 0);
    uint32_t i = 0;
    for (const auto& [id, pcm] : clips) {
        while (b.size() % 4) b.push_back(0);
        const uint32_t offset = static_cast<uint32_t>(b.size());
        const uint32_t bytes = static_cast<uint32_t>(pcm.size() * 2);
        for (int16_t s : pcm) {
            b.push_back(uint8_t(s));
            b.push_back(uint8_t(uint16_t(s) >> 8));
        }
        const size_t r = 32 + 16 * i++;
        Put16(b, r, id);
        Put32(b, r + 4, offset);
        Put32(b, r + 8, bytes);
        Put32(b, r + 12, SoundPackReader::Crc32(b.data() + offset, bytes));
    }
    std::memcpy(b.data(), "KPS1", 4);
    Put16(b, 4, 1);
    Put16(b, 6, count);
    Put32(b, 8, 24000);
    Put32(b, 12, 1);
    Put32(b, 16, data);
    Put32(b, 20, static_cast<uint32_t>(b.size()));
    Put32(b, 24, SoundPackReader::Crc32(b.data() + 32, 16 * count));
    Put32(b, 28, SoundPackReader::Crc32(b.data(), 28));
    return b;
}

std::vector<int16_t> Ramp(size_t n, int16_t start) {
    std::vector<int16_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<int16_t>(start + int(i) * 3);
    return v;
}

constexpr uint16_t kIds[] = {21, 22, 23, 24, 25, 26, 27, 28, 29,
                             31, 32, 33, 34, 35, 36, 37, 38, 39};

void TestIdMapping() {
    for (uint16_t id : kIds) {
        assert(GameHasRule(id));
        const uint16_t kps = GameRuleKpsId(id);
        assert(kps >= 600 && kps <= 999);
        // Memory 6 says the same words as memory 4: one clip.
        assert(kps == (id == 22 ? 621 : 600 + id));
        const uint16_t clip = GameRuleClip(id);
        assert(IsGameRuleClip(clip) && GameRuleGameId(clip) == id);
        assert(clip >= kGameRuleClipBase && clip > kGameToneBase);
    }
    // No rule: the older games, pack content ids, gaps and zero.
    for (uint16_t id : {0, 1, 2, 5, 20, 30, 40, 99, 100, 201, 4000}) {
        assert(!GameHasRule(static_cast<uint16_t>(id)));
        assert(GameRuleKpsId(static_cast<uint16_t>(id)) == 0);
        assert(GameRuleClip(static_cast<uint16_t>(id)) == 0);
    }
    // Not a rule clip: tones, session lines, junk.
    for (uint16_t clip : {uint16_t{0}, kGameClipRight, kGameClipBye, kGameTonePrompt,
                          GameArtTone(41), GameArtTone(530),
                          static_cast<uint16_t>(kGameRuleClipBase - 1),
                          static_cast<uint16_t>(kGameRuleClipBase + 30),
                          static_cast<uint16_t>(kGameRuleClipBase + 40)}) {
        assert(!IsGameRuleClip(clip) && GameRuleGameId(clip) == 0);
    }
    // Rule clips never collide with a tone: the player must not beep them.
    for (uint16_t id : kIds) assert(GameToneSamples(GameRuleClip(id)) == 0);
}

void TestPlaybackPlan() {
    const uint16_t rule = GameRuleClip(28);
    // A recorded rule plays from the pack as it is.
    RulePlan p = PlanGameRule(rule, true);
    assert(p.source == RuleSource::Pack && p.game_clip == rule);
    // Not recorded: the old prompt tone stands in, and says so.
    p = PlanGameRule(rule, false);
    assert(p.source == RuleSource::Tone && p.game_clip == kGameTonePrompt);
    // Everything else passes through untouched, whatever the pack holds.
    for (uint16_t clip : {uint16_t{0}, kGameClipRight, kGameTonePrompt, GameArtTone(41)}) {
        for (bool found : {false, true}) {
            p = PlanGameRule(clip, found);
            assert(p.source == RuleSource::None && p.game_clip == clip);
        }
    }
}

void TestKpsIdsAreFree() {
    // 600..999 holds neither pet lines nor care lines today.
    for (uint16_t id = 600; id <= 999; ++id) {
        assert(!Personality::FindPhrase(id));
        assert(!FindCarePhrase(id));
    }
    for (uint16_t id = 1; id <= 5; ++id) assert(OptionClipId(id) < 600);
}

void TestBankLookup() {
    SoundBank none;
    SoundClip clip;
    assert(!none.LoadGameRule(21, &clip));  // no pack: the caller keeps the tone
    assert(!none.LoadGameRule(21, nullptr));

    MemoryFile file(Encode({{OptionClipId(1), Ramp(100, 5)},
                            {621, Ramp(4801, -2000)},
                            {623, Ramp(240, 100)}}));
    SoundPackReader pack;
    assert(pack.Open(file));
    SoundBank bank;
    bank.UsePack(&pack);
    assert(bank.LoadGameRule(21, &clip));
    assert(clip.source == SoundClip::Source::Pack && clip.samples == 4801);
    int16_t pcm[4] = {};
    assert(clip.Copy(0, pcm, 4) == 4 && pcm[0] == -2000 && pcm[3] == -1991);
    // Memory 6 shares the memory 4 clip.
    SoundClip six;
    assert(bank.LoadGameRule(22, &six) && six.samples == 4801);
    assert(bank.LoadGameRule(23, &clip) && clip.samples == 240);
    // Recorded for some games only: the rest is not found (tone fallback).
    assert(!bank.LoadGameRule(24, &clip));
    // A game without a rule never reads the pack, even for a stray clip.
    assert(!bank.LoadGameRule(1, &clip) && !bank.LoadGameRule(30, &clip));
    assert(!bank.LoadGameRule(100, &clip));
}

}  // namespace

int main() {
    TestIdMapping();
    TestPlaybackPlan();
    TestKpsIdsAreFree();
    TestBankLookup();
    std::cout << "game rules ok\n";
    return 0;
}
