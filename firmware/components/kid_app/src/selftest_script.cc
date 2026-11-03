#include "kid_app/selftest_script.h"

#include <cstring>

namespace kid_app {

namespace {
constexpr size_t kMaxLine = 80;
constexpr size_t kMaxText = 4096;
constexpr const char kHeader[] = "PETSELFTEST1";

// Splits a line into at most four whitespace-separated words.
size_t Words(char* line, char* words[4]) {
    size_t n = 0;
    char* p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p) break;
        if (n == 4) return 5;  // too many fields
        words[n++] = p;
        while (*p && *p != ' ' && *p != '\t') ++p;
        if (*p) *p++ = '\0';
    }
    return n;
}

bool Number(const char* text, uint32_t* out) {
    if (!text || !*text) return false;
    uint64_t value = 0;
    for (const char* p = text; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        value = value * 10 + static_cast<uint64_t>(*p - '0');
        if (value > SelfTestScript::kMaxMs) return false;
    }
    *out = static_cast<uint32_t>(value);
    return true;
}

bool MarkName(const char* name, char out[16]) {
    const size_t len = std::strlen(name);
    if (len == 0 || len > 15) return false;
    for (size_t i = 0; i < len; ++i) {
        const char ch = name[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')) return false;
    }
    std::memcpy(out, name, len + 1);
    return true;
}

bool Key(const char* name, InputKey* out) {
    struct Entry {
        const char* name;
        InputKey key;
    };
    static const Entry kKeys[] = {{"up", InputKey::DialUp},     {"down", InputKey::DialDown},
                                  {"press", InputKey::DialPress}, {"boot", InputKey::Boot},
                                  {"pwr", InputKey::Power}};
    for (const auto& entry : kKeys) {
        if (std::strcmp(name, entry.name) == 0) {
            *out = entry.key;
            return true;
        }
    }
    return false;
}
}  // namespace

bool SelfTestScript::Parse(const char* text) {
    count_ = 0;
    if (!text) return false;
    std::array<SelfTestStep, kMaxSteps> steps{};
    size_t count = 0;
    bool header = false, ended = false;
    const char* p = text;
    size_t consumed = 0;

    while (*p && !ended) {
        char line[kMaxLine + 1];
        size_t len = 0;
        while (p[len] && p[len] != '\n') ++len;
        consumed += len + 1;
        if (len > kMaxLine || consumed > kMaxText + 1) return false;
        std::memcpy(line, p, len);
        line[len] = '\0';
        p += len;
        if (*p == '\n') ++p;
        if (len && line[len - 1] == '\r') line[len - 1] = '\0';

        char* words[4] = {};
        const size_t n = Words(line, words);
        if (n == 0 || words[0][0] == '#') continue;
        if (n > 4) return false;
        if (!header) {
            if (n != 1 || std::strcmp(words[0], kHeader) != 0) return false;
            header = true;
            continue;
        }
        if (std::strcmp(words[0], "end") == 0) {
            if (n != 1) return false;
            ended = true;
            continue;
        }
        if (count == kMaxSteps) return false;
        SelfTestStep step;
        if (std::strcmp(words[0], "wait") == 0) {
            if (n != 2 || !Number(words[1], &step.ms)) return false;
            step.op = SelfTestOp::Wait;
        } else if (std::strcmp(words[0], "mark") == 0) {
            if (n != 2 || !MarkName(words[1], step.name)) return false;
            step.op = SelfTestOp::Mark;
        } else if (std::strcmp(words[0], "pet") == 0) {
            if (n != 1) return false;
            step.op = SelfTestOp::TogglePet;
        } else if (std::strcmp(words[0], "tap") == 0) {
            if (n < 2 || n > 3 || !Key(words[1], &step.key)) return false;
            step.op = SelfTestOp::Hold;
            step.ms = kTapMs;
            step.gap_ms = kDefaultGapMs;
            if (n == 3 && !Number(words[2], &step.gap_ms)) return false;
        } else if (std::strcmp(words[0], "hold") == 0) {
            if (n < 3 || !Key(words[1], &step.key) || !Number(words[2], &step.ms) ||
                step.ms == 0)
                return false;
            step.op = SelfTestOp::Hold;
            step.gap_ms = kDefaultGapMs;
            if (n == 4 && !Number(words[3], &step.gap_ms)) return false;
        } else {
            return false;
        }
        steps[count++] = step;
    }
    if (!header || !ended) return false;
    steps_ = steps;
    count_ = count;
    return true;
}

bool SelfTestScript::ParseSector(const uint8_t* sector, size_t size) {
    count_ = 0;
    if (!sector || size == 0) return false;
    char text[kMaxText + 1];
    const size_t limit = size < kMaxText ? size : kMaxText;
    size_t len = 0;
    while (len < limit && sector[len] != 0x00 && sector[len] != 0xFF) {
        text[len] = static_cast<char>(sector[len]);
        ++len;
    }
    text[len] = '\0';
    return Parse(text);
}

uint64_t SelfTestScript::TotalMs() const {
    uint64_t total = 0;
    for (size_t i = 0; i < count_; ++i) total += steps_[i].ms + steps_[i].gap_ms;
    return total;
}

}  // namespace kid_app
