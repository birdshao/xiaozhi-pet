// LCP-10: boot-only selected KPG1, real C++ filesystem reader and fallback.
// All four input fixtures contain SYNTHETIC TONES, not reviewed curriculum.
#include "kid_app/game_course_loader.h"
#include "kid_app/mini_game.h"

#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace kid_app;
namespace fs = std::filesystem;

namespace {

void SetActive(const fs::path& games, const std::string& text) {
    std::ofstream out(games / "active.txt", std::ios::binary | std::ios::trunc);
    assert(out);
    out.write(text.data(), text.size());
    assert(out.good());
}

void CopyDefault(const fs::path& fixtures, const fs::path& games) {
    fs::copy_file(fixtures / "default.kpg", games / "default.kpg",
                  fs::copy_options::overwrite_existing);
}

void CheckSpeech(GameCourseLoader* loader) {
    auto* pack = loader->Pack();
    assert(pack && pack->Ready() && pack->RoundCount() > 0);
    const auto r = pack->Rounds()[0];
    GameClip clip;
    assert(LoadGameClip(pack, r.prompt_clip, &clip));
    assert(clip.source == GameClip::Source::Pack && clip.samples >= 2400);
    int16_t data[480]{};
    size_t consumed = 0;
    while (consumed < clip.samples) {
        const size_t got = clip.Copy(consumed, data, 480);
        assert(got > 0);
        consumed += got;
    }
    GameSession session;
    GameOutput out{};
    assert(session.Start(pack->Rounds(), pack->RoundCount(), pack->ContentId(),
                         7, 3, 1000, &out));
    assert(session.Result().game_id == loader->ContentId());
    for (int n = 0; n < 3; ++n) {
        const auto round = session.Current();
        for (uint8_t j = 0; j < round.correct; ++j)
            session.Input(GameInput::Down, 1100 + n * 3500 + j * 50);
        out = session.Input(GameInput::Press, 1500 + n * 3500);
        assert(out.right && out.clip == kGameClipRight);
        out = session.Tick(1500 + n * 3500 + GameSession::kRightMs);
    }
    assert(session.Result().end == GameEnd::Completed);
    assert(session.Result().correct == 3);
}

}  // namespace

