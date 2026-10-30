#include "kid_app/story_pack_file_port.h"

#include <cstdio>
#include <cstring>
#include <limits>

namespace kid_app {
namespace {
bool ValidBookId(const char* value) {
    if (!value) return false;
    size_t length=0;
    for (;length<64 && value[length];++length) {
        const char c=value[length];
        if (!((c>='a'&&c<='z') || (c>='0'&&c<='9') ||
              (length>0 && (c=='_'||c=='-')))) return false;
    }
    return length>=3 && length<64;
}
}

bool StoryPackFilePort::OpenBook(const char* base, const char* book_id) {
    Close();
    if (!base || !ValidBookId(book_id) || !*base) return false;
    const size_t root_len=std::strlen(base);
    if (root_len<2 || root_len>150 || base[0]!='/' ||
        std::strstr(base,"..") || std::strchr(base,'\\')) return false;
    char full[256]{};
    const int n=std::snprintf(full,sizeof(full),"%s/%s.kpb",base,book_id);
    if (n<1 || static_cast<size_t>(n)>=sizeof(full)) return false;
    FILE* fp=std::fopen(full,"rb");
    if (!fp) return false;
    if (std::fseek(fp,0,SEEK_END)!=0) {
        std::fclose(fp);
        return false;
    }
    const long size=std::ftell(fp);
    if (size<32 || static_cast<uint64_t>(size)>StoryPackReader::kMaxBytes) {
        std::fclose(fp);
        return false;
    }
    file_=fp;
    size_=static_cast<uint64_t>(size);
    return true;
}

void StoryPackFilePort::Close() {
    if (file_) std::fclose(file_);
    file_=nullptr;
    size_=0;
}

bool StoryPackFilePort::ReadAt(uint32_t offset, void* out, size_t count) {
    if (!file_ || !out || count==0 ||
        uint64_t(offset)+count>size_ ||
        offset>static_cast<uint32_t>(std::numeric_limits<long>::max()))
        return false;
    return std::fseek(file_,static_cast<long>(offset),SEEK_SET)==0 &&
           std::fread(out,1,count,file_)==count;
}

}  // namespace kid_app
