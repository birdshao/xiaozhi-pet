// V2-08 sound pack (KPS1 reader, built-in fallback) and V5-04 pet lines.
// Optional argv[1]: a directory holding default.kps made by
// `python3 -m tools.sound_pack demo` (cross-language check, scripts/p2_test.sh).
#include "kid_app/home_menu.h"
#include "kid_app/pet_phrases.h"
#include "kid_app/sound_bank.h"
#include "kid_app/sound_pack_reader.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using namespace kid_app;

namespace {

class MemoryFile final : public StoryPackFile {
public:
    explicit MemoryFile(std::vector<uint8_t> b) : bytes(std::move(b)) {}
    uint64_t Size() const override { return bytes.size(); }
    bool ReadAt(uint32_t pos, void* dst, size_t size) override {
        if (fail || !dst || uint64_t(pos) + size > bytes.size()) return false;
        std::memcpy(dst, bytes.data() + pos, size);
        return true;
    }
    std::vector<uint8_t> bytes;
    bool fail = false;
};

void Put16(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    b[at] = uint8_t(v);
    b[at + 1] = uint8_t(v >> 8);
}
void Put32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + i] = uint8_t(v >> (8 * i));
}
uint32_t Get32(const std::vector<uint8_t>& b, size_t at) {
    return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 | uint32_t(b[at + 2]) << 16 |
           uint32_t(b[at + 3]) << 24;
}

std::vector<int16_t> Ramp(size_t n, int16_t start) {
    std::vector<int16_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<int16_t>(start + int(i) * 3);
    return v;
}

// Same layout as tools/sound_pack.py encode().
std::vector<uint8_t> Encode(const std::map<uint16_t, std::vector<int16_t>>& clips,
                            uint32_t revision = 7) {
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
    Put32(b, 12, revision);
    Put32(b, 16, data);
    Put32(b, 20, static_cast<uint32_t>(b.size()));
    Put32(b, 24, SoundPackReader::Crc32(b.data() + 32, 16 * count));
    Put32(b, 28, SoundPackReader::Crc32(b.data(), 28));
    return b;
}
void Reseal(std::vector<uint8_t>& b) {
    const uint32_t count = b[6] | b[7] << 8;
    Put32(b, 24, SoundPackReader::Crc32(b.data() + 32, 16 * count));
    Put32(b, 28, SoundPackReader::Crc32(b.data(), 28));
}

constexpr uint16_t kHello = 211;  // gentle greeting line
constexpr uint16_t kNotAPhrase = 999;

std::map<uint16_t, std::vector<int16_t>> SampleClips() {
    return {{OptionClipId(1), Ramp(301, -400)},  // odd sample count: padding
            {OptionClipId(3), Ramp(240, 100)},
            {kHello, Ramp(4800, -2000)},
            {kNotAPhrase, Ramp(10, 0)}};
}

void TestCrcMatchesZlib() {
    const char* text = "123456789";
    const auto* d = reinterpret_cast<const uint8_t*>(text);
    assert(SoundPackReader::Crc32(d, 9) == 0xCBF43926u);
    assert(SoundPackReader::Crc32(d + 4, 5, SoundPackReader::Crc32(d, 4)) == 0xCBF43926u);
}

void TestReaderOpensAndReads() {
    MemoryFile file(Encode(SampleClips()));
    SoundPackReader pack;
    assert(pack.Open(file) && pack.Ready());
    assert(pack.ClipCount() == 4 && pack.Revision() == 7);
    SoundClipInfo info{};
    assert(pack.Find(OptionClipId(1), &info) && info.Samples() == 301);
    assert(info.offset % 4 == 0);
    assert(pack.Find(211, &info) && info.Samples() == 4800);
    assert(!pack.Find(OptionClipId(0), &info) && !pack.Find(998, &info));
    int16_t pcm[4] = {};
    assert(pack.ReadPcm(info, 2, pcm, sizeof(pcm)));
    assert(pcm[0] == -1997 && pcm[3] == -1988);
    assert(!pack.ReadPcm(info, 1, pcm, 2));               // odd offset
    assert(!pack.ReadPcm(info, info.bytes - 2, pcm, 4));  // past the clip
    pack.Close();
    assert(!pack.Ready() && !pack.Find(211, &info));
}

