#include "kid_app/memory_phrases.h"

#include <cassert>
#include <iostream>

using namespace kid_app;
using K = MemoryPhraseKind;
using R = MemoryPhraseReason;

namespace {
constexpr uint32_t kMin = 60u * 1000u;

MemoryStore WithTemperament(Temperament t) {
    MemoryStore m;
    MemoryEvent e{};
    e.id = 1;
    e.kind = MemoryEventKind::TemperamentSet;
    e.source = MemorySource::Parent;
    e.option = static_cast<uint16_t>(t);
    assert(m.Apply(e) == MemoryResult::Applied);
    return m;
}

void AddBook(MemoryStore* m, uint64_t id, uint32_t day, uint16_t book) {
    MemoryEvent e{};
    e.id = id;
    e.kind = MemoryEventKind::BookFinished;
    e.source = MemorySource::Device;
    e.day = day;
    e.option = book;
    assert(m->Apply(e) == MemoryResult::Applied);
}

void AddLike(MemoryStore* m, uint64_t id, uint32_t day, uint16_t slot, uint16_t option) {
    MemoryEvent e{};
    e.id = id;
    e.kind = MemoryEventKind::PreferenceChosen;
    e.source = MemorySource::DialChoice;
    e.day = day;
    e.slot = slot;
    e.option = option;
    assert(m->Apply(e) == MemoryResult::Applied);
}

void TestNoMemoryFallsBack() {
    MemoryStore empty;
    MemoryPhraseSelector s;
    for (K k : {K::MorningRecap, K::PickupLike}) {
        const auto d = s.Select(k, empty, 100, false, 0);
        assert(!d.use && d.reason == R::NoMemory && d.phrase_id == 0);
    }
    // A book from today or a week ago is not "yesterday"; unknown clock too.
    MemoryStore m;
    AddBook(&m, 2, 99, 7);
    for (uint32_t day : {0u, 1u, 99u, 105u})
        assert(!s.Select(K::MorningRecap, m, day, false, 0).use);
    // Books are not likes: pickup stays quiet without a chosen like.
    assert(!s.Select(K::PickupLike, m, 100, false, 0).use);
}

void TestMorningRecapByTemperament() {
    const struct { Temperament t; uint16_t id; } kCases[] = {
        {Temperament::Curious, 122}, {Temperament::Gentle, 222}, {Temperament::Lively, 322}};
    for (const auto& c : kCases) {
        MemoryStore m = WithTemperament(c.t);
        AddBook(&m, 2, 99, 7);
        MemoryPhraseSelector a, b;
        const auto d = a.Select(K::MorningRecap, m, 100, false, 5);
        assert(d.use && d.phrase_id == c.id && d.slot == PhraseSlot::Book && d.value == 7);
        const auto again = b.Select(K::MorningRecap, m, 100, false, 5);  // same in, same out
        assert(again.phrase_id == d.phrase_id && again.value == d.value);
        // Quiet hours and the pickup cap do not hold back the morning line.
        assert(a.Select(K::MorningRecap, m, 100, true, 5).use);
    }
}

void TestPickupLikeCaps() {
    MemoryStore m = WithTemperament(Temperament::Gentle);
    AddLike(&m, 2, 100, 1, 3);
    AddLike(&m, 3, 100, 2, 9);  // newest first: slot 2 then slot 1
    MemoryPhraseSelector s;
    uint32_t now = 1000;
    auto d = s.Select(K::PickupLike, m, 100, false, now);
    assert(d.use && d.phrase_id == 232 && d.slot == PhraseSlot::Like);
    assert(d.like_slot == 2 && d.value == 9);
    // Not every pickup: too soon.
    now += kMin;
    assert(s.Select(K::PickupLike, m, 100, false, now).reason == R::TooSoon);
    // After the gap it mentions the next like (rotation), then the cap bites.
    now += MemoryPhraseSelector::kPickupMinGapMs;
    d = s.Select(K::PickupLike, m, 100, false, now);
    assert(d.use && d.like_slot == 1 && d.value == 3);
    now += MemoryPhraseSelector::kPickupMinGapMs;
    assert(s.Select(K::PickupLike, m, 100, false, now).use);
    now += MemoryPhraseSelector::kPickupMinGapMs;
    assert(s.Select(K::PickupLike, m, 100, false, now).reason == R::DayLimit);
    // A new day resets the count.
    assert(s.Select(K::PickupLike, m, 101, false, now).use);
}

void TestPickupQuietAndClockWrap() {
    MemoryStore m = WithTemperament(Temperament::Lively);
    AddLike(&m, 2, 100, 1, 3);
    MemoryPhraseSelector s;
    // Quiet hours: silent, and it does not use up the cap or start the gap.
    for (int i = 0; i < 10; ++i)
        assert(s.Select(K::PickupLike, m, 100, true, i * 1000).reason == R::Quiet);
    auto d = s.Select(K::PickupLike, m, 100, false, 0xFFFFF000u);
    assert(d.use && d.phrase_id == 332);
    // The gap survives the uint32 ms clock wrapping.
    assert(s.Select(K::PickupLike, m, 100, false, 0xFFFFF000u + 5000u).reason == R::TooSoon);
    assert(s.Select(K::PickupLike, m, 100, false,
                    0xFFFFF000u + MemoryPhraseSelector::kPickupMinGapMs).use);
}

void TestEveryTemperamentHasSlotTemplates() {
    for (Temperament t : {Temperament::Curious, Temperament::Gentle, Temperament::Lively}) {
        MemoryStore m = WithTemperament(t);
        AddBook(&m, 2, 9, 1);
        AddLike(&m, 3, 10, 1, 1);
        MemoryPhraseSelector s;
        assert(s.Select(K::MorningRecap, m, 10, false, 0).use);
        assert(s.Select(K::PickupLike, m, 10, false, 0).use);
    }
}
}  // namespace

int main() {
    TestNoMemoryFallsBack();
    TestMorningRecapByTemperament();
    TestPickupLikeCaps();
    TestPickupQuietAndClockWrap();
    TestEveryTemperamentHasSlotTemplates();
    std::cout << "PASS: V5-04 memory phrases are deterministic, capped and never invented\n";
}
