#pragma once

#include "kid_app/wav_clip.h"

#include <cstdint>

namespace kid_app {

// Home-rail option prompt clips embedded in the app image (CMake
// EMBED_FILES assets/prompts/prompt_<item>.wav, HomeItem order). ESP-IDF
// build only. False if the index is out of range or a clip is malformed.
bool LoadPromptClip(uint8_t index, WavClip* clip);

}  // namespace kid_app