void TestReaderRejectsDamage() {
    const auto good = Encode(SampleClips());
    auto expect_bad = [](std::vector<uint8_t> bytes) {
        MemoryFile file(std::move(bytes));
        SoundPackReader pack;
        assert(!pack.Open(file) && !pack.Ready());
    };
    {
        auto b = good;
        b[0] = 'X';
        Reseal(b);
        expect_bad(b);  // magic (KPB1 books are not sound packs)
    }
    {
        auto b = good;
        std::memcpy(b.data(), "KPB2", 4);  // a KPB2 book (or reserved type)
        Reseal(b);
        expect_bad(b);
    }
    {
        auto b = good;
        Put16(b, 4, 2);
        Reseal(b);
        expect_bad(b);  // unknown version
    }
    {
        auto b = good;
        Put32(b, 8, 16000);
        Reseal(b);
        expect_bad(b);  // wrong sample rate
    }
    {
        auto b = good;
        b[13] ^= 1;
        expect_bad(b);  // header CRC
    }
    {
        auto b = good;
        b[32] ^= 1;  // clip id changed, table CRC not updated
        Put32(b, 28, SoundPackReader::Crc32(b.data(), 28));
        expect_bad(b);
    }
    {
        auto b = good;
        b.back() ^= 0x40;
        expect_bad(b);  // clip payload CRC
    }
    {
        auto b = good;
        b.pop_back();
        expect_bad(b);  // truncated
    }
    {
        auto b = good;
        b.insert(b.end(), {0, 0, 0, 0});
        expect_bad(b);  // trailing bytes
    }
    {
        auto b = good;
        Put16(b, 32 + 16, 1);  // second id not ascending
        Reseal(b);
        expect_bad(b);
    }
    {
        auto b = good;
        Put32(b, 32 + 4, Get32(b, 32 + 4) + 4);  // gap before the first clip
        Reseal(b);
        expect_bad(b);
    }
    {
        auto b = good;
        Put16(b, 32 + 2, 1);  // reserved field
        Reseal(b);
        expect_bad(b);
    }
    {
        auto b = good;
        Put16(b, 6, 0);
        Reseal(b);
        expect_bad(b);  // empty pack
    }
    {
        // A clip longer than 6 s is refused even with valid CRCs.
        expect_bad(Encode({{kHello, std::vector<int16_t>(6 * 24000 + 2, 1)}}));
    }
    {
        MemoryFile file(good);
        file.fail = true;
        SoundPackReader pack;
        assert(!pack.Open(file));
    }
    {
        MemoryFile file(std::vector<uint8_t>(good.begin(), good.begin() + 20));
        SoundPackReader pack;
        assert(!pack.Open(file));
    }
}

// Embedded option clips stand-in: one 24 kHz PCM16 mono WAV.
std::vector<uint8_t> g_wav;
bool FakeEmbedded(uint8_t index, WavClip* clip) {
    if (g_wav.empty()) {
        const uint32_t samples = 120;
        g_wav.assign(44 + samples * 2, 0);
        std::memcpy(g_wav.data(), "RIFF", 4);
        Put32(g_wav, 4, static_cast<uint32_t>(g_wav.size() - 8));
        std::memcpy(g_wav.data() + 8, "WAVEfmt ", 8);
        Put32(g_wav, 16, 16);
        Put16(g_wav, 20, 1);
        Put16(g_wav, 22, 1);
        Put32(g_wav, 24, 24000);
        Put32(g_wav, 28, 48000);
        Put16(g_wav, 32, 2);
        Put16(g_wav, 34, 16);
        std::memcpy(g_wav.data() + 36, "data", 4);
        Put32(g_wav, 40, samples * 2);
        for (uint32_t i = 0; i < samples; ++i) Put16(g_wav, 44 + 2 * i, 50 + i);
    }
    return index < HomeMenu::kItemCount && ParseWavClip(g_wav.data(), g_wav.size(), clip);
}

