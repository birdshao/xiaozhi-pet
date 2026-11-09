#include "kid_app/activity_recommend.h"

#include "kid_app/play_content.h"

#include <algorithm>

namespace kid_app {
namespace {

constexpr size_t kMaxPool = 32;  // every Play row, with room to spare

struct Pool {
    std::array<GameChoice, kMaxPool> games{};
    size_t count = 0;
    size_t next = 0;
};

bool Allowed(const Activity& a, const RecommendInput& in) {
    if (a.id == GameChoice::Songs) return false;  // a player, not a game
    if (a.needs == ActivityNeeds::Motion && !in.motion_ok) return false;
    if (a.needs == ActivityNeeds::Mic && !in.mic_ok) return false;
    if (in.age != AgeBand::All) {
        const PlayGameInfo* info = FindPlayGame(PlayGameOf(a.id));
        if (info && info->min_age > static_cast<uint8_t>(in.age)) return false;
    }
    return true;
}

// xorshift32: the shuffle of one category on one day.
uint32_t Step(uint32_t* s) {
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}

// The allowed games of `page` in a day-shuffled order; the game played last
// goes to the end (offered again only when nothing else is left).
Pool BuildPool(ActivityCategory page, const RecommendInput& in) {
    Pool pool;
    for (size_t i = 0; i < ActivityCountIn(page) && pool.count < kMaxPool; ++i) {
        const Activity* a = ActivityInCategory(page, i);
        if (a && Allowed(*a, in)) pool.games[pool.count++] = a->id;
    }
    uint32_t seed = in.day * 2654435761u ^ (static_cast<uint32_t>(page) + 1) * 0x85EBCA6Bu;
    if (seed == 0) seed = 1;
    for (size_t i = pool.count; i > 1; --i)
        std::swap(pool.games[i - 1], pool.games[Step(&seed) % i]);
    if (in.last_game != GameChoice::None) {
        std::stable_partition(pool.games.begin(), pool.games.begin() + pool.count,
                              [&](GameChoice c) { return c != in.last_game; });
    }
    return pool;
}

}  // namespace

Recommendation RecommendToday(const RecommendInput& in) {
    constexpr ActivityCategory kDefault[3] = {ActivityCategory::Brain, ActivityCategory::Move,
                                              ActivityCategory::Sound};
    ActivityCategory order[3] = {};
    size_t pages = 0;
    bool sequential = true;  // one page fills the slots; false: one from each in turn
    if (in.evening) {  // dusk: quiet games only, whatever else is going on
        order[pages++] = ActivityCategory::Brain;
    } else if (in.bored >= 2) {
        order[pages++] = ActivityCategory::Move;
        order[pages++] = ActivityCategory::Brain;
        order[pages++] = ActivityCategory::Sound;
    } else {
        sequential = false;
        for (size_t i = 0; i < 3; ++i) order[pages++] = kDefault[(i + in.day % 3) % 3];
    }
    Pool pools[3];
    for (size_t i = 0; i < pages; ++i) pools[i] = BuildPool(order[i], in);

    Recommendation out{};
    size_t n = 0;
    if (sequential) {
        for (size_t i = 0; i < pages && n < kRecommendCount; ++i)
            while (n < kRecommendCount && pools[i].next < pools[i].count)
                out[n++] = pools[i].games[pools[i].next++];
    } else {
        bool progress = true;
        while (n < kRecommendCount && progress) {
            progress = false;
            for (size_t i = 0; i < pages && n < kRecommendCount; ++i)
                if (pools[i].next < pools[i].count) {
                    out[n++] = pools[i].games[pools[i].next++];
                    progress = true;
                }
        }
    }
    return out;
}

GameChoice GameChoiceOfMemoryId(uint16_t id) {
    switch (id) {
        case 1: return GameChoice::Quiz;
        case 2: return GameChoice::Candle;
        case 3: return GameChoice::Clap;
        case 4: return GameChoice::Ball;
        case 5: return GameChoice::Peekaboo;
        default: break;
    }
    size_t n = 0;
    const PlayGameInfo* games = PlayGames(&n);
    for (size_t i = 0; i < n; ++i)
        if (games[i].id == id) return GameChoiceOf(games[i].game);
    return GameChoice::None;
}

}  // namespace kid_app