int main(int argc, char** argv) {
    assert(argc == 3);
    const fs::path fixture = fs::absolute(argv[1]);
    const fs::path games = fs::absolute(argv[2]);
    assert(fs::exists(fixture / "shapes.kpg"));
    assert(fs::exists(fixture / "counts.kpg"));
    assert(fs::exists(fixture / "sizes.kpg"));
    fs::create_directories(games);

    char name[GameCourseLoader::kNameBytes]{};
    assert(GameCourseLoader::ParsePackId("counts\n", 7, name));
    assert(std::strcmp(name, "counts") == 0);
    assert(GameCourseLoader::ParsePackId("sizes\r\n", 7, name));
    assert(std::strcmp(name, "sizes") == 0);
    for (const std::string bad : {
             "", "co", "Count", "../counts", "counts.kpg", "counts/else",
             "counts\\wrong", "counts\nextra", " counts", "counts ", "counts\r",
             "abcdefghijklmnop", "école", "c\0nts"}) {
        // The embedded-NUL case is exercised separately below: std::string
        // constructed from a C string would truncate at the NUL.
        if (bad == "c") continue;
        assert(!GameCourseLoader::ParsePackId(bad.data(), bad.size(), name));
    }
    const char with_null[6] = {'c', 'o', '\0', 'u', 'n', 't'};
    assert(!GameCourseLoader::ParsePackId(with_null, 6, name));

    // First boot reads the adult's active course and keeps the file alive.
    {
        GameCourseLoader selected;
        assert(selected.State() == GameCourseLoader::Result::NotStarted);
        assert(selected.LoadAtBoot(games.string().c_str()));
        assert(selected.Selection() == GameCourseLoader::Choice::Valid);
        assert(selected.State() == GameCourseLoader::Result::Selected);
        assert(std::strcmp(selected.LoadedName(), "counts") == 0);
        assert(selected.ContentId() == 20012 && selected.Rounds() == 5);
        CheckSpeech(&selected);
        // Changing active.txt while a session exists CANNOT hot-swap
        // its GamePackReader or the FILE* seen by the audio task.
        SetActive(games, "shapes\n");
        assert(selected.LoadAtBoot(games.string().c_str()));
        assert(selected.ContentId() == 20012);
        CheckSpeech(&selected);
    }
    {
        GameCourseLoader shapes;
        assert(shapes.LoadAtBoot(games.string().c_str()));
        assert(shapes.ContentId() == 20011);
        CheckSpeech(&shapes);
    }
    SetActive(games, "sizes\r\n");
    {
        GameCourseLoader sizes;
        assert(sizes.LoadAtBoot(games.string().c_str()));
        assert(sizes.ContentId() == 20013);
        CheckSpeech(&sizes);
    }

    // With the stable default installed, missing/invalid/absent selections
    // are not allowed to choose an arbitrary filename or break gameplay.
    CopyDefault(fixture, games);
    fs::remove(games / "active.txt");
    {
        GameCourseLoader fallback;
        assert(fallback.LoadAtBoot(games.string().c_str()));
        assert(fallback.State() == GameCourseLoader::Result::Default);
        assert(fallback.Selection() == GameCourseLoader::Choice::Missing);
        assert(fallback.ContentId() == 20001);
        CheckSpeech(&fallback);
    }
    SetActive(games, "../counts\n");
    {
        GameCourseLoader invalid;
        assert(invalid.LoadAtBoot(games.string().c_str()));
        assert(invalid.Selection() == GameCourseLoader::Choice::Invalid);
        assert(invalid.State() == GameCourseLoader::Result::Default);
        assert(std::strcmp(invalid.LoadedName(), "default") == 0);
    }
    SetActive(games, "missing\n");
    {
        GameCourseLoader absent;
        assert(absent.LoadAtBoot(games.string().c_str()));
        assert(absent.State() == GameCourseLoader::Result::FallbackDefault);
        assert(absent.ContentId() == 20001);
    }

    // Selected file exists but CRC is invalid -> full-pool default only.
    fs::copy_file(fixture / "counts.kpg", games / "counts.kpg",
                  fs::copy_options::overwrite_existing);
    {
        std::fstream file(games / "counts.kpg", std::ios::binary | std::ios::in |
                                                  std::ios::out);
        assert(file);
        file.seekp(-1, std::ios::end);
        const char flip = static_cast<char>(0xFF);
        file.write(&flip, 1);
    }
    SetActive(games, "counts\n");
    {
        GameCourseLoader damaged;
        assert(damaged.LoadAtBoot(games.string().c_str()));
        assert(damaged.State() == GameCourseLoader::Result::FallbackDefault);
        assert(damaged.ContentId() == 20001);
    }

    // No fallback -> builtin. No mutated reader/file survives a failed open.
    fs::remove(games / "default.kpg");
    {
        GameCourseLoader builtin;
        assert(!builtin.LoadAtBoot(games.string().c_str()));
        assert(!builtin.Ready() && builtin.Pack() == nullptr);
        assert(builtin.State() == GameCourseLoader::Result::Builtin);
        assert(builtin.ContentId() == 0);
        assert(!builtin.LoadAtBoot(games.string().c_str()));
    }
    {
        GameCourseLoader invalid_root;
        assert(!invalid_root.LoadAtBoot("../games"));
        assert(invalid_root.State() == GameCourseLoader::Result::Builtin);
    }
    std::cout << "PASS LCP10: 3 boot-selected course IDs/5 rounds, stable no hot-swap, "
                 "PCM + 3-round play, default/invalid/corrupt/builtin fallback\n";
}