void TestBankFallsBackClipByClip() {
    SoundClip clip;
    // No pack: options embedded, lines a chime.
    SoundBank builtin(&FakeEmbedded);
    assert(!builtin.HasPack());
    assert(builtin.LoadOption(1, &clip) && clip.source == SoundClip::Source::Embedded);
    assert(clip.samples == 120);
    int16_t out[480] = {};
    assert(clip.Copy(0, out, 480) == 120 && out[0] == 50 && out[119] == 169);
    assert(!builtin.LoadOption(HomeMenu::kItemCount, &clip));
    assert(builtin.LoadPhrase(211, &clip) && clip.source == SoundClip::Source::Chime);
    assert(clip.samples == ChimeSamples(PhraseContext::Greeting));
    assert(!builtin.LoadPhrase(0, &clip) && !builtin.LoadPhrase(999, &clip));
    assert(!SoundBank{}.LoadOption(0, &clip));  // no embedded loader

    // With a pack: its clips win, missing ones still fall back.
    MemoryFile file(Encode(SampleClips()));
    SoundPackReader pack;
    assert(pack.Open(file));
    SoundBank bank(&FakeEmbedded);
    bank.UsePack(&pack);
    assert(bank.HasPack());
    assert(bank.LoadOption(1, &clip) && clip.source == SoundClip::Source::Pack);
    assert(clip.samples == 301);
    assert(clip.Copy(0, out, 480) == 301 && out[0] == -400 && out[300] == 500);
    assert(clip.Copy(300, out, 480) == 1 && out[0] == 500);
    assert(clip.Copy(301, out, 480) == 0);
    assert(bank.LoadOption(0, &clip) && clip.source == SoundClip::Source::Embedded);
    assert(bank.LoadPhrase(211, &clip) && clip.source == SoundClip::Source::Pack);
    assert(clip.samples == 4800);
    assert(bank.LoadPhrase(212, &clip) && clip.source == SoundClip::Source::Chime);
    // A pack id that is not a reviewed phrase is never played as one.
    assert(!bank.LoadPhrase(999, &clip));

    // TF card gone mid-clip: Copy reports 0 so the player stops.
    assert(bank.LoadPhrase(211, &clip));
    file.fail = true;
    assert(clip.Copy(0, out, 480) == 0);

    SoundPackReader closed;
    bank.UsePack(&closed);  // not Ready(): ignored
    assert(!bank.HasPack());
    assert(std::string(SoundSourceName(SoundClip::Source::Pack)) == "pack");
    assert(std::string(SoundSourceName(SoundClip::Source::Chime)) == "chime");
}

void TestChimes() {
    for (size_t c = 0; c < kPhraseContextCount; ++c) {
        const auto context = static_cast<PhraseContext>(c);
        const size_t n = ChimeSamples(context);
        // Short: 0.2..1.2 s, well under a recorded line.
        assert(n >= 24000 / 5 && n <= 24000 * 6 / 5);
        std::vector<int16_t> whole(n + 10, 0);
        assert(RenderChime(context, 0, whole.data(), whole.size()) == n);
        int peak = 0;
        for (size_t i = 0; i < n; ++i) peak = std::max(peak, std::abs(int(whole[i])));
        assert(peak > 3000 && peak < 32767 / 4);  // audible, quiet
        assert(std::abs(int(whole[0])) < 200 && std::abs(int(whole[n - 1])) < 200);
        // Chunked rendering equals one pass (the player streams 480 at a time).
        std::vector<int16_t> chunk(480);
        for (size_t off = 0; off < n; off += 480) {
            const size_t got = RenderChime(context, off, chunk.data(), chunk.size());
            assert(got == std::min<size_t>(480, n - off));
            assert(std::equal(chunk.begin(), chunk.begin() + got, whole.begin() + off));
        }
        assert(RenderChime(context, n, chunk.data(), chunk.size()) == 0);
    }
    assert(ChimeSamples(PhraseContext::Giggle) != ChimeSamples(PhraseContext::Bedtime));
    assert(ChimeSamples(static_cast<PhraseContext>(kPhraseContextCount)) == 0);
}

const PhraseContext kPlayed[] = {PhraseContext::Greeting, PhraseContext::MorningRecap,
                                 PhraseContext::Feed, PhraseContext::Bedtime,
                                 PhraseContext::Giggle, PhraseContext::HotTip,
                                 PhraseContext::ColdTip};

void TestPlannerIsDeterministicPerTemperament() {
    for (Temperament t : {Temperament::Curious, Temperament::Gentle, Temperament::Lively}) {
        PhrasePlanner a, b;
        a.SetTemperament(t);
        b.SetTemperament(t);
        assert(a.GetTemperament() == t);
        uint32_t now = 1000;
        for (int round = 0; round < 6; ++round) {
            for (PhraseContext c : kPlayed) {
                now += PhrasePlanner::kMinGapMs;
                const uint16_t id = a.Next(c, now);
                assert(id && id == b.Next(c, now));
                const PhraseTemplate* p = Personality::FindPhrase(id);
                assert(p && p->temperament == t && p->context == c);
                assert(RecordablePhrase(id));  // never a {book}/{like} line
            }
        }
    }
    // Repeated events rotate between the reviewed lines of a context.
    PhrasePlanner p;
    std::map<uint16_t, int> seen;
    for (int i = 0; i < 12; ++i) ++seen[p.Next(PhraseContext::Feed, 10000u * (i + 1))];
    assert(seen.size() == 2 && seen.count(241) && seen.count(242));
}

