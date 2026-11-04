#include "kid_app/personality.h"

namespace kid_app {
namespace {
using T = Temperament;
using C = PhraseContext;
using S = PhraseSlot;

// Reviewed phrase library (PRD v0.2 §8.1). id = (temperament + 1) * 100 +
// (context + 1) * 10 + n. Never edit an id's meaning; add new ids instead.
// Memory-slot lines weigh more so remembered things are mentioned when known.
constexpr PhraseTemplate kPhrases[] = {
    {111, T::Curious, C::Greeting, S::None, 1, "咦，你来啦！今天会发现什么新东西呢？"},
    {112, T::Curious, C::Greeting, S::None, 1, "我刚刚在想一个问题，你想听吗？"},
    {121, T::Curious, C::MorningRecap, S::None, 1, "早上好！新的一天，有好多好奇的事等着我们。"},
    {122, T::Curious, C::MorningRecap, S::Book, 3, "早上好！昨天我们读了《{book}》，今天还想读吗？"},
    {131, T::Curious, C::FavoriteMention, S::None, 1, "你最喜欢什么呢？下次用拨轮告诉我好不好？"},
    {132, T::Curious, C::FavoriteMention, S::Like, 3, "我记得你最喜欢{like}！它为什么这么好玩呀？"},
    {141, T::Curious, C::Feed, S::None, 1, "好香！这是什么味道呀？"},
    {142, T::Curious, C::Feed, S::None, 1, "吃饱了，我的脑袋转得更快啦！"},
    {151, T::Curious, C::StoryInvite, S::None, 1, "书里藏着好多秘密，我们一起去找吧！"},
    {152, T::Curious, C::StoryInvite, S::Book, 3, "《{book}》后来会怎样呢？再读一遍吧！"},
    {161, T::Curious, C::Bedtime, S::None, 1, "今天学到好多，明天还要一起探索哦，晚安。"},
    {162, T::Curious, C::Bedtime, S::None, 1, "星星为什么会眨眼呢？我们梦里去问问吧。"},

    {211, T::Gentle, C::Greeting, S::None, 1, "你来啦，我好想你呀。"},
    {212, T::Gentle, C::Greeting, S::None, 1, "抱抱，今天也要开开心心的。"},
    {221, T::Gentle, C::MorningRecap, S::None, 1, "早上好，昨晚睡得好吗？"},
    {222, T::Gentle, C::MorningRecap, S::Book, 3, "早上好，昨天我们一起读了《{book}》，真温暖。"},
    {231, T::Gentle, C::FavoriteMention, S::None, 1, "你喜欢什么，都可以慢慢告诉我。"},
    {232, T::Gentle, C::FavoriteMention, S::Like, 3, "我记得你最喜欢{like}，我也喜欢。"},
    {241, T::Gentle, C::Feed, S::None, 1, "谢谢你喂我，好幸福。"},
    {242, T::Gentle, C::Feed, S::None, 1, "慢慢吃，好好吃饭身体棒。"},
    {251, T::Gentle, C::StoryInvite, S::None, 1, "我们靠在一起，读个故事好吗？"},
    {252, T::Gentle, C::StoryInvite, S::Book, 3, "要不要再听一次《{book}》？"},
    {261, T::Gentle, C::Bedtime, S::None, 1, "该睡觉啦，我陪着你，晚安。"},
    {262, T::Gentle, C::Bedtime, S::None, 1, "闭上眼睛，做个甜甜的梦。"},

    {311, T::Lively, C::Greeting, S::None, 1, "嘿！你来啦！我们玩点什么吧！"},
    {312, T::Lively, C::Greeting, S::None, 1, "哇，我等你好久啦，跳一跳！"},
    {321, T::Lively, C::MorningRecap, S::None, 1, "早上好！今天也要元气满满！"},
    {322, T::Lively, C::MorningRecap, S::Book, 3, "早上好！昨天的《{book}》太好玩啦，再来一次？"},
    {331, T::Lively, C::FavoriteMention, S::None, 1, "你最喜欢什么？我们来比一比！"},
    {332, T::Lively, C::FavoriteMention, S::Like, 3, "我知道！你最喜欢{like}！对不对？"},
    {341, T::Lively, C::Feed, S::None, 1, "开饭啦！吃饱了就有力气玩！"},
    {342, T::Lively, C::Feed, S::None, 1, "好吃好吃，再来一口！"},
    {351, T::Lively, C::StoryInvite, S::None, 1, "讲故事时间到！冲呀！"},
    {352, T::Lively, C::StoryInvite, S::Book, 3, "《{book}》太精彩了，我们再读一遍！"},
    {361, T::Lively, C::Bedtime, S::None, 1, "今天玩得好开心，明天继续，晚安！"},
    {362, T::Lively, C::Bedtime, S::None, 1, "电量用光啦，我们去睡觉吧，晚安！"},
};
constexpr size_t kPhraseTotal = sizeof(kPhrases) / sizeof(kPhrases[0]);

//                        Chat Story Game Feed Sleep
constexpr PersonalityParams kParams[] = {
    {T::Curious, 111, PetMood::Curious, {{2, 4, 2, 1, 0}}},
    {T::Gentle, 211, PetMood::Neutral, {{4, 3, 1, 2, 0}}},
    {T::Lively, 311, PetMood::Happy, {{2, 1, 4, 2, 0}}},
};

// Integer hash so consecutive seeds do not walk the table in order.
uint32_t Mix(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
}  // namespace

bool Personality::Valid(Temperament t) {
    return static_cast<uint8_t>(t) <= static_cast<uint8_t>(Temperament::Lively);
}

const PersonalityParams& Personality::Params(Temperament t) {
    return kParams[Valid(t) ? static_cast<size_t>(t)
                            : static_cast<size_t>(Temperament::Gentle)];
}

uint8_t Personality::ProposalWeight(Temperament t, DayPhase phase, HomeItem item) {
    const size_t i = static_cast<size_t>(item);
    if (!Valid(t) || i >= HomeMenu::kItemCount) return 0;
    const uint8_t base = Params(t).proposal_weights[i];
    switch (phase) {
    case DayPhase::Night:
    case DayPhase::Nap:
        return item == HomeItem::Sleep ? 1 : 0;  // the pet only suggests rest
    case DayPhase::Morning:
        return item == HomeItem::Feed ? static_cast<uint8_t>(base + 2) : base;
    case DayPhase::Day:
        return base;
    case DayPhase::Evening:
        if (item == HomeItem::Story) return static_cast<uint8_t>(base * 2);
        if (item == HomeItem::Game) return 0;  // wind down, no games
        if (item == HomeItem::Sleep) return 2;
        return base;
    }
    return 0;
}

HomeItem Personality::Propose(Temperament t, DayPhase phase, uint32_t seed) {
    unsigned total = 0;
    for (size_t i = 0; i < HomeMenu::kItemCount; ++i)
        total += ProposalWeight(t, phase, static_cast<HomeItem>(i));
    if (!total) return HomeItem::Story;  // invalid temperament / phase
    unsigned pick = Mix(seed) % total;
    for (size_t i = 0; i < HomeMenu::kItemCount; ++i) {
        const auto item = static_cast<HomeItem>(i);
        const unsigned w = ProposalWeight(t, phase, item);
        if (pick < w) return item;
        pick -= w;
    }
    return HomeItem::Story;  // unreachable
}

uint16_t Personality::SelectPhrase(Temperament t, PhraseContext context,
                                   bool has_book, bool has_like, uint32_t seed) {
    if (!Valid(t) || static_cast<size_t>(context) >= kPhraseContextCount) return 0;
    auto eligible = [&](const PhraseTemplate& p) {
        return p.temperament == t && p.context == context &&
               (p.slot == PhraseSlot::None ||
                (p.slot == PhraseSlot::Book && has_book) ||
                (p.slot == PhraseSlot::Like && has_like));
    };
    unsigned total = 0;
    for (const auto& p : kPhrases)
        if (eligible(p)) total += p.weight;
    if (!total) return 0;
    unsigned pick = Mix(seed ^ (static_cast<uint32_t>(context) << 24)) % total;
    for (const auto& p : kPhrases) {
        if (!eligible(p)) continue;
        if (pick < p.weight) return p.id;
        pick -= p.weight;
    }
    return 0;
}

uint16_t Personality::SelectPhrase(const MemoryStore& memory, PhraseContext context,
                                   uint32_t current_day, uint32_t seed) {
    const bool has_book = context == PhraseContext::MorningRecap
        ? current_day > 1 && memory.BookReadOn(current_day - 1) != 0
        : memory.RecentBookCount() > 0;
    return SelectPhrase(memory.GetTemperament(), context, has_book,
                        memory.PreferenceCount() > 0, seed);
}

const PhraseTemplate* Personality::FindPhrase(uint16_t id) {
    for (const auto& p : kPhrases)
        if (p.id == id) return &p;
    return nullptr;
}

size_t Personality::PhraseCount() { return kPhraseTotal; }

const PhraseTemplate& Personality::PhraseAt(size_t index) {
    return kPhrases[index < kPhraseTotal ? index : 0];
}

}  // namespace kid_app
