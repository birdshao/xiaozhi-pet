// Native device-equivalent math game integration test. NO ESP32 flash.
// Uses the actual KPG1 Python encoder output, the real C++ firmware
// GamePackReader / GameSession / InputHub / SceneComposer, and exports P4
// monochrome 800x480 PBMs using the actual GameArt renderer.
//
// Input: argv[1] = generated synthetic KPG1 placeholder path;
//        argv[2] = writable directory for generated PBM previews.
#include "kid_app/game_pack_reader.h"
#include "kid_app/input_hub.h"
#include "kid_app/mini_game.h"
#include "kid_app/scene_model.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace kid_app;

namespace {

class MemoryFile final : public StoryPackFile {
public:
    explicit MemoryFile(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {}
    uint64_t Size() const override { return bytes_.size(); }
    bool ReadAt(uint32_t offset, void* out, size_t length) override {
        if (!out || uint64_t(offset) + length > bytes_.size()) return false;
        std::memcpy(out, bytes_.data() + offset, length);
        return true;
    }
private:
    std::vector<uint8_t> bytes_{};
};

std::vector<uint8_t> Read(const char* path) {
    std::ifstream stream(path, std::ios::binary);
    assert(stream && "Synthetic KPG1 fixture missing");
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

SceneModel GameScene(const GameView& game) {
    SceneModel model{};
    model.kind = SceneKind::Game;
    model.game = game;
    return model;
}

void SavePbm(const std::filesystem::path& output, const GameView& view) {
    MonoFrame pixels;
    SceneComposer::Render(GameScene(view), pixels);
    std::ofstream out(output, std::ios::binary);
    assert(out);
    out << "P4\n" << MonoFrame::kWidth << " " << MonoFrame::kHeight << "\n";
    // MonoFrame bit 0 = black; PBM P4 bit 1 = black.
    for (uint8_t byte : pixels.Data()) {
        const auto inverted = static_cast<char>(~byte);
        out.write(&inverted, 1);
    }
    assert(out.good());
}

bool DifferenceOnlyInRail(const GameView& before, const GameView& after) {
    MonoFrame a, b;
    SceneComposer::Render(GameScene(before), a);
    SceneComposer::Render(GameScene(after), b);
    bool any = false;
    for (int y = 0; y < MonoFrame::kHeight; ++y) {
        for (int x = 0; x < MonoFrame::kWidth; ++x) {
            if (a.IsBlack(x, y) != b.IsBlack(x, y)) {
                if (y < SceneComposer::kGameRailTop) return false;
                any = true;
            }
        }
    }
    return any;
}

GameOutput DialMove(GameSession* game, InputHub* dial, InputKey key,
                    InputKind expected, GameInput move, uint32_t at) {
    dial->Press(key, at);
    InputEvent event{};
    assert(dial->Next(&event) && event.kind == expected && !event.repeat);
    assert(event.at_ms == at);
    assert(!dial->Next(&event));
    GameOutput result = game->Input(move, event.at_ms);
    dial->Release(key, at + 15);
    dial->Tick(at + 140);  // debounce expired
    assert(!dial->Next(&event));
    return result;
}

GameOutput DialPress(GameSession* game, InputHub* dial, uint32_t at) {
    dial->Press(InputKey::DialPress, at);
    dial->Release(InputKey::DialPress, at + 40);
    dial->Tick(at + 170);  // 120ms release bounce window
    InputEvent event{};
    assert(dial->Next(&event) && event.kind == InputKind::DialPress);
    assert(event.at_ms == at + 40);
    assert(!dial->Next(&event));
    return game->Input(GameInput::Press, event.at_ms);
}

void VerifyRoundSound(GamePackReader* pack, const GameRound& round) {
    GameClip clip;
    assert(LoadGameClip(pack, round.prompt_clip, &clip));
    assert(clip.source == GameClip::Source::Pack && clip.samples >= 2400);
    std::array<int16_t, 480> pcm{};
    size_t total = 0;
    int16_t peak = 0;
    while (total < clip.samples) {
        const auto copied = clip.Copy(total, pcm.data(), pcm.size());
        assert(copied > 0);
        for (size_t k = 0; k < copied; ++k) {
            const int16_t s = pcm[k];
            if (s > peak) peak = s;
        }
        total += copied;
    }
    assert(peak > 100);  // test-only tones must carry actual PCM
    for (uint8_t j = 0; j < round.option_count; ++j) {
        assert(LoadGameClip(pack, round.option_clip[j], &clip));
        assert(clip.source == GameClip::Source::Pack && clip.samples > 0);
        assert(clip.Copy(0, pcm.data(), pcm.size()) > 0);
    }
}

GameView ExampleView(const GameRound& r) {
    GameView g{};
    g.round = 1;
    g.rounds = 3;
    g.option_count = r.option_count;
    g.highlight = 0;
    g.question_art = r.question_art;
    g.options = r.option_art;
    return g;
}

void CorrectSession(GamePackReader* pack, uint32_t seed) {
    GameSession session;
    GameOutput output{};
    assert(session.Start(pack->Rounds(), pack->RoundCount(), pack->ContentId(),
                         seed, 3, 1000, &output));
    assert(output.redraw && !output.finished && !output.right);
    uint32_t now = 1100;
    for (int i = 0; i < 3; ++i) {
        const GameRound question = session.Current();
        VerifyRoundSound(pack, question);
        assert(GameRoundValid(question));
        for (uint8_t j = 0; j < question.correct; ++j) {
            output = session.Input(GameInput::Down, now);
            assert(output.clip == question.option_clip[j + 1]);
            now += 200;
        }
        assert(session.View().highlight == question.correct);
        output = session.Input(GameInput::Press, now);
        assert(output.right && output.redraw && output.clip == kGameClipRight);
        assert(session.View().feedback == GameFeedback::Right);
        // Re-press during animation must not reward twice.
        assert(!session.Input(GameInput::Press, now + 10).right);
        now += GameSession::kRightMs;
        output = session.Tick(now);
        assert(output.finished == (i == 2));
        now += 200;
    }
    assert(!session.Active());
    assert(session.Result().end == GameEnd::Completed);
    assert(session.Result().correct == 3 && session.Result().played == 3);
    assert(session.Result().game_id == pack->ContentId());
}

}  // namespace

int main(int argc, char** argv) {
    assert(argc == 3);
    const auto bytes = Read(argv[1]);
    MemoryFile source(bytes);
    GamePackReader pack;
    assert(pack.Open(source) && pack.Ready());
    assert(pack.ContentId() == 20001 && pack.Revision() == 1);
    assert(pack.RoundCount() == 15 && pack.ClipCount() == 19);
    std::map<GameKind, int> kinds;
    std::map<GameKind, GameRound> samples;
    for (size_t i = 0; i < pack.RoundCount(); ++i) {
        const auto& round = pack.Rounds()[i];
        assert(GameRoundValid(round));
        assert(round.correct < round.option_count);
        VerifyRoundSound(&pack, round);
        ++kinds[round.kind];
        samples[round.kind] = round;
    }
    assert(kinds[GameKind::MatchShape] == 5);
    assert(kinds[GameKind::Count] == 5);
    assert(kinds[GameKind::BigSmall] == 5);
    assert(kinds.size() == 3);

    std::filesystem::path preview_dir = argv[2];
    std::filesystem::create_directories(preview_dir);
    SavePbm(preview_dir / "math_shape_1bit.pbm",
            ExampleView(samples.at(GameKind::MatchShape)));
    SavePbm(preview_dir / "math_count_1bit.pbm",
            ExampleView(samples.at(GameKind::Count)));
    SavePbm(preview_dir / "math_size_1bit.pbm",
            ExampleView(samples.at(GameKind::BigSmall)));

    // A physical three-way dial controls the actual GameSession; no
    // auto-repeat and no clickable app-specific bypass are required.
    GameSession dial_game;
    GameOutput started{};
    uint32_t seed = 1;
    while (true) {
        assert(seed < 100);
        assert(dial_game.Start(pack.Rounds(), pack.RoundCount(),
                               pack.ContentId(), seed++, 3, 1000, &started));
        if (dial_game.Current().correct != 0) break;
    }
    InputHub input;
    input.SetAutoRepeat(false);  // game choices should move one step per press
    const GameView initial = dial_game.View();
    assert(initial.highlight == 0);
    auto moved = DialMove(&dial_game, &input, InputKey::DialDown,
                          InputKind::DialDown, GameInput::Down, 1200);
    assert(moved.redraw && moved.clip == dial_game.Current().option_clip[1]);
    assert(DifferenceOnlyInRail(initial, dial_game.View()));
    SavePbm(preview_dir / "math_selected_1bit.pbm", dial_game.View());
    // Reach the correct card without overshooting.
    uint32_t time = 1600;
    while (dial_game.View().highlight < dial_game.Current().correct) {
        moved = DialMove(&dial_game, &input, InputKey::DialDown,
                         InputKind::DialDown, GameInput::Down, time);
        assert(moved.redraw);
        time += 400;
    }
    const auto answer = DialPress(&dial_game, &input, time);
    assert(answer.right && answer.clip == kGameClipRight);
    // The positive response uses the existing gentle tone fallback.
    GameClip response;
    assert(LoadGameClip(&pack, answer.clip, &response));
    assert(response.source == GameClip::Source::Tone && response.samples > 0);
    const GameView happy = dial_game.View();
    SavePbm(preview_dir / "math_right_1bit.pbm", happy);
    assert(happy.feedback == GameFeedback::Right);

    // Two wrong attempts reveal the right answer without a reward.
    GameSession wrong_game;
    assert(wrong_game.Start(pack.Rounds(), pack.RoundCount(),
                            pack.ContentId(), 17, 3, 4000, &started));
    const uint8_t wrong = wrong_game.Current().correct == 0 ? 1 : 0;
    for (uint8_t i = 0; i < wrong; ++i)
        wrong_game.Input(GameInput::Down, 4100 + i * 100);
    auto feedback = wrong_game.Input(GameInput::Press, 4300);
    assert(feedback.clip == kGameClipTryAgain && !feedback.right);
    feedback = wrong_game.Input(GameInput::Press, 4500);
    assert(feedback.clip == kGameClipReveal && !feedback.right);
    assert(wrong_game.View().feedback == GameFeedback::Reveal);
    assert(wrong_game.View().highlight == wrong_game.Current().correct);
    assert(!wrong_game.Input(GameInput::Press, 4700).right);
    wrong_game.Tick(4500 + GameSession::kRevealMs);
    assert(wrong_game.Result().played == 1 && wrong_game.Result().correct == 0);

    // Real long dial-hold exits to home without a game-completed reward.
    InputHub long_dial;
    long_dial.Press(InputKey::DialPress, 5000);
    long_dial.Tick(6001);
    InputEvent long_event{};
    assert(long_dial.Next(&long_event) && long_event.kind == InputKind::DialLong);
    feedback = wrong_game.Input(GameInput::Long, long_event.at_ms);
    assert(feedback.finished && !feedback.right);
    assert(wrong_game.Result().end == GameEnd::Home && !wrong_game.Active());
    long_dial.Release(InputKey::DialPress, 6030);
    long_dial.Tick(6200);
    assert(!long_dial.Next(&long_event));

    // No interaction: repeat once at 12s, home after 60s.
    GameSession idle;
    assert(idle.Start(pack.Rounds(), pack.RoundCount(),
                      pack.ContentId(), 31, 3, 200, &started));
    assert(idle.Tick(200 + GameSession::kRepeatPromptMs).clip ==
           idle.Current().prompt_clip);
    assert(idle.Tick(200 + GameSession::kRepeatPromptMs + 1).clip == 0);
    assert(idle.Tick(200 + GameSession::kIdleExitMs).finished);
    assert(idle.Result().end == GameEnd::Idle && idle.Result().correct == 0);

    // Same real KPG1 pool must behave deterministically across many seeds.
    for (uint32_t value = 1; value <= 32; ++value) CorrectSession(&pack, value);

    // The C++ reader rejects a corrupted binary rather than rendering
    // a possibly mismatched question/audio/answer.
    auto broken = bytes;
    broken.back() ^= 0x01;
    MemoryFile damaged(std::move(broken));
    GamePackReader reject;
    assert(!reject.Open(damaged) && !reject.Ready());

    std::cout << "PASS: 15 KPG1 rounds (5/5/5), 19 PCM clips, real dial flow, "
                 "32 completed seeded sessions, reveal/idle/long, actual 1-bit "
                 "GameArt previews and CRC corruption rejection\n";
}