void TestPlannerKeepsLinesApart() {
    PhrasePlanner p;  // gentle by default
    assert(p.GetTemperament() == Temperament::Gentle);
    const uint32_t t0 = 0xFFFFF000u;  // wraps during the test
    assert(p.Next(PhraseContext::Greeting, t0) != 0);
    assert(p.Next(PhraseContext::Giggle, t0 + 100) == 0);
    assert(p.Next(PhraseContext::Feed, t0 + PhrasePlanner::kMinGapMs - 1) == 0);
    // Good morning and good night are never dropped.
    assert(p.Next(PhraseContext::Bedtime, t0 + 200) / 10 == 26);
    assert(p.Next(PhraseContext::MorningRecap, t0 + 300) == 221);
    const uint32_t later = t0 + 300 + PhrasePlanner::kMinGapMs;  // past the wrap
    assert(p.Next(PhraseContext::Giggle, later) / 10 == 27);
    // A dropped line does not advance the rotation.
    PhrasePlanner q, r;
    assert(q.Next(PhraseContext::Greeting, 0) == r.Next(PhraseContext::Greeting, 0));
    assert(q.Next(PhraseContext::Feed, 10) == 0);
    assert(q.Next(PhraseContext::Feed, 9000) == r.Next(PhraseContext::Feed, 9000));
    assert(q.Next(static_cast<PhraseContext>(kPhraseContextCount), 99999) == 0);
}

// tools/selftest/v5_phrases.pettest expects exactly these lines.
void TestSelfTestScenarioLines() {
    PhrasePlanner p;  // the board's default (gentle)
    assert(p.Next(PhraseContext::Greeting, 0) == 211);
    assert(p.Next(PhraseContext::Giggle, 1500) == 0);  // skipped: too soon
    assert(p.Next(PhraseContext::Giggle, 7500) == 272);
    assert(p.Next(PhraseContext::Feed, 18500) == 241);
    assert(p.Next(PhraseContext::Bedtime, 29000) == 261);
}

void TestPhraseNamesAndRecordability() {
    assert(std::string(PhraseContextName(PhraseContext::Greeting)) == "greeting");
    assert(std::string(PhraseContextName(PhraseContext::MorningRecap)) == "morning");
    assert(std::string(PhraseContextName(PhraseContext::Giggle)) == "giggle");
    assert(RecordablePhrase(211) && RecordablePhrase(371));
    assert(!RecordablePhrase(122) && !RecordablePhrase(232));  // slot lines
    assert(!RecordablePhrase(0) && !RecordablePhrase(5));
}

void TestToolPack(const std::string& dir) {
    SoundPackFilePort port;
    assert(!port.Open(dir.c_str(), "../escape"));
    assert(!port.Open(dir.c_str(), "Default"));
    assert(!port.Open("relative/dir", "default"));
    assert(!port.Open(dir.c_str(), "absent_pack"));
    assert(port.Open(dir.c_str(), "default"));
    SoundPackReader pack;
    assert(pack.Open(port));
    SoundClipInfo info{};
    for (uint8_t i = 0; i < HomeMenu::kItemCount; ++i)
        assert(pack.Find(OptionClipId(i), &info) && info.Samples() <= 2 * 24000u);
    // The demo pack has every line the firmware speaks, for all temperaments.
    size_t phrases = 0;
    for (size_t i = 0; i < Personality::PhraseCount(); ++i) {
        const PhraseTemplate& p = Personality::PhraseAt(i);
        const bool played = std::find(std::begin(kPlayed), std::end(kPlayed), p.context) !=
                            std::end(kPlayed);
        const bool in_pack = pack.Find(p.id, &info);
        assert(in_pack == (played && p.slot == PhraseSlot::None));
        phrases += in_pack;
    }
    assert(pack.ClipCount() == HomeMenu::kItemCount + phrases);
    SoundBank bank;
    bank.UsePack(&pack);
    SoundClip clip;
    assert(bank.LoadPhrase(271, &clip) && clip.source == SoundClip::Source::Pack);
    std::vector<int16_t> pcm(clip.samples);
    assert(clip.Copy(0, pcm.data(), pcm.size()) == pcm.size());
    assert(*std::max_element(pcm.begin(), pcm.end()) > 1000);
    std::cout << "PASS: tool KPS1 pack, clips=" << pack.ClipCount() << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    TestCrcMatchesZlib();
    TestReaderOpensAndReads();
    TestReaderRejectsDamage();
    TestBankFallsBackClipByClip();
    TestChimes();
    TestPlannerIsDeterministicPerTemperament();
    TestPlannerKeepsLinesApart();
    TestSelfTestScenarioLines();
    TestPhraseNamesAndRecordability();
    if (argc > 1) TestToolPack(argv[1]);
    std::cout << "sound pack tests passed\n";
    return 0;
}
