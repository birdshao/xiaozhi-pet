#include "kid_app/pet_phrases.h"

namespace kid_app {

const char* PhraseContextName(PhraseContext context) {
    switch (context) {
        case PhraseContext::Greeting: return "greeting";
        case PhraseContext::MorningRecap: return "morning";
        case PhraseContext::FavoriteMention: return "favorite";
        case PhraseContext::Feed: return "feed";
        case PhraseContext::StoryInvite: return "story";
        case PhraseContext::Bedtime: return "bedtime";
        case PhraseContext::Giggle: return "giggle";
        case PhraseContext::HotTip: return "hot_tip";
        case PhraseContext::ColdTip: return "cold_tip";
    }
    return "none";
}

bool RecordablePhrase(uint16_t id) {
    const PhraseTemplate* p = Personality::FindPhrase(id);
    return p && p->slot == PhraseSlot::None;
}

uint16_t PhrasePlanner::Next(PhraseContext context, uint32_t now_ms) {
    const size_t c = static_cast<size_t>(context);
    if (c >= kPhraseContextCount) return 0;
    const bool always = context == PhraseContext::MorningRecap ||
                        context == PhraseContext::Bedtime;
    if (!always && spoke_ && now_ms - last_ms_ < kMinGapMs) return 0;
    const uint16_t id = Personality::SelectPhrase(temperament_, context, false, false, said_[c]);
    if (!id) return 0;
    ++said_[c];
    spoke_ = true;
    last_ms_ = now_ms;
    return id;
}

}  // namespace kid_app
