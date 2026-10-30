#include "kid_app/story_pack_file_port.h"
#include "kid_app/story_pack_reader.h"
#include "kid_app/scene_model.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

using namespace kid_app;

class MemoryFile final : public StoryPackFile {
public:
    explicit MemoryFile(std::vector<uint8_t> b) : bytes(std::move(b)) {}
    uint64_t Size() const override { return bytes.size(); }
    bool ReadAt(uint32_t pos, void* dst, size_t size) override {
        if (!dst || uint64_t(pos)+size>bytes.size() || fail_read) return false;
        std::copy_n(bytes.data()+pos,size,static_cast<uint8_t*>(dst));
        return true;
    }
    std::vector<uint8_t> bytes;
    bool fail_read=false;
};

int main(int argc, char** argv) {
    assert(argc==3);
    const std::string base=argv[1];
    const std::string book=argv[2];
    StoryPackFilePort port{};
    assert(!port.OpenBook(base.c_str(),"../../etc/passwd"));
    assert(!port.OpenBook(base.c_str(),"bad-id!"));
    assert(!port.OpenBook("../relative","test_book"));
    assert(port.OpenBook(base.c_str(),book.c_str()));

    StoryPackReader parser{};
    assert(parser.Open(port));
    assert(parser.PageCount()==2);
    assert(parser.Ready());
    StoryPageInfo info{};
    assert(parser.Page(1,&info));
    assert(info.image_bytes==StoryPackReader::kImageBytes);
    assert(info.sample_rate==24000 && info.channels==1 && info.bits==16);
    assert(info.pcm_bytes==12000);
    assert(!parser.Page(0,&info));
    assert(!parser.Page(3,&info));
    std::vector<uint8_t> pixels(StoryPackReader::kImageBytes,0);
    assert(!parser.ReadImage(1,pixels.data(),pixels.size()-1));
    assert(parser.ReadImage(1,pixels.data(),pixels.size()));
    assert(pixels[0]==0x00); // fixture draws a black pixel at (0,0).
    assert(pixels[1]!=0x00); // neighboring pixel is white.
    assert(parser.ReadImage(2,pixels.data(),pixels.size()));
    MonoFrame screen;
    assert(parser.ReadFrame(1,screen));
    assert(screen.IsBlack(0,0));
    assert(!screen.IsBlack(8,0));

    char text[1280]{};
    assert(parser.ReadText(1,text,sizeof(text)));
    assert(std::string(text)=="小兔子遇见了小鸟。");
    assert(parser.ReadText(2,text,sizeof(text)));
    assert(std::string(text)=="小鸟说你好。");
    std::array<uint8_t,512> audio{};
    assert(parser.ReadPcm(1,0,audio.data(),audio.size()));
    assert(parser.ReadPcm(1,info.pcm_bytes-2,audio.data(),2));
    assert(!parser.ReadPcm(1,1,audio.data(),audio.size()));
    assert(!parser.ReadPcm(1,info.pcm_bytes,audio.data(),2));

    const auto file_name=base+"/"+book+".kpb";
    std::ifstream input(file_name,std::ios::binary);
    assert(input);
    std::vector<uint8_t> raw((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
    assert(raw.size()==port.Size());
    MemoryFile mem(raw);
    StoryPackReader memory_reader{};
    assert(memory_reader.Open(mem));
    mem.bytes[34]^=0x01; // TOC page number corrupted
    assert(!memory_reader.Open(mem));
    assert(!memory_reader.Ready());
    MemoryFile broken_audio(raw);
    // first page audio offset is stored at 32-byte header + 16-byte entry pos
    const uint32_t pcm_offset=uint32_t(raw[48]) |
         (uint32_t(raw[49])<<8) | (uint32_t(raw[50])<<16) |
         (uint32_t(raw[51])<<24);
    broken_audio.bytes.at(pcm_offset+5)^=0x40;
    assert(!memory_reader.Open(broken_audio)); // audio CRC mismatch
    MemoryFile truncated(raw);
    truncated.bytes.pop_back();
    assert(!memory_reader.Open(truncated));
    MemoryFile unreadable(raw);
    unreadable.fail_read=true;
    assert(!memory_reader.Open(unreadable));

    port.Close();
    assert(!parser.ReadImage(1,pixels.data(),pixels.size()));
    std::cout<<"PASS: SD KPB1 2-page image/audio/text, strict TOC/CRC and bad paths\n";
}
