#include "kid_app/env_tips.h"
#include "kid_app/pet_phrases.h"
#include "kid_app/personality.h"
#include "kid_app/selftest_clock.h"

#include <cassert>
#include <cstdint>
#include <cstring>

using namespace kid_app;
using P = EnvTipPolicy;

namespace {

constexpr uint32_t kStep = 10000;  // readings every 10 s here (the board: about once a minute)

// Feeds `c10` over a span of `ms` (readings at both ends); returns the
// first tip that became pending.
EnvTip Feed(P& p, uint32_t* now, int c10, uint32_t ms) {
    EnvTip first = EnvTip::None;
    for (uint32_t t = 0; t <= ms; t += kStep) {
        *now += kStep;
        const EnvTip tip = p.Observe(true, c10, *now);
        if (first == EnvTip::None) first = tip;
    }
    return first;
}

void TestHotNeedsToHold() {
    P p;
    uint32_t now = 1000;
    assert(p.Observe(true, 230, now) == EnvTip::None);  // first reading: silent
    assert(p.Zone() == EnvZone::Normal);
    // A 30 s spike does not count; dropping back resets the candidate.
    assert(Feed(p, &now, 300, 30000) == EnvTip::None);
    assert(Feed(p, &now, 250, 20000) == EnvTip::None && p.Zone() == EnvZone::Normal);
    assert(Feed(p, &now, 300, P::kHoldMs - kStep) == EnvTip::None);
    assert(p.Pending() == EnvTip::None);
    assert(Feed(p, &now, 300, kStep) == EnvTip::Hot);
    assert(p.Zone() == EnvZone::Hot && p.Pending() == EnvTip::Hot);
    // Handed out once.
    assert(p.Take(false, true, 100, now) == EnvTip::Hot);
    assert(p.Take(false, true, 100, now) == EnvTip::None);
    assert(p.TodayCount() == 1);
}

void TestColdAndHysteresis() {
    P p;
    uint32_t now = 1000;
    p.Observe(true, 220, now);
    assert(Feed(p, &now, 150, P::kHoldMs) == EnvTip::Cold && p.Zone() == EnvZone::Cold);
    assert(p.Take(false, true, 5, now) == EnvTip::Cold);
    // Jitter between 16.0 and 18.0 keeps the zone: no flapping, no new tip.
    for (int i = 0; i < 20; ++i) {
        assert(Feed(p, &now, i % 2 ? 175 : 165, 2 * kStep) == EnvTip::None);
        assert(p.Zone() == EnvZone::Cold && p.Pending() == EnvTip::None);
    }
    // Recovery needs the hold above 18.0, then it is a normal room again.
    assert(Feed(p, &now, 200, P::kHoldMs) == EnvTip::None);
    assert(p.Zone() == EnvZone::Normal);
    // Same for hot: 27.9 is not hot enough to enter, 26.0 is the way out.
    assert(Feed(p, &now, 279, 3 * P::kHoldMs) == EnvTip::None && p.Zone() == EnvZone::Normal);
    assert(Feed(p, &now, 285, P::kHoldMs) == EnvTip::Hot);
    p.Take(false, true, 5, now);
    assert(Feed(p, &now, 265, 3 * P::kHoldMs) == EnvTip::None && p.Zone() == EnvZone::Hot);
}

void TestBoardHeatOffset() {
    P p(40);  // the board reads 4.0 C above the room
    uint32_t now = 1000;
    p.Observe(true, 250, now);
    assert(p.RoomC10() == 210);
    assert(Feed(p, &now, 310, 3 * P::kHoldMs) == EnvTip::None);  // 27.0 room: fine
    assert(Feed(p, &now, 330, P::kHoldMs) == EnvTip::Hot);       // 29.0 room: hot
}

void TestSensorFailureRestartsTheHold() {
    P p;
    uint32_t now = 1000;
    p.Observe(true, 220, now);
    Feed(p, &now, 300, P::kHoldMs - kStep);
    now += kStep;
    assert(p.Observe(false, 0, now) == EnvTip::None);
    assert(Feed(p, &now, 300, P::kHoldMs - kStep) == EnvTip::None);
    assert(Feed(p, &now, 300, kStep) == EnvTip::Hot);
}

void TestQuietHoursAndAllowed() {
    P p;
    uint32_t now = 1000;
    p.Observe(true, 220, now);
    assert(Feed(p, &now, 300, P::kHoldMs) == EnvTip::Hot);
    // Quiet hours / not the right moment: it waits, silently.
    assert(p.Take(true, true, 7, now) == EnvTip::None);
    assert(p.Take(false, false, 7, now + 1000) == EnvTip::None);
    assert(p.Pending() == EnvTip::Hot && p.TodayCount() == 0);
    assert(p.Take(false, true, 7, now + 2000) == EnvTip::Hot);

    // A tip that waited through the night is dropped, not blurted at breakfast.
    P q;
    now = 1000;
    q.Observe(true, 220, now);
    assert(Feed(q, &now, 300, P::kHoldMs) == EnvTip::Hot);
    assert(q.Take(true, true, 7, now + 1000) == EnvTip::None);
    assert(q.Take(false, true, 7, now + P::kPendingMs) == EnvTip::None);
    assert(q.Pending() == EnvTip::None);
}

void TestRecoveredBeforeSpoken() {
    P p;
    uint32_t now = 1000;
    p.Observe(true, 220, now);
    assert(Feed(p, &now, 300, P::kHoldMs) == EnvTip::Hot);
    assert(Feed(p, &now, 220, P::kHoldMs) == EnvTip::None);  // quiet the whole time
    assert(p.Zone() == EnvZone::Normal && p.Pending() == EnvTip::None);
    assert(p.Take(false, true, 7, now) == EnvTip::None);
}

void TestDailyLimitAndRepeat() {
    P p;
    uint32_t now = 1000;
    p.Observe(true, 220, now);
    assert(Feed(p, &now, 300, P::kHoldMs) == EnvTip::Hot);
    assert(p.Take(false, true, 20, now) == EnvTip::Hot);
    // Still hot: a reminder every kRepeatMs, three a day at most.
    for (int round = 2; round <= 5; ++round) {
        assert(Feed(p, &now, 300, P::kRepeatMs) == EnvTip::Hot);
        const bool within = round <= P::kDailyLimit;
        assert(p.Take(false, true, 20, now) == (within ? EnvTip::Hot : EnvTip::None));
        assert(p.TodayCount() == (within ? round : P::kDailyLimit));
    }
    // A new local day starts the count again.
    assert(Feed(p, &now, 300, P::kRepeatMs) == EnvTip::Hot);
    assert(p.Take(false, true, 21, now) == EnvTip::Hot);
    assert(p.TodayCount() == 1);
}

void TestClockNotSetCountsPerBoot() {
    P p;
    uint32_t now = 1000;
    p.Observe(true, 220, now);
    // Alternate hot / cold rooms: every tip counts toward the one limit.
    for (int i = 0; i < P::kDailyLimit; ++i) {
        assert(Feed(p, &now, i % 2 ? 150 : 300, P::kHoldMs) != EnvTip::None);
        assert(p.Take(false, true, 0, now) != EnvTip::None);
    }
    assert(Feed(p, &now, P::kDailyLimit % 2 ? 150 : 300, P::kHoldMs) != EnvTip::None);
    assert(p.Take(false, true, 0, now) == EnvTip::None);
    assert(p.TodayCount() == P::kDailyLimit);
}

void TestWrapAndScale() {
    P p;
    uint32_t now = 0xFFFFFFFFu - 25000;  // the ms clock wraps during the hold
    p.Observe(true, 220, now);
    assert(Feed(p, &now, 300, P::kHoldMs) == EnvTip::Hot);
    assert(p.Take(false, true, 3, now) == EnvTip::Hot);

    // TEST ONLY clock scale: the hold is shorter; real time again after.
    assert(SelfTestClock::SetForSelfTest(25));
    P q;
    uint32_t t = 1000;
    q.Observe(true, 220, t);
    assert(Feed(q, &t, 300, P::kHoldMs / 4 + kStep) == EnvTip::Hot);
    assert(SelfTestClock::SetForSelfTest(100));
}

void TestNames() {
    assert(std::strcmp(EnvTipName(EnvTip::Hot), "hot") == 0);
    assert(std::strcmp(EnvTipName(EnvTip::Cold), "cold") == 0);
    assert(std::strcmp(EnvTipName(EnvTip::None), "none") == 0);
    assert(std::strcmp(EnvZoneName(EnvZone::Unknown), "unknown") == 0);
    assert(std::strcmp(EnvZoneName(EnvZone::Cold), "cold") == 0);
}

void TestTipPhrases() {
    // Every temperament has reviewed, slot-free lines for both tips, found
    // by the usual planner; nothing here needs a memory slot.
    for (Temperament t : {Temperament::Curious, Temperament::Gentle, Temperament::Lively}) {
        for (PhraseContext c : {PhraseContext::HotTip, PhraseContext::ColdTip}) {
            PhrasePlanner planner;
            planner.SetTemperament(t);
            const uint16_t first = planner.Next(c, 100000);
            const uint16_t second = planner.Next(c, 200000);
            assert(first && second);
            for (uint16_t id : {first, second}) {
                const PhraseTemplate* tpl = Personality::FindPhrase(id);
                assert(tpl && tpl->context == c && tpl->temperament == t);
                assert(RecordablePhrase(id) && std::strlen(tpl->text) > 0);
            }
        }
    }
    assert(std::strcmp(PhraseContextName(PhraseContext::HotTip), "hot_tip") == 0);
    assert(std::strcmp(PhraseContextName(PhraseContext::ColdTip), "cold_tip") == 0);
}

}  // namespace

int main() {
    TestHotNeedsToHold();
    TestColdAndHysteresis();
    TestBoardHeatOffset();
    TestSensorFailureRestartsTheHold();
    TestQuietHoursAndAllowed();
    TestRecoveredBeforeSpoken();
    TestDailyLimitAndRepeat();
    TestClockNotSetCountsPerBoot();
    TestWrapAndScale();
    TestNames();
    TestTipPhrases();
    return 0;
}
