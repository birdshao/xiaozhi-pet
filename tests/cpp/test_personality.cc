#include "kid_app/personality.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <set>
#include <string>

using namespace kid_app;
using T = Temperament;
using C = PhraseContext;

namespace {
constexpr T kAll[] = {T::Curious, T::Gentle, T::Lively};
constexpr DayPhase kPhases[] = {DayPhase::Night, DayPhase::Morning, DayPhase::Day,
                                DayPhase::Nap, DayPhase::Evening};

void TestReviewedPhraseTable() {
    std::set<uint16_t> ids;
    for (size_t i = 0; i < Personality::PhraseCount(); ++i) {
        const auto& p = Personality::PhraseAt(i);
        assert(ids.insert(p.id).second);  // stable unique ids
        const unsigned id = p.id;
        assert(id / 100 == static_cast<unsigned>(p.temperament) + 1);
        assert((id / 10) % 10 == static_cast<unsigned>(p.context) + 1);
        assert(p.weight > 0 && p.text && std::strlen(p.text) > 0);
        const std::string text(p.text);
        const bool book = text.find("{book}") != std::string::npos;
        const bool like = text.find("{like}") != std::string::npos;
        assert(book == (p.slot == PhraseSlot::Book));
        assert(like == (p.slot == PhraseSlot::Like));
        assert(Personality::FindPhrase(p.id) == &p);
    }
    assert(!Personality::FindPhrase(0) && !Personality::FindPhrase(999));
    // Every temperament x context has a slot-free fallback line.
    for (T t : kAll) {
        for (size_t c = 0; c < kPhraseContextCount; ++c) {
            bool fallback = false;
            for (size_t i = 0; i < Personality::PhraseCount(); ++i) {
                const auto& p = Personality::PhraseAt(i);
                fallback |= p.temperament == t && p.context == static_cast<C>(c) &&
                            p.slot == PhraseSlot::None;
            }
            assert(fallback);
        }
        const auto& params = Personality::Params(t);
        assert(params.temperament == t);
        const auto* catchphrase = Personality::FindPhrase(params.catchphrase_id);
        assert(catchphrase && catchphrase->temperament == t &&
               catchphrase->context == C::Greeting);
    }
    assert(Personality::Params(static_cast<T>(7)).temperament == T::Gentle);
    assert(Personality::Params(T::Curious).resting_mood == PetMood::Curious);
}

void TestSameInputSameOutput() {
    for (T t : kAll) {
        for (DayPhase ph : kPhases) {
            for (uint32_t seed = 0; seed < 500; ++seed) {
                assert(Personality::Propose(t, ph, seed) == Personality::Propose(t, ph, seed));
            }
        }
        for (size_t c = 0; c < kPhraseContextCount; ++c) {
            for (uint32_t seed = 0; seed < 500; ++seed) {
                const auto a = Personality::SelectPhrase(t, static_cast<C>(c), true, true, seed);
                assert(a && a == Personality::SelectPhrase(t, static_cast<C>(c), true, true, seed));
            }
        }
    }
    // Golden values pin the hash and tables across compilers and releases.
    assert(Personality::Propose(T::Curious, DayPhase::Day, 0) == HomeItem::Chat);
    assert(Personality::Propose(T::Gentle, DayPhase::Day, 1) == HomeItem::Chat);
    assert(Personality::Propose(T::Lively, DayPhase::Morning, 2) == HomeItem::Game);
    assert(Personality::SelectPhrase(T::Curious, C::Greeting, false, false, 0) == 111);
    assert(Personality::SelectPhrase(T::Lively, C::MorningRecap, true, false, 3) == 322);
}

void TestProposalWeightsByPhase() {
    for (T t : kAll) {
        for (uint32_t seed = 0; seed < 300; ++seed) {
            assert(Personality::Propose(t, DayPhase::Night, seed) == HomeItem::Sleep);
            assert(Personality::Propose(t, DayPhase::Nap, seed) == HomeItem::Sleep);
            assert(Personality::Propose(t, DayPhase::Evening, seed) != HomeItem::Game);
            assert(Personality::Propose(t, DayPhase::Day, seed) != HomeItem::Sleep);
        }
        assert(Personality::ProposalWeight(t, DayPhase::Morning, HomeItem::Feed) ==
               Personality::ProposalWeight(t, DayPhase::Day, HomeItem::Feed) + 2);
        assert(Personality::ProposalWeight(t, DayPhase::Evening, HomeItem::Story) ==
               2 * Personality::ProposalWeight(t, DayPhase::Day, HomeItem::Story));
    }
    assert(Personality::ProposalWeight(static_cast<T>(5), DayPhase::Day, HomeItem::Chat) == 0);
    assert(Personality::Propose(static_cast<T>(5), DayPhase::Day, 1) == HomeItem::Story);

    // The temperament's favourite wins most often during the day.
    const HomeItem favourite[] = {HomeItem::Story, HomeItem::Chat, HomeItem::Game};
    for (size_t i = 0; i < 3; ++i) {
        unsigned counts[HomeMenu::kItemCount]{};
        for (uint32_t seed = 0; seed < 3000; ++seed)
            ++counts[static_cast<size_t>(Personality::Propose(kAll[i], DayPhase::Day, seed))];
        for (size_t item = 0; item < HomeMenu::kItemCount; ++item) {
            if (item != static_cast<size_t>(favourite[i]))
                assert(counts[static_cast<size_t>(favourite[i])] > counts[item]);
        }
    }
}

void TestMemoryDrivesSlotTemplates() {
    // Without memory a slot line is never chosen.
    for (T t : kAll) {
        for (uint32_t seed = 0; seed < 200; ++seed) {
            for (size_t c = 0; c < kPhraseContextCount; ++c) {
                const auto* p = Personality::FindPhrase(
                    Personality::SelectPhrase(t, static_cast<C>(c), false, false, seed));
                assert(p && p->slot == PhraseSlot::None && p->temperament == t);
            }
        }
    }
    MemoryStore memory;
    std::set<uint16_t> recap, favourite;
    for (uint32_t seed = 0; seed < 200; ++seed) {
        recap.insert(Personality::SelectPhrase(memory, C::MorningRecap, seed));
        favourite.insert(Personality::SelectPhrase(memory, C::FavoriteMention, seed));
    }
    assert(recap == std::set<uint16_t>{221});
    assert(favourite == std::set<uint16_t>{231});

    MemoryEvent book{};
    book.id = 1;
    book.kind = MemoryEventKind::BookFinished;
    book.source = MemorySource::Device;
    book.option = 12;
    assert(memory.Apply(book) == MemoryResult::Applied);
    MemoryEvent like{};
    like.id = 2;
    like.kind = MemoryEventKind::PreferenceChosen;
    like.source = MemorySource::DialChoice;
    like.slot = 1;
    like.option = 3;
    assert(memory.Apply(like) == MemoryResult::Applied);
    unsigned book_lines = 0, like_lines = 0;
    for (uint32_t seed = 0; seed < 400; ++seed) {
        book_lines += Personality::SelectPhrase(memory, C::MorningRecap, seed) == 222;
        like_lines += Personality::SelectPhrase(memory, C::FavoriteMention, seed) == 232;
    }
    assert(book_lines > 200 && like_lines > 200);  // weight 3 vs 1

    assert(Personality::SelectPhrase(static_cast<T>(4), C::Greeting, true, true, 0) == 0);
    assert(Personality::SelectPhrase(T::Gentle, static_cast<C>(6), true, true, 0) == 0);
}
}  // namespace

int main() {
    TestReviewedPhraseTable();
    TestSameInputSameOutput();
    TestProposalWeightsByPhase();
    TestMemoryDrivesSlotTemplates();
    std::cout << "PASS: V5-01 personality is deterministic, reviewed and memory-aware\n";
}
