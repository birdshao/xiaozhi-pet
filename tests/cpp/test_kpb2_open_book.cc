// Opens one pipeline-built KPB2 book with the real device reader and checks
// that its last (generated credits) page is readable and not blank.
// Usage: test_kpb2_open_book <book.kpb> <expected page count>
#include "kid_app/content_pack_reader.h"
#include "kid_app/story_pack_file_port.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

using namespace kid_app;

namespace {

class MemoryFile final : public StoryPackFile {
public:
    explicit MemoryFile(std::vector<uint8_t> b) : bytes(std::move(b)) {}
    uint64_t Size() const override { return bytes.size(); }
    bool ReadAt(uint32_t pos, void* dst, size_t size) override {
        if (!dst || uint64_t(pos) + size > bytes.size()) return false;
        std::copy_n(bytes.data() + pos, size, static_cast<uint8_t*>(dst));
        return true;
    }
    std::vector<uint8_t> bytes;
};

int Fail(const char* what) {
    std::cerr << "FAIL: " << what << "\n";
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) return Fail("usage: <book.kpb> <expected pages>");
    std::ifstream in(argv[1], std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    MemoryFile file(std::move(bytes));
    ContentPackReader reader;
    if (!reader.Open(file)) return Fail("Open (header/table/CRC validation)");
    if (reader.Format() != PackFormat::Kpb2) return Fail("not KPB2");
    const uint16_t expected = static_cast<uint16_t>(std::atoi(argv[2]));
    if (reader.PageCount() != expected) return Fail("page count");

    const uint16_t last = reader.PageCount();
    ContentPage page;
    if (!reader.Page(last, &page) || !page.has_grey) return Fail("credits page entry");
    std::vector<uint8_t> grey(ContentPackReader::kGreyBytes);
    if (!reader.ReadGrey(last, grey.data(), grey.size())) return Fail("credits grey frame");
    size_t inked = 0;  // 2-bit pixels, 4 per byte; 3 = white
    for (uint8_t b : grey) {
        for (int shift = 0; shift < 8; shift += 2) inked += ((b >> shift) & 3u) != 3u;
    }
    if (inked < 12000) return Fail("credits page image is blank");  // frame alone is ~9,700
    char text[1300] = {};
    if (!reader.ReadPageText(last, text, sizeof(text)) || text[0] == '\0')
        return Fail("credits page text");
    std::cout << "PASS: KPB2 book with credits page opened; inked pixels=" << inked << "\n";
    return 0;
}
