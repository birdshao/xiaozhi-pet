#include "kid_app/json_lite.h"

#include <cstring>

namespace kid_app {
namespace {

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

int JsonDoc::NewNode(Type type, size_t start) {
    if (count_ >= kMaxNodes) return -1;
    Node& n = nodes_[count_];
    n = Node{};
    n.type = type;
    n.start = static_cast<uint32_t>(start);
    return static_cast<int>(count_++);
}

void JsonDoc::SkipSpace() {
    while (pos_ < size_) {
        const char c = text_[pos_];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return;
        ++pos_;
    }
}

bool JsonDoc::Literal(const char* word) {
    const size_t n = std::strlen(word);
    if (size_ - pos_ < n || std::memcmp(text_ + pos_, word, n) != 0) return false;
    pos_ += n;
    return true;
}

int JsonDoc::ParseString() {
    // pos_ is at the opening quote.
    const int index = NewNode(Type::String, pos_ + 1);
    if (index < 0) return -1;
    ++pos_;
    while (pos_ < size_) {
        const unsigned char c = static_cast<unsigned char>(text_[pos_]);
        if (c == '"') {
            nodes_[index].length = static_cast<uint32_t>(pos_ - nodes_[index].start);
            ++pos_;
            return index;
        }
        if (c < 0x20) return -1;
        if (c == '\\') {
            if (++pos_ >= size_) return -1;
            const char e = text_[pos_];
            if (e == 'u') {
                if (size_ - pos_ < 5) return -1;
                int value = 0;
                for (int i = 1; i <= 4; ++i) {
                    const int h = HexValue(text_[pos_ + i]);
                    if (h < 0) return -1;
                    value = value * 16 + h;
                }
                if (value < 0x20 || value > 0x7E) return -1;  // ASCII only
                pos_ += 4;
            } else if (!std::strchr("\"\\/bfnrt", e) || e == '\0') {
                return -1;
            }
        }
        ++pos_;
    }
    return -1;
}

int JsonDoc::ParseNumber() {
    const size_t start = pos_;
    bool integer = true;
    if (pos_ < size_ && text_[pos_] == '-') ++pos_;
    if (pos_ >= size_ || !IsDigit(text_[pos_])) return -1;
    if (text_[pos_] == '0') {
        ++pos_;
    } else {
        while (pos_ < size_ && IsDigit(text_[pos_])) ++pos_;
    }
    if (pos_ < size_ && text_[pos_] == '.') {
        integer = false;
        ++pos_;
        if (pos_ >= size_ || !IsDigit(text_[pos_])) return -1;
        while (pos_ < size_ && IsDigit(text_[pos_])) ++pos_;
    }
    if (pos_ < size_ && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
        integer = false;
        ++pos_;
        if (pos_ < size_ && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
        if (pos_ >= size_ || !IsDigit(text_[pos_])) return -1;
        while (pos_ < size_ && IsDigit(text_[pos_])) ++pos_;
    }
    const int index = NewNode(Type::Number, start);
    if (index < 0) return -1;
    nodes_[index].flag = integer;
    nodes_[index].length = static_cast<uint32_t>(pos_ - start);
    return index;
}

int JsonDoc::ParseValue(int depth) {
    if (depth > kMaxDepth) return -1;
    SkipSpace();
    if (pos_ >= size_) return -1;
    const char c = text_[pos_];
    if (c == '"') return ParseString();
    if (c == '-' || IsDigit(c)) return ParseNumber();
    if (c == 't' || c == 'f' || c == 'n') {
        const size_t start = pos_;
        Type type = Type::Bool;
        bool value = false;
        if (Literal("true")) {
            value = true;
        } else if (Literal("false")) {
            value = false;
        } else if (Literal("null")) {
            type = Type::Null;
        } else {
            return -1;
        }
        const int index = NewNode(type, start);
        if (index < 0) return -1;
        nodes_[index].flag = value;
        nodes_[index].length = static_cast<uint32_t>(pos_ - start);
        return index;
    }
    if (c != '{' && c != '[') return -1;
    const bool object = c == '{';
    const int index = NewNode(object ? Type::Object : Type::Array, pos_);
    if (index < 0) return -1;
    ++pos_;
    SkipSpace();
    const char close = object ? '}' : ']';
    if (pos_ < size_ && text_[pos_] == close) {
        ++pos_;
        return index;
    }
    int last = -1;
    for (;;) {
        int key = -1;
        if (object) {
            SkipSpace();
            if (pos_ >= size_ || text_[pos_] != '"') return -1;
            key = ParseString();
            if (key < 0) return -1;
            SkipSpace();
            if (pos_ >= size_ || text_[pos_] != ':') return -1;
            ++pos_;
        }
        const int value = ParseValue(depth + 1);
        if (value < 0) return -1;
        nodes_[value].key = static_cast<int16_t>(key);
        if (last < 0) nodes_[index].child = static_cast<int16_t>(value);
        else nodes_[last].next = static_cast<int16_t>(value);
        last = value;
        SkipSpace();
        if (pos_ >= size_) return -1;
        if (text_[pos_] == ',') {
            ++pos_;
            continue;
        }
        if (text_[pos_] != close) return -1;
        ++pos_;
        return index;
    }
}

bool JsonDoc::Parse(const char* text, size_t size) {
    text_ = text;
    size_ = text ? size : 0;
    pos_ = 0;
    count_ = 0;
    if (!text || !size) return false;
    const int root = ParseValue(0);
    SkipSpace();
    if (root != 0 || pos_ != size_) {
        count_ = 0;
        return false;
    }
    return true;
}

JsonDoc::Type JsonDoc::TypeOf(int node) const {
    return Valid(node) ? nodes_[node].type : Type::Null;
}

int JsonDoc::Get(int object, const char* key) const {
    if (!Is(object, Type::Object) || !key) return -1;
    for (int child = nodes_[object].child; child >= 0; child = nodes_[child].next) {
        if (StringEquals(nodes_[child].key, key)) return child;
    }
    return -1;
}

int JsonDoc::First(int node) const {
    if (!Is(node, Type::Array) && !Is(node, Type::Object)) return -1;
    return nodes_[node].child;
}

int JsonDoc::Next(int node) const {
    return Valid(node) ? nodes_[node].next : -1;
}

size_t JsonDoc::Count(int node) const {
    size_t n = 0;
    for (int child = First(node); child >= 0; child = Next(child)) ++n;
    return n;
}

bool JsonDoc::Bool(int node, bool* out) const {
    if (!out || !Is(node, Type::Bool)) return false;
    *out = nodes_[node].flag;
    return true;
}

bool JsonDoc::Int(int node, int64_t* out) const {
    if (!out || !Is(node, Type::Number) || !nodes_[node].flag) return false;
    const Node& n = nodes_[node];
    const char* p = text_ + n.start;
    const char* end = p + n.length;
    const bool negative = *p == '-';
    if (negative) ++p;
    // Accumulate as a negative number so INT64_MIN fits.
    int64_t value = 0;
    constexpr int64_t kMin = INT64_MIN;
    for (; p < end; ++p) {
        const int digit = *p - '0';
        if (value < (kMin + digit) / 10) return false;
        value = value * 10 - digit;
    }
    if (!negative) {
        if (value == kMin) return false;
        value = -value;
    }
    *out = value;
    return true;
}

bool JsonDoc::Uint(int node, uint64_t* out) const {
    if (!out || !Is(node, Type::Number) || !nodes_[node].flag) return false;
    const Node& n = nodes_[node];
    const char* p = text_ + n.start;
    const char* end = p + n.length;
    if (*p == '-') return false;
    uint64_t value = 0;
    for (; p < end; ++p) {
        const uint64_t digit = static_cast<uint64_t>(*p - '0');
        if (value > (UINT64_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    *out = value;
    return true;
}

long JsonDoc::Unescape(const Node& n, char* out, size_t capacity) const {
    const char* p = text_ + n.start;
    const char* end = p + n.length;
    size_t used = 0;
    while (p < end) {
        char c = *p++;
        if (c == '\\') {
            const char e = *p++;
            switch (e) {
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'u': {
                    int value = 0;
                    for (int i = 0; i < 4; ++i) value = value * 16 + HexValue(*p++);
                    c = static_cast<char>(value);
                    break;
                }
                default: c = e; break;  // \" \\ \/ (validated by ParseString)
            }
        }
        if (out) {
            if (used + 1 >= capacity) return -1;
            out[used] = c;
        }
        ++used;
    }
    if (out) out[used] = '\0';
    return static_cast<long>(used);
}

bool JsonDoc::String(int node, char* out, size_t capacity) const {
    if (!out || !capacity || !Is(node, Type::String)) return false;
    return Unescape(nodes_[node], out, capacity) >= 0;
}

bool JsonDoc::StringEquals(int node, const char* value) const {
    if (!value || !Is(node, Type::String)) return false;
    const Node& n = nodes_[node];
    const size_t want = std::strlen(value);
    // Fast path: no escapes in the raw text.
    if (n.length == want && std::memcmp(text_ + n.start, value, want) == 0) return true;
    if (!std::memchr(text_ + n.start, '\\', n.length)) return false;
    char buffer[128];
    if (want >= sizeof(buffer)) return false;
    const long got = Unescape(n, buffer, sizeof(buffer));
    return got >= 0 && static_cast<size_t>(got) == want && std::memcmp(buffer, value, want) == 0;
}

}  // namespace kid_app
