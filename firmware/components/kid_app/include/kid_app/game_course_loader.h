#pragma once

#include "kid_app/game_pack_reader.h"

#include <cstddef>
#include <cstdint>

namespace kid_app {

// LCP-10 V1: parent selects a pre-installed KPG1 before boot by writing
// /games/active.txt = <short ASCII pack id> + optional newline.
// Deliberately NO runtime hot switch: the audio worker and input task share
// GamePackReader and its underlying FILE* for the entire boot.
class GameCourseLoader {
public:
    static constexpr size_t kNameBytes = 16;  // 3..15 ASCII characters
    enum class Choice : uint8_t { Missing, Valid, Invalid };
    enum class Result : uint8_t {
        NotStarted, Selected, Default, FallbackDefault, Builtin,
    };

    // Strict, non-allocating parser; accepts optional LF or CRLF once.
    // Rejects traversal, spaces, non-ASCII and overlong names.
    static bool ParsePackId(const char* text, size_t size,
                            char (&output)[kNameBytes]);

    // Call exactly once, on the original pet_prompt initialization task
    // BEFORE any GameSession/PCM playback. Returns false => builtin questions.
    // The loader owns the one opened file and validated KPG1 for the lifetime
    // of the firmware instance. Calling again does not reload or swap it.
    bool LoadAtBoot(const char* games_directory);

    bool Ready() const { return pack_.Ready(); }
    GamePackReader* Pack() { return Ready() ? &pack_ : nullptr; }
    const GamePackReader* Pack() const { return Ready() ? &pack_ : nullptr; }
    const char* LoadedName() const { return loaded_name_; }
    Result State() const { return result_; }
    Choice Selection() const { return choice_; }
    uint16_t ContentId() const { return pack_.ContentId(); }
    size_t Rounds() const { return pack_.RoundCount(); }
    size_t Clips() const { return pack_.ClipCount(); }

private:
    Choice ReadActive(const char* root, char (&wanted)[kNameBytes]) const;
    bool TryOpen(const char* root, const char* id);

    SoundPackFilePort file_;
    GamePackReader pack_;
    bool started_ = false;
    Choice choice_ = Choice::Missing;
    Result result_ = Result::NotStarted;
    char loaded_name_[kNameBytes] = "builtin";
};

const char* GameCourseResultName(GameCourseLoader::Result result);
const char* GameCourseChoiceName(GameCourseLoader::Choice choice);

}  // namespace kid_app
