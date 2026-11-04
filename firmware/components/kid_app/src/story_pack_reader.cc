#include "kid_app/story_pack_reader.h"
#include "kid_app/scene_model.h"

namespace kid_app {

bool StoryPackReader::Page(uint16_t number, StoryPageInfo* out) const {
    ContentPage p{};
    if (!out || !content_.Page(number, &p)) return false;
    StoryPageInfo info{};
    info.index = p.number;
    info.image_offset = p.mono.offset;
    info.image_bytes = p.mono.bytes;
    info.image_crc = p.mono.crc;
    info.pcm_offset = p.pcm.offset;
    info.pcm_bytes = p.pcm.bytes;
    info.pcm_crc = p.pcm.crc;
    info.sample_rate = p.sample_rate;
    info.channels = p.channels;
    info.bits = p.bits;
    info.text_offset = p.text.offset;
    info.text_bytes = p.text.bytes;
    info.text_crc = p.text.crc;
    *out = info;
    return true;
}

bool StoryPackReader::ReadFrame(uint16_t page, MonoFrame& frame) {
    return content_.ReadMono(page, frame.pixels_.data(), frame.pixels_.size());
}

}  // namespace kid_app
