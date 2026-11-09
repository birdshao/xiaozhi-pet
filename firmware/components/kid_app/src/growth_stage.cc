#include "kid_app/growth_stage.h"

namespace kid_app {
namespace {

uint32_t Sub(uint32_t a, uint32_t b) { return a > b ? a - b : 0; }

}  // namespace

uint32_t GrowthScoreNow(const CareStore& care, const MemoryStore& memory) {
    const MemoryStore::Growth& g = memory.GetGrowth();
    return GrowthScore(g.active_days, g.books_finished, g.games_completed, care.GetTotals().All());
}

uint8_t GrowthStageNow(const CareStore& care, const MemoryStore& memory) {
    return GrowthStageOf(GrowthScoreNow(care, memory));
}

uint8_t GrewOn(const CareStore& care, const MemoryStore& memory, uint32_t day,
               bool with_yesterday) {
    if (!day) return 0;
    const uint32_t from = with_yesterday && day > 1 ? day - 1 : day;
    auto held = [&](uint32_t d) { return d && d >= from && d <= day; };
    const MemoryStore::Growth& g = memory.GetGrowth();
    uint32_t active = g.active_days, books = g.books_finished, games = g.games_completed;
    uint32_t cares = care.GetTotals().All();
    for (const MemoryStore::Day* m : {&memory.Today(), &memory.Yesterday()}) {
        if (!held(m->day)) continue;
        if (m->Active()) active = Sub(active, 1);
        books = Sub(books, m->books);
        games = Sub(games, m->games);
    }
    const CareStore::Today& c = care.GetToday();
    if (held(c.day)) cares = Sub(cares, c.Cares());
    const uint8_t now = GrowthStageNow(care, memory);
    return now > GrowthStageOf(GrowthScore(active, books, games, cares)) ? now : 0;
}

}  // namespace kid_app
