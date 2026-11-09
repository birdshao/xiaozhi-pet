// V9 slice 1A: "today's pick" for the play menu. A pure, deterministic
// function of (day, evening, boredom, last game, age band, switches).
#include "kid_app/activity_recommend.h"
#include "kid_app/play_content.h"

#include <cassert>
#include <iostream>
#include <set>

using namespace kid_app;

namespace {

bool IsRecommendable(GameChoice c) {
    const Activity* a = FindActivity(c);
    return a && a->group == ActivityGroup::Play && c != GameChoice::Songs &&
           c != GameChoice::Stickers;
}

ActivityCategory CategoryOf(GameChoice c) { return FindActivity(c)->category; }

void CheckWellFormed(const Recommendation& r) {
    std::set<uint8_t> seen;
    for (GameChoice c : r) {
        assert(c != GameChoice::None);
        assert(IsRecommendable(c));
        assert(seen.insert(static_cast<uint8_t>(c)).second);  // no duplicates
    }
}

void TestDeterministic() {
    RecommendInput in;
    in.day = 20400;
    const Recommendation a = RecommendToday(in), b = RecommendToday(in);
    assert(a == b);
    CheckWellFormed(a);
    // Another day can differ; over two weeks several different sets show up.
    std::set<std::array<uint8_t, 3>> sets;
    for (uint32_t d = 0; d < 14; ++d) {
        in.day = 20400 + d;
        const Recommendation r = RecommendToday(in);
        CheckWellFormed(r);
        sets.insert({static_cast<uint8_t>(r[0]), static_cast<uint8_t>(r[1]),
                     static_cast<uint8_t>(r[2])});
    }
    assert(sets.size() >= 5);
}

void TestDaytimeMixesCategories() {
    RecommendInput in;
    for (uint32_t d = 0; d < 30; ++d) {
        in.day = 20400 + d;
        const Recommendation r = RecommendToday(in);
        CheckWellFormed(r);
        std::set<ActivityCategory> cats;
        for (GameChoice c : r) cats.insert(CategoryOf(c));
        assert(cats.size() == 3);  // one from each of brain / move / sound
    }
}

void TestEveningIsQuiet() {
    RecommendInput in;
    in.evening = true;
    for (uint32_t d = 0; d < 30; ++d) {
        in.day = 20400 + d;
        const Recommendation r = RecommendToday(in);
        CheckWellFormed(r);
        for (GameChoice c : r) {
            assert(CategoryOf(c) == ActivityCategory::Brain);
            assert(FindActivity(c)->needs == ActivityNeeds::None);
        }
        in.bored = 3;  // quiet wins over boredom after dusk
        const Recommendation sleepy = RecommendToday(in);
        for (GameChoice c : sleepy) assert(CategoryOf(c) == ActivityCategory::Brain);
        in.bored = 0;
    }
}

void TestBoredMeansMove() {
    RecommendInput in;
    in.bored = 2;
    for (uint32_t d = 0; d < 30; ++d) {
        in.day = 20400 + d;
        const Recommendation r = RecommendToday(in);
        CheckWellFormed(r);
        for (GameChoice c : r) assert(CategoryOf(c) == ActivityCategory::Move);
    }
    in.bored = 1;  // a little bored is not enough
    in.day = 20400;
    std::set<ActivityCategory> cats;
    for (GameChoice c : RecommendToday(in)) cats.insert(CategoryOf(c));
    assert(cats.size() == 3);
    // Motion switched off: nothing that needs the board to move.
    in.bored = 3;
    in.motion_ok = false;
    for (uint32_t d = 0; d < 30; ++d) {
        in.day = 20400 + d;
        const Recommendation r = RecommendToday(in);
        CheckWellFormed(r);
        for (GameChoice c : r) assert(FindActivity(c)->needs != ActivityNeeds::Motion);
    }
}

void TestSwitchesAndLastGame() {
    RecommendInput in;
    in.mic_ok = false;
    in.motion_ok = false;
    for (uint32_t d = 0; d < 30; ++d) {
        in.day = 20400 + d;
        const Recommendation r = RecommendToday(in);
        CheckWellFormed(r);
        for (GameChoice c : r) assert(FindActivity(c)->needs == ActivityNeeds::None);
    }
    // The game just played is not offered again while others are free.
    RecommendInput last;
    for (uint32_t d = 0; d < 30; ++d) {
        last.day = 20400 + d;
        const Recommendation first = RecommendToday(last);
        for (GameChoice played : first) {
            last.last_game = played;
            const Recommendation again = RecommendToday(last);
            CheckWellFormed(again);
            for (GameChoice c : again) assert(c != played);
        }
        last.last_game = GameChoice::None;
    }
}

void TestAgeBand() {
    // "All" is the default until the age band exists; a band keeps games
    // that are suggested from a later age out of the pick.
    RecommendInput in;
    in.age = AgeBand::Three;
    for (uint32_t d = 0; d < 60; ++d) {
        in.day = 20400 + d;
        for (bool evening : {false, true}) {
            in.evening = evening;
            const Recommendation r = RecommendToday(in);
            CheckWellFormed(r);
            for (GameChoice c : r) {
                const PlayGameInfo* info = FindPlayGame(PlayGameOf(c));
                if (info) assert(info->min_age <= 3);
            }
        }
    }
    in.evening = false;
    in.age = AgeBand::All;
    assert(RecommendToday(in) == RecommendToday(RecommendInput{in}));
}

void TestMemoryIdMapping() {
    assert(GameChoiceOfMemoryId(1) == GameChoice::Quiz);
    assert(GameChoiceOfMemoryId(2) == GameChoice::Candle);
    assert(GameChoiceOfMemoryId(3) == GameChoice::Clap);
    assert(GameChoiceOfMemoryId(4) == GameChoice::Ball);
    assert(GameChoiceOfMemoryId(5) == GameChoice::Peekaboo);
    size_t n = 0;
    const PlayGameInfo* games = PlayGames(&n);
    assert(n == kPlayGameCount);
    for (size_t i = 0; i < n; ++i)
        assert(GameChoiceOfMemoryId(games[i].id) == GameChoiceOf(games[i].game));
    assert(GameChoiceOfMemoryId(0) == GameChoice::None);
    assert(GameChoiceOfMemoryId(999) == GameChoice::None);
}

}  // namespace

int main() {
    TestDeterministic();
    TestDaytimeMixesCategories();
    TestEveningIsQuiet();
    TestBoredMeansMove();
    TestSwitchesAndLastGame();
    TestAgeBand();
    TestMemoryIdMapping();
    std::cout << "activity recommend tests passed\n";
    return 0;
}
