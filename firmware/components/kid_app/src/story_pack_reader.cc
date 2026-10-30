#include "kid_app/story_pack_reader.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace kid_app {

uint16_t StoryPackReader::U16(const uint8_t* d) {
    return static_cast<uint16_t>(d[0] | (uint16_t(d[1]) << 8));
}
uint32_t StoryPackReader::U32(const uint8_t* d) {
    return uint32_t(d[0]) | (uint32_t(d[1]) << 8) |
           (uint32_t(d[2]) << 16) | (uint32_t(d[3]) << 24);
}
uint32_t StoryPackReader::AdvanceChecksum(
        uint32_t crc, const uint8_t* data, size_t bytes) {
    for (size_t i=0;i<bytes;++i) {
        crc ^= data[i];
        for (unsigned k=0;k<8;++k)
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
    }
    return crc;
}
uint32_t StoryPackReader::Checksum(const uint8_t* data, size_t size) {
    return ~AdvanceChecksum(0xFFFFFFFFu,data,size);
}
bool StoryPackReader::Verified(StoryPackFile& file, uint32_t off,
                               uint32_t bytes, uint32_t crc) {
    std::array<uint8_t,512> chunk{};
    uint32_t next=0xFFFFFFFFu;
    uint32_t done=0;
    while (done<bytes) {
        const size_t n=std::min<size_t>(chunk.size(),bytes-done);
        if (!file.ReadAt(off+done,chunk.data(),n)) return false;
        next=AdvanceChecksum(next,chunk.data(),n);
        done+=static_cast<uint32_t>(n);
    }
    return ~next==crc;
}

bool StoryPackReader::Open(StoryPackFile& file) {
    file_=nullptr;
    count_=0;
    pages_={};
    std::array<uint8_t,32> head{};
    if (file.Size() < head.size() || file.Size() > kMaxBytes ||
        !file.ReadAt(0,head.data(),head.size())) return false;

    if (std::memcmp(head.data(),"KPB1",4)!=0 ||
        U16(head.data()+4)!=1 ||
        U16(head.data()+6)!=kWidth ||
        U16(head.data()+8)!=kHeight ||
        Checksum(head.data(),28)!=U32(head.data()+28))
        return false;
    const uint16_t count=U16(head.data()+10);
    const uint32_t toc=U32(head.data()+12);
    const uint32_t toc_bytes=U32(head.data()+16);
    const uint32_t first_data=U32(head.data()+20);
    const uint32_t total=U32(head.data()+24);
    if (count==0 || count>kMaxPages || toc!=head.size() ||
        toc_bytes!=count*64u || first_data!=toc+toc_bytes ||
        total!=file.Size() || total<first_data) return false;

    std::array<StoryPageInfo,kMaxPages> candidate{};
    uint32_t previous_end=first_data;
    for (uint32_t i=0;i<count;++i) {
        std::array<uint8_t,64> record{};
        if (!file.ReadAt(toc+i*64u,record.data(),record.size()))
            return false;
        const uint8_t* r=record.data();
        StoryPageInfo p{};
        p.index=U16(r);
        p.image_offset=U32(r+4);
        p.image_bytes=U32(r+8);
        p.image_crc=U32(r+12);
        p.pcm_offset=U32(r+16);
        p.pcm_bytes=U32(r+20);
        p.pcm_crc=U32(r+24);
        p.sample_rate=U32(r+28);
        p.channels=U16(r+32);
        p.bits=U16(r+34);
        p.text_offset=U32(r+36);
        p.text_bytes=U32(r+40);
        p.text_crc=U32(r+44);
        if (U16(r+2)!=0 ||
            std::any_of(r+48,r+64,[](uint8_t v){return v!=0;}) ||
            p.index!=i+1 || p.image_bytes!=kImageBytes ||
            p.sample_rate!=16000 || p.channels!=1 || p.bits!=16 ||
            p.pcm_bytes<2 || p.pcm_bytes>16000u*2*60 ||
            (p.pcm_bytes&1u)!=0 ||
            p.text_bytes==0 || p.text_bytes>1280 ||
            (p.image_offset&3u)!=0 || (p.pcm_offset&3u)!=0 ||
            (p.text_offset&3u)!=0)
            return false;
        const uint64_t image_end=uint64_t(p.image_offset)+p.image_bytes;
        const uint64_t pcm_end=uint64_t(p.pcm_offset)+p.pcm_bytes;
        const uint64_t text_end=uint64_t(p.text_offset)+p.text_bytes;
        if (p.image_offset<previous_end || p.image_offset>uint64_t(previous_end)+3 ||
            image_end>total ||
            p.pcm_offset<image_end || p.pcm_offset>image_end+3 ||
            pcm_end>total ||
            p.text_offset<pcm_end || p.text_offset>pcm_end+3 ||
            text_end>total) return false;
        // No content is trusted until its full CRC is verified.
        if (!Verified(file,p.image_offset,p.image_bytes,p.image_crc) ||
            !Verified(file,p.pcm_offset,p.pcm_bytes,p.pcm_crc) ||
            !Verified(file,p.text_offset,p.text_bytes,p.text_crc))
            return false;
        candidate[i]=p;
        previous_end=static_cast<uint32_t>(text_end);
    }
    if (previous_end!=total) return false;
    pages_=candidate;
    count_=count;
    file_=&file;
    return true;
}
bool StoryPackReader::Page(uint16_t number, StoryPageInfo* out) const {
    if (!Ready() || !out || number<1 || number>count_) return false;
    *out=pages_[number-1];
    return true;
}
bool StoryPackReader::ReadImage(uint16_t page, uint8_t* target, size_t size) {
    StoryPageInfo p{};
    return Page(page,&p) && target && size>=kImageBytes &&
           file_->ReadAt(p.image_offset,target,kImageBytes);
}
bool StoryPackReader::ReadPcm(uint16_t page, uint32_t position,
                              void* target, size_t bytes) {
    StoryPageInfo p{};
    if (!Page(page,&p) || !target || bytes==0 ||
        (position&1u) || (bytes&1u) ||
        uint64_t(position)+bytes>p.pcm_bytes) return false;
    return file_->ReadAt(p.pcm_offset+position,target,bytes);
}
bool StoryPackReader::ReadText(uint16_t page, char* target, size_t capacity) {
    StoryPageInfo p{};
    if (!Page(page,&p) || !target || capacity<=p.text_bytes) return false;
    if (!file_->ReadAt(p.text_offset,target,p.text_bytes)) return false;
    target[p.text_bytes]='\0';
    return true;
}

}  // namespace kid_app
