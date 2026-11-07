#pragma once

#include <cstddef>
#include <cstdint>

namespace kid_app {

// Strict, bounded, allocation-free JSON reader for the small PetLearning
// server replies (pairing, state sync, commands). Shared by the device and
// the host tests, so the parsing the board runs is the parsing CI tests.
//
// Parse() indexes the text in a fixed node pool; the text must outlive the
// document. Rejected: trailing garbage, nesting deeper than kMaxDepth, raw
// control characters in strings, invalid escapes, non-ASCII \u escapes and
// more nodes than kMaxNodes. Numbers are checked against the JSON grammar;
// Int()/Uint() only accept integer tokens (no fraction or exponent) that fit.
class JsonDoc {
public:
    static constexpr size_t kMaxNodes = 768;
    static constexpr int kMaxDepth = 12;
    enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };

    bool Parse(const char* text, size_t size);
    int Root() const { return count_ ? 0 : -1; }

    Type TypeOf(int node) const;
    bool Is(int node, Type type) const { return Valid(node) && nodes_[node].type == type; }
    // Object member by key (the first one); -1 if missing or not an object.
    int Get(int object, const char* key) const;
    // Array / object children in order; -1 at the end.
    int First(int node) const;
    int Next(int node) const;
    size_t Count(int node) const;

    bool Bool(int node, bool* out) const;
    bool Int(int node, int64_t* out) const;
    bool Uint(int node, uint64_t* out) const;
    // Unescaped string; false if not a string or it does not fit (with NUL).
    bool String(int node, char* out, size_t capacity) const;
    bool StringEquals(int node, const char* value) const;

private:
    struct Node {
        Type type = Type::Null;
        bool flag = false;     // Bool value / Number is an integer token
        int16_t key = -1;      // object member: its key's String node
        int16_t child = -1;
        int16_t next = -1;
        uint32_t start = 0;    // String: after the quote; others: first byte
        uint32_t length = 0;   // String: raw (escaped) bytes inside the quotes
    };

    bool Valid(int node) const { return node >= 0 && static_cast<size_t>(node) < count_; }
    int NewNode(Type type, size_t start);
    void SkipSpace();
    int ParseValue(int depth);
    int ParseString();
    int ParseNumber();
    bool Literal(const char* word);
    // Writes the unescaped string to out (when non-null); returns its length
    // or -1 if it does not fit / is invalid.
    long Unescape(const Node& n, char* out, size_t capacity) const;

    const char* text_ = nullptr;
    size_t size_ = 0;
    size_t pos_ = 0;
    size_t count_ = 0;
    Node nodes_[kMaxNodes];
};

}  // namespace kid_app
