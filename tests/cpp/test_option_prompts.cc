#include "kid_app/prompt_scheduler.h"
#include "kid_app/wav_clip.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

using namespace kid_app;

namespace {
void Put32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void Put16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v));
    b.push_back(static_cast<uint8_t>(v >> 8));
}
void Tag(std::vector<uint8_t>& b, const char* t) { b.insert(b.end(), t, t + 4); }

// Canonical RIFF/WAVE with an optional extra chunk before "data".
std::vector<uint8_t> Wav(uint32_t rate, uint16_t channels, uint16_t bits, size_t samples,
                         bool list_chunk = false, uint16_t format = 1) {
    std::vector<uint8_t> b;
    Tag(b, "RIFF");
    Put32(b, 0);  // patched below
    Tag(b, "WAVE");
    Tag(b, "fmt ");
    Put32(b, 16);
    Put16(b, format);
    Put16(b, channels);
    Put32(b, rate);
    Put32(b, rate * channels * bits / 8);
    Put16(b, static_cast<uint16_t>(channels * bits / 8));
    Put16(b, bits);
    if (list_chunk) {
        Tag(b, "LIST");
        Put32(b, 5);
        b.insert(b.end(), {'I', 'N', 'F', 'O', 'x', 0});  // odd size + pad byte
    }
    Tag(b, "data");
    Put32(b, static_cast<uint32_t>(samples * 2));
    for (size_t i = 0; i < samples; ++i) Put16(b, static_cast<uint16_t>(i));
    const uint32_t riff = static_cast<uint32_t>(b.size() - 8);
    std::memcpy(&b[4], &riff, 4);
    return b;
}
}  // namespace

int main() {
    // WAV clips: only 24 kHz mono 16-bit PCM (the XiaoZhi output format).
    {
        const auto ok = Wav(24000, 1, 16, 1200);
        WavClip clip;
        assert(ParseWavClip(ok.data(), ok.size(), &clip));
        assert(clip.samples == 1200 && clip.Sample(0) == 0 && clip.Sample(1199) == 1199);
        // Chunked copy for the 480-sample AudioService limit (any alignment).
        int16_t chunk[480];
        assert(clip.Copy(960, chunk, 480) == 240 && chunk[0] == 960 && chunk[239] == 1199);
        assert(clip.Copy(1200, chunk, 480) == 0);

        const auto with_list = Wav(24000, 1, 16, 10, true);
        assert(ParseWavClip(with_list.data(), with_list.size(), &clip) && clip.samples == 10);

        assert(!ParseWavClip(Wav(16000, 1, 16, 10).data(), Wav(16000, 1, 16, 10).size(), &clip));
        assert(!ParseWavClip(Wav(24000, 2, 16, 10).data(), Wav(24000, 2, 16, 10).size(), &clip));
        assert(!ParseWavClip(Wav(24000, 1, 8, 10).data(), Wav(24000, 1, 8, 10).size(), &clip));
        assert(!ParseWavClip(Wav(24000, 1, 16, 10, false, 3).data(),
                             Wav(24000, 1, 16, 10, false, 3).size(), &clip));  // float
        auto truncated = Wav(24000, 1, 16, 100);
        truncated.resize(truncated.size() - 2);  // data chunk claims more bytes
        assert(!ParseWavClip(truncated.data(), truncated.size(), &clip));
        auto not_riff = ok;
        not_riff[0] = 'X';
        assert(!ParseWavClip(not_riff.data(), not_riff.size(), &clip));
        assert(!ParseWavClip(ok.data(), 20, &clip));
        assert(!ParseWavClip(nullptr, 0, &clip));
        // At most 2 s per prompt: a long recording is a content mistake.
        const auto long_clip = Wav(24000, 1, 16, 2 * 24000 + 1);
        assert(!ParseWavClip(long_clip.data(), long_clip.size(), &clip));
        assert(clip.samples == 0);  // failures never leave a usable clip
    }

    // Prompt scheduling: speak the item the child stops on, after 350 ms.
    {
        PromptScheduler prompts;
        PromptRequest req{};
        prompts.OnSelect(2, 1000);
        assert(!prompts.Tick(1349, &req));
        assert(prompts.Tick(1350, &req) && req.index == 2 && req.token == 1);
        assert(!prompts.Tick(5000, &req));  // only once

        // Rapid moves: only the final item is spoken.
        prompts.OnSelect(3, 6000);
        prompts.OnSelect(4, 6200);
        assert(!prompts.Tick(6500, &req));
        assert(prompts.Tick(6550, &req) && req.index == 4 && req.token == 2);

        // Cancel (press / leaving home / sleep / voice) drops a pending one.
        prompts.OnSelect(1, 7000);
        prompts.Cancel();
        assert(!prompts.Tick(9000, &req));

        // Disabled (not home, quiet, voice, asleep): selections are ignored.
        prompts.SetEnabled(false);
        prompts.OnSelect(0, 10000);
        assert(!prompts.Tick(11000, &req));
        prompts.SetEnabled(true);
        prompts.OnSelect(0, 12000);
        prompts.SetEnabled(false);  // disabling also drops what is pending
        prompts.SetEnabled(true);
        assert(!prompts.Tick(13000, &req));
        prompts.OnSelect(0, 14000);
        assert(prompts.Tick(14350, &req) && req.index == 0 && req.token == 3);
    }

    // Software volume: exact at 100 %, silent at 0, symmetric, clamped.
    {
        int16_t pcm[5] = {32767, -32768, 1000, -1000, 1};
        kid_app::ScalePcm(pcm, 5, 100);
        assert(pcm[0] == 32767 && pcm[1] == -32768 && pcm[4] == 1);
        kid_app::ScalePcm(pcm, 5, 250);  // clamps to 100
        assert(pcm[0] == 32767 && pcm[1] == -32768);
        kid_app::ScalePcm(pcm, 5, 15);
        assert(pcm[0] == 4915 && pcm[1] == -4915 && pcm[2] == 150 && pcm[3] == -150 &&
               pcm[4] == 0);
        kid_app::ScalePcm(pcm, 5, 0);
        for (int16_t s : pcm) assert(s == 0);
        kid_app::ScalePcm(nullptr, 5, 50);  // no crash
    }

    // uint32 wrap-around.
    {
        PromptScheduler prompts;
        PromptRequest req{};
        prompts.OnSelect(1, 0xFFFFFF00u);
        assert(!prompts.Tick(0x0000005Du, &req));  // 349 ms later
        assert(prompts.Tick(0x0000005Eu, &req));   // 350 ms later
    }

    std::cout << "PASS: WAV clip validation and option prompt scheduling\n";
}
