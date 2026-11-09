#pragma once

#include "kid_app/care_store.h"
#include "kid_app/memory_store.h"

#include <cstdint>

namespace kid_app {

// V9 C3: TuanTuan grows up (docs/V9_MENU_DEEPENING_PROPOSAL.md §2.1 C3).
// Four stages: little TuanTuan, TuanTuan, big TuanTuan, captain TuanTuan.
// Derived, never stored: a pure function of counters that only ever go up
// (active days, cares, books finished, games completed), so a stage never
// goes back. Only the parent's "forget everything" starts again from 0,
// exactly like the stickers.
//
//   score = active days + cares / 10 + books + games
//
// An ordinary day (one book, two games, five cares) is about 4.5 points:
// stage 1 after about a week, 2 after about a month, 3 after three months.
constexpr uint8_t kGrowthStages = 4;
constexpr uint32_t kGrowthStageScore[kGrowthStages] = {0, 25, 120, 380};

constexpr uint32_t GrowthScore(uint32_t active_days, uint32_t books, uint32_t games,
                               uint32_t cares) {
    return active_days + cares / 10 + books + games;
}
constexpr uint8_t GrowthStageOf(uint32_t score) {
    uint8_t stage = 0;
    for (uint8_t s = 1; s < kGrowthStages; ++s)
        if (score >= kGrowthStageScore[s]) stage = s;
    return stage;
}

uint32_t GrowthScoreNow(const CareStore& care, const MemoryStore& memory);
uint8_t GrowthStageNow(const CareStore& care, const MemoryStore& memory);

// The stage TuanTuan reached on civil `day` (with_yesterday: on `day` or
// the day before), else 0: the stage now is compared with the stage
// without what the ledger still holds of those days (memory today /
// yesterday, care today). The diary asks for its own day, the good morning
// also for yesterday (a stage reached while the child was asleep is news
// the next morning). Care counted on a day the care store already rolled
// past is not known any more; it only ever makes the news a day late.
uint8_t GrewOn(const CareStore& care, const MemoryStore& memory, uint32_t day,
               bool with_yesterday);

}  // namespace kid_app
