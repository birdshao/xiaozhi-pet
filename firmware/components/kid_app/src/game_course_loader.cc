#include "kid_app/game_course_loader.h"

#include <cstdio>
#include <cstring>

namespace kid_app {
namespace {
constexpr char kDefault[] = "default";
constexpr char kSelectionFile[] = "active.txt";

bool ValidRoot(const char* base) {
    if (!base) return false;
    const size_t n = std::strlen(base);
    return n >= 2 && n <= 150 && base[0] == '/' &&
           std::strstr(base, "..") == nullptr && std::strchr(base, '\\') == nullptr;
}
}  // namespace

bool GameCourseLoader::ParsePackId(const char* text, size_t bytes,
                                   char (&output)[kNameBytes]) {
    output[0] = '\0';
    if (!text || bytes == 0 || bytes > kNameBytes + 2) return false;
    // Trim exactly one line ending, not arbitrary whitespace. The file is
    // a single pack basename, never a path or a multi-line program.
    if (bytes && text[bytes - 1] == '\n') {
        --bytes;
        if (bytes && text[bytes - 1] == '\r') --bytes;
    }
    if (bytes < 3 || bytes >= kNameBytes) return false;
    for (size_t i = 0; i < bytes; ++i) {
        const char c = text[i];
        const bool alpha = c >= 'a' && c <= 'z';
        const bool digit = c >= '0' && c <= '9';
        const bool delimiter = i > 0 && (c == '_' || c == '-');
        if (!alpha && !digit && !delimiter) return false;
    }
    std::memcpy(output, text, bytes);
    output[bytes] = '\0';
    return true;
}

GameCourseLoader::Choice GameCourseLoader::ReadActive(
        const char* root, char (&wanted)[kNameBytes]) const {
    char file_name[192]{};
    const int n = std::snprintf(file_name, sizeof(file_name), "%s/%s",
                                root, kSelectionFile);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(file_name))
        return Choice::Invalid;
    FILE* input = std::fopen(file_name, "rb");
    if (!input) return Choice::Missing;
    // A valid line is max 17 bytes (15 ASCII + CRLF); reading 32 detects
    // oversized or multi-line config without any dynamic allocation.
    char buffer[32]{};
    const size_t len = std::fread(buffer, 1, sizeof(buffer), input);
    const bool failed = std::ferror(input) != 0;
    const bool ended = std::feof(input) != 0;
    std::fclose(input);
    if (failed || !ended || !ParsePackId(buffer, len, wanted))
        return Choice::Invalid;
    return Choice::Valid;
}

bool GameCourseLoader::TryOpen(const char* root, const char* id) {
    pack_.Close();
    file_.Close();
    if (!file_.Open(root, id, "kpg")) return false;
    if (!pack_.Open(file_)) {
        file_.Close();
        return false;
    }
    std::snprintf(loaded_name_, sizeof(loaded_name_), "%s", id);
    return true;
}

bool GameCourseLoader::LoadAtBoot(const char* games_directory) {
    if (started_) return Ready();  // never replace an in-use FILE*
    started_ = true;
    if (!ValidRoot(games_directory)) {
        result_ = Result::Builtin;
        return false;
    }
    char selected[kNameBytes] = "default";
    choice_ = ReadActive(games_directory, selected);
    if (choice_ != Choice::Valid)
        std::snprintf(selected, sizeof(selected), "%s", kDefault);

    if (TryOpen(games_directory, selected)) {
        result_ = (std::strcmp(selected, kDefault) == 0)
                  ? Result::Default : Result::Selected;
        return true;
    }
    if (std::strcmp(selected, kDefault) != 0 &&
        TryOpen(games_directory, kDefault)) {
        result_ = Result::FallbackDefault;
        return true;
    }
    result_ = Result::Builtin;
    return false;
}

const char* GameCourseResultName(GameCourseLoader::Result result) {
    switch (result) {
        case GameCourseLoader::Result::NotStarted: return "not_started";
        case GameCourseLoader::Result::Selected: return "selected";
        case GameCourseLoader::Result::Default: return "default";
        case GameCourseLoader::Result::FallbackDefault: return "fallback_default";
        case GameCourseLoader::Result::Builtin: return "builtin";
    }
    return "unknown";
}

const char* GameCourseChoiceName(GameCourseLoader::Choice choice) {
    switch (choice) {
        case GameCourseLoader::Choice::Missing: return "missing";
        case GameCourseLoader::Choice::Valid: return "valid";
        case GameCourseLoader::Choice::Invalid: return "invalid";
    }
    return "unknown";
}

}  // namespace kid_app
