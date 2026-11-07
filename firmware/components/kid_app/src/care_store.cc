#include "kid_app/care_store.h"

#include <cstring>
#include <utility>

namespace kid_app {
namespace {
// Encoding (little endian, 96 bytes, reserved bytes zero):
//   0 format=1  1 habit  2 birth month  3 birth day  4..8 worn item per
//   WearSlot  9..11 0  12 fed_at  16 rested_at  20 bathed_at  24 played_at
//   (u32 care minutes)  28 food counts 6x u16  40 stickers u32
//   44 habit streak  46 best  48 total (u16)  50 0  52 habit last day u32
//   56 totals feeds, baths, brushes, tickles, lulls (u16)  66..67 0
//   68 today day u32  72 foods mask  73 last food  74 feeds  75 baths
//   76 brushes  77 tickles  78 lulls  79 habit done  80..95 0
constexpr uint8_t kFormat = 1;
constexpr size_t kWearAt = 4;
constexpr size_t kAnchorsAt = 12;
constexpr size_t kFoodsAt = 28;
constexpr size_t kStickersAt = 40;
constexpr size_t kHabitAt = 44;
constexpr size_t kTotalsAt = 56;
constexpr size_t kTodayAt = 68;
static_assert(kTodayAt + 12 <= CareStore::kBytes, "care layout overflow");

void Put16(uint8_t* b, size_t p, uint16_t n) {
    b[p] = static_cast<uint8_t>(n);
    b[p + 1] = static_cast<uint8_t>(n >> 8);
}
void Put32(uint8_t* b, size_t p, uint32_t n) {
    for (size_t i = 0; i < 4; ++i) b[p + i] = static_cast<uint8_t>(n >> (8 * i));
}
uint16_t Get16(const uint8_t* b, size_t p) {
    return static_cast<uint16_t>(b[p] | (b[p + 1] << 8));
}
uint32_t Get32(const uint8_t* b, size_t p) {
    uint32_t n = 0;
    for (size_t i = 0; i < 4; ++i) n |= uint32_t(b[p + i]) << (8 * i);
    return n;
}
void Saturate(uint16_t* n) { if (*n != UINT16_MAX) ++*n; }
void Count(uint8_t* n) { if (*n < CareStore::kTodayCap) ++*n; }
uint32_t Elapsed(uint32_t anchor, uint32_t minute) {
    return (!anchor || !minute || minute < anchor) ? 0 : minute - anchor;
}
uint8_t Level(uint32_t elapsed, uint32_t step) {
    const uint32_t l = elapsed / step;
    return static_cast<uint8_t>(l > kNeedMax ? kNeedMax : l);
}

// Wardrobe: every item has one unlocking sticker (append-only per slot).
constexpr WardrobeItem kWardrobe[] = {
    {WearSlot::Head, 1, Sticker::FirstFeed, "cap"},
    {WearSlot::Head, 2, Sticker::FirstBath, "bow"},
    {WearSlot::Head, 3, Sticker::TenBooks, "crown"},
    {WearSlot::Head, 4, Sticker::FirstLike, "flower"},
    {WearSlot::Neck, 1, Sticker::FirstBrush, "scarf"},
    {WearSlot::Neck, 2, Sticker::FirstGame, "bowtie"},
    {WearSlot::Neck, 3, Sticker::SevenDays, "medal"},
    {WearSlot::Face, 1, Sticker::FirstBook, "glasses"},
    {WearSlot::Face, 2, Sticker::TenGames, "sunglasses"},
    {WearSlot::Bed, 1, Sticker::FirstLull, "bed"},
    {WearSlot::Lamp, 1, Sticker::FiveBooks, "lamp"},
    {WearSlot::Lamp, 2, Sticker::Habit7, "starlight"},
};
constexpr size_t kWardrobeCount = sizeof(kWardrobe) / sizeof(kWardrobe[0]);

// Taste per temperament: one loved and one liked food, the rest okay.
struct TasteRow { Food love, like; };
constexpr TasteRow kTaste[] = {
    {Food::Apple, Food::Carrot},    // Curious
    {Food::Milk, Food::RiceBall},   // Gentle
    {Food::Fish, Food::Cookie},     // Lively
};
}  // namespace

uint32_t CareMinute(const CivilTime& now, bool clock_ok) {
    if (!clock_ok || !PlausibleWallClock(now)) return 0;
    const int64_t s = EpochFromCivil(now);
    return s > 0 ? static_cast<uint32_t>(s / 60) : 0;
}

Taste TasteOf(Temperament t, Food food) {
    const uint8_t i = static_cast<uint8_t>(t);
    if (i > static_cast<uint8_t>(Temperament::Lively) || food == Food::None) return Taste::Okay;
    if (kTaste[i].love == food) return Taste::Love;
    if (kTaste[i].like == food) return Taste::Like;
    return Taste::Okay;
}

const char* FoodName(Food food) {
    switch (food) {
        case Food::None: return "none";
        case Food::Fish: return "fish";
        case Food::Milk: return "milk";
        case Food::Apple: return "apple";
        case Food::Carrot: return "carrot";
        case Food::Cookie: return "cookie";
        case Food::RiceBall: return "riceball";
    }
    return "none";
}

const char* StickerName(Sticker s) {
    static const char* const kNames[kStickerCount] = {
        "first_book", "five_books", "ten_books", "first_game", "ten_games", "first_like",
        "seven_days", "first_feed", "ten_feeds", "first_bath", "first_brush", "first_tickle",
        "first_lull", "habit_3", "habit_7", "habit_14", "birthday", "festival", "care_star"};
    const uint8_t i = static_cast<uint8_t>(s);
    return i < kStickerCount ? kNames[i] : "unknown";
}

const char* HabitName(Habit h) {
    static const char* const kNames[kHabitCount + 1] = {
        "none", "brush_teeth", "wash_hands", "tidy_toys", "drink_water",
        "dress_self", "eat_veggies", "bed_on_time", "read_book"};
    const uint8_t i = static_cast<uint8_t>(h);
    return i <= kHabitCount ? kNames[i] : "none";
}

const char* FestivalName(Festival f) {
    static const char* const kNames[kFestivalCount + 1] = {
        "none", "new_year", "spring_festival", "lantern", "childrens_day",
        "dragon_boat", "mid_autumn", "birthday"};
    const uint8_t i = static_cast<uint8_t>(f);
    return i <= kFestivalCount ? kNames[i] : "none";
}

size_t WardrobeCount() { return kWardrobeCount; }
const WardrobeItem& WardrobeAt(size_t i) { return kWardrobe[i < kWardrobeCount ? i : 0]; }
const WardrobeItem* FindWardrobe(WearSlot slot, uint8_t item) {
    for (const auto& w : kWardrobe)
        if (w.slot == slot && w.item == item) return &w;
    return nullptr;
}
bool WardrobeUnlocked(const WardrobeItem& w, uint32_t stickers) {
    return (stickers & StickerBit(w.unlock)) != 0;
}
uint8_t WardrobeItemsIn(WearSlot slot) {
    uint8_t n = 0;
    for (const auto& w : kWardrobe)
        if (w.slot == slot && w.item > n) n = w.item;
    return n;
}

bool ValidBirthday(uint8_t month, uint8_t day) {
    static const uint8_t kDays[12] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month >= 1 && month <= 12 && day >= 1 && day <= kDays[month - 1];
}

bool CareStore::Today::operator==(const Today& o) const {
    return day == o.day && foods == o.foods && last_food == o.last_food && feeds == o.feeds &&
           baths == o.baths && brushes == o.brushes && tickles == o.tickles && lulls == o.lulls &&
           habit_done == o.habit_done;
}

bool CareStore::SourceAllowed(CareEventKind kind, MemorySource source) {
    switch (kind) {
        case CareEventKind::Fed:
        case CareEventKind::Dressed:
            // Taste memory and today's look: only the child's own dial choice.
            return source == MemorySource::DialChoice;
        case CareEventKind::Bathed:
        case CareEventKind::Brushed:
        case CareEventKind::Tickled:
        case CareEventKind::Lulled:
        case CareEventKind::FestivalSeen:
            return source == MemorySource::Device;  // on-device session rules
        case CareEventKind::HabitDone:
            return source == MemorySource::DialChoice || source == MemorySource::Button;
        case CareEventKind::HabitSet:
        case CareEventKind::BirthdaySet:
            return source == MemorySource::Parent;
    }
    return false;  // Voice / Network never write care or growth.
}

void CareStore::RollTo(uint32_t day) {
    if (!day) return;
    if (!today_.day) {  // undated activity belongs to the first known day
        today_.day = day;
        return;
    }
    if (day == today_.day) return;
    // A new day, or the clock was corrected backwards (it once ran ahead):
    // either way today starts again, so counts never freeze on a date that
    // the clock will not reach for a while.
    today_ = Today{};
    today_.day = day;
}

void CareStore::SeedAnchors(uint32_t minute) {
    // The first dated care starts every need clock, so a pet nobody has
    // bathed yet still gets a little dusty later (never before).
    if (!minute) return;
    // Sleepiness follows the day phase; only a lullaby sets rested_at_.
    for (uint32_t* a : {&fed_at_, &bathed_at_, &played_at_})
        if (!*a) *a = minute;
}

CareResult CareStore::Apply(const CareEvent& e) {
    if (!e.id || !SourceAllowed(e.kind, e.source)) return CareResult::Rejected;
    CareStore next = *this;
    const uint32_t day = CareDay(e.minute);
    const bool dated = e.minute != 0;
    auto body_care = [&](uint8_t* today, uint16_t* total, uint32_t* anchor) {
        if (e.slot || e.option) return false;
        next.RollTo(day);
        if (next.CountsToday(day)) Count(today);
        Saturate(total);
        if (anchor && dated) *anchor = e.minute;
        next.SeedAnchors(e.minute);
        return true;
    };
    switch (e.kind) {
        case CareEventKind::Fed: {
            if (e.slot || e.option < 1 || e.option > kFoodCount) return CareResult::Rejected;
            next.RollTo(day);
            if (next.CountsToday(day)) {
                next.today_.foods = static_cast<uint8_t>(next.today_.foods | (1u << (e.option - 1)));
                next.today_.last_food = static_cast<uint8_t>(e.option);
                Count(&next.today_.feeds);
            }
            Saturate(&next.food_counts_[e.option - 1]);
            Saturate(&next.totals_.feeds);
            if (dated) next.fed_at_ = e.minute;
            next.SeedAnchors(e.minute);
            break;
        }
        case CareEventKind::Bathed:
            if (!body_care(&next.today_.baths, &next.totals_.baths, &next.bathed_at_))
                return CareResult::Rejected;
            break;
        case CareEventKind::Brushed:
            if (!body_care(&next.today_.brushes, &next.totals_.brushes, nullptr))
                return CareResult::Rejected;
            // Brushing tidies a little: one dirt level, never cleaner than a bath.
            if (dated && next.bathed_at_ && next.bathed_at_ < e.minute) {
                const uint32_t gap = e.minute - next.bathed_at_;
                next.bathed_at_ += gap < kDirtyStepMin ? gap : kDirtyStepMin;
            }
            break;
        case CareEventKind::Tickled:
            if (!body_care(&next.today_.tickles, &next.totals_.tickles, &next.played_at_))
                return CareResult::Rejected;
            break;
        case CareEventKind::Lulled:
            if (!body_care(&next.today_.lulls, &next.totals_.lulls, &next.rested_at_))
                return CareResult::Rejected;
            break;
        case CareEventKind::HabitDone: {
            // A tick needs a known date (streaks are days) and a chosen habit.
            if (e.slot || e.option || !dated || next.habit_.habit == Habit::None ||
                day == next.habit_.last_day)
                return CareResult::Rejected;  // already ticked today
            auto& h = next.habit_;
            // The day after: the streak goes on. Later, or earlier (the clock
            // was corrected backwards after running ahead): a quiet new
            // streak of 1; best and total are kept.
            if (h.last_day && h.last_day + 1 == day) Saturate(&h.streak);
            else h.streak = 1;
            if (h.streak > h.best) h.best = h.streak;
            Saturate(&h.total);
            h.last_day = day;
            next.RollTo(day);
            if (next.today_.day == day) next.today_.habit_done = true;
            break;
        }
        case CareEventKind::Dressed: {
            if (e.slot >= kWearSlots) return CareResult::Rejected;
            if (e.option) {
                const WardrobeItem* w = FindWardrobe(static_cast<WearSlot>(e.slot),
                                                     static_cast<uint8_t>(e.option));
                if (!w || !WardrobeUnlocked(*w, next.stickers_)) return CareResult::Rejected;
            }
            next.wear_[e.slot] = static_cast<uint8_t>(e.option);
            break;
        }
        case CareEventKind::HabitSet: {
            if (e.slot || e.option > kHabitCount) return CareResult::Rejected;
            const Habit h = static_cast<Habit>(e.option);
            if (h != next.habit_.habit) {  // a new habit starts a new track
                next.habit_ = HabitTrack{};
                next.habit_.habit = h;
                next.today_.habit_done = false;
            }
            break;
        }
        case CareEventKind::BirthdaySet:
            if (e.slot > 12 || (e.slot ? !ValidBirthday(static_cast<uint8_t>(e.slot),
                                                         static_cast<uint8_t>(e.option))
                                       : e.option != 0))
                return CareResult::Rejected;
            next.birth_month_ = static_cast<uint8_t>(e.slot);
            next.birth_day_ = static_cast<uint8_t>(e.option);
            break;
        case CareEventKind::FestivalSeen:
            if (e.slot || e.option < 1 || e.option > kFestivalCount) return CareResult::Rejected;
            next.stickers_ |= static_cast<Festival>(e.option) == Festival::Birthday
                                  ? StickerBit(Sticker::Birthday)
                                  : StickerBit(Sticker::Festival);
            break;
        default:
            return CareResult::Rejected;
    }
    *this = next;
    return CareResult::Applied;
}

void CareStore::NotePlay(uint32_t minute) {
    if (!minute) return;
    played_at_ = minute;
    SeedAnchors(minute);
}

uint32_t CareStore::EarnStickers(const MemoryStore::Growth& g) {
    uint32_t s = stickers_;
    auto earn = [&](bool cond, Sticker st) { if (cond) s |= StickerBit(st); };
    const uint32_t m = g.milestones;
    earn(m & MemoryStore::kFirstBook, Sticker::FirstBook);
    earn(m & MemoryStore::kFiveBooks, Sticker::FiveBooks);
    earn(m & MemoryStore::kTenBooks, Sticker::TenBooks);
    earn(m & MemoryStore::kFirstGame, Sticker::FirstGame);
    earn(m & MemoryStore::kTenGames, Sticker::TenGames);
    earn(m & MemoryStore::kFirstPreference, Sticker::FirstLike);
    earn(m & MemoryStore::kSevenDays, Sticker::SevenDays);
    earn(totals_.feeds >= 1, Sticker::FirstFeed);
    earn(totals_.feeds >= 10, Sticker::TenFeeds);
    earn(totals_.baths >= 1, Sticker::FirstBath);
    earn(totals_.brushes >= 1, Sticker::FirstBrush);
    earn(totals_.tickles >= 1, Sticker::FirstTickle);
    earn(totals_.lulls >= 1, Sticker::FirstLull);
    earn(habit_.best >= 3, Sticker::Habit3);
    earn(habit_.best >= 7, Sticker::Habit7);
    earn(habit_.best >= 14, Sticker::Habit14);
    earn(totals_.All() >= 20, Sticker::CareStar);
    const uint32_t added = s & ~stickers_;
    stickers_ = s;
    return added;
}

void CareStore::WipeChildData() {
    CareStore blank{};
    blank.birth_month_ = birth_month_;
    blank.birth_day_ = birth_day_;
    blank.habit_.habit = habit_.habit;
    *this = blank;
}

NeedLevels CareStore::Needs(uint32_t minute, DayPhase phase) const {
    NeedLevels n{};
    if (!minute) return n;  // clock unknown: a content, neutral pet
    n.hungry = Level(Elapsed(fed_at_, minute), kHungryStepMin);
    n.dirty = Level(Elapsed(bathed_at_, minute), kDirtyStepMin);
    n.bored = Level(Elapsed(played_at_, minute), kBoredStepMin);
    switch (phase) {
        case DayPhase::Night: n.sleepy = 3; break;
        case DayPhase::Evening:
        case DayPhase::Nap: n.sleepy = 2; break;
        case DayPhase::Morning:
        case DayPhase::Day: n.sleepy = 0; break;
    }
    if (rested_at_ && minute >= rested_at_ && minute - rested_at_ < kRestedMin) n.sleepy = 0;
    return n;
}

Need CareStore::TopNeed(const NeedLevels& n, uint8_t* level) {
    Need top = Need::None;
    uint8_t best = 1;  // only level >= 2 shows
    const std::pair<Need, uint8_t> order[] = {
        {Need::Hungry, n.hungry}, {Need::Sleepy, n.sleepy}, {Need::Dirty, n.dirty},
        {Need::Bored, n.bored}};
    for (const auto& o : order) {
        if (o.second > best) {
            best = o.second;
            top = o.first;
        }
    }
    if (level) *level = top == Need::None ? 0 : best;
    return top;
}

Food CareStore::MostFed() const {
    uint16_t best = 0;
    Food food = Food::None;
    for (uint8_t i = 0; i < kFoodCount; ++i) {
        if (food_counts_[i] > best) {
            best = food_counts_[i];
            food = static_cast<Food>(i + 1);
        }
    }
    return food;
}

CareStore::Today CareStore::TodayOn(uint32_t day) const {
    if (day && today_.day && today_.day != day) {
        Today t{};
        t.day = day;
        return t;
    }
    return today_;
}

uint16_t CareStore::FoodCount(Food f) const {
    const uint8_t i = static_cast<uint8_t>(f);
    return i >= 1 && i <= kFoodCount ? food_counts_[i - 1] : 0;
}

bool CareStore::Encode(uint8_t* b, size_t size) const {
    if (!b || size != kBytes) return false;
    std::memset(b, 0, size);
    b[0] = kFormat;
    b[1] = static_cast<uint8_t>(habit_.habit);
    b[2] = birth_month_;
    b[3] = birth_day_;
    for (size_t i = 0; i < kWearSlots; ++i) b[kWearAt + i] = wear_[i];
    Put32(b, kAnchorsAt, fed_at_);
    Put32(b, kAnchorsAt + 4, rested_at_);
    Put32(b, kAnchorsAt + 8, bathed_at_);
    Put32(b, kAnchorsAt + 12, played_at_);
    for (size_t i = 0; i < kFoodCount; ++i) Put16(b, kFoodsAt + 2 * i, food_counts_[i]);
    Put32(b, kStickersAt, stickers_);
    Put16(b, kHabitAt, habit_.streak);
    Put16(b, kHabitAt + 2, habit_.best);
    Put16(b, kHabitAt + 4, habit_.total);
    Put32(b, kHabitAt + 8, habit_.last_day);
    Put16(b, kTotalsAt, totals_.feeds);
    Put16(b, kTotalsAt + 2, totals_.baths);
    Put16(b, kTotalsAt + 4, totals_.brushes);
    Put16(b, kTotalsAt + 6, totals_.tickles);
    Put16(b, kTotalsAt + 8, totals_.lulls);
    Put32(b, kTodayAt, today_.day);
    b[kTodayAt + 4] = today_.foods;
    b[kTodayAt + 5] = today_.last_food;
    b[kTodayAt + 6] = today_.feeds;
    b[kTodayAt + 7] = today_.baths;
    b[kTodayAt + 8] = today_.brushes;
    b[kTodayAt + 9] = today_.tickles;
    b[kTodayAt + 10] = today_.lulls;
    b[kTodayAt + 11] = today_.habit_done ? 1 : 0;
    return true;
}

bool CareStore::Decode(const uint8_t* b, size_t size, CareStore* out) {
    if (!b || !out || size != kBytes || b[0] != kFormat || b[1] > kHabitCount) return false;
    CareStore c{};
    c.habit_.habit = static_cast<Habit>(b[1]);
    c.birth_month_ = b[2];
    c.birth_day_ = b[3];
    if ((c.birth_month_ || c.birth_day_) && !ValidBirthday(c.birth_month_, c.birth_day_))
        return false;
    for (size_t i = 0; i < kWearSlots; ++i) c.wear_[i] = b[kWearAt + i];
    c.fed_at_ = Get32(b, kAnchorsAt);
    c.rested_at_ = Get32(b, kAnchorsAt + 4);
    c.bathed_at_ = Get32(b, kAnchorsAt + 8);
    c.played_at_ = Get32(b, kAnchorsAt + 12);
    for (size_t i = 0; i < kFoodCount; ++i) c.food_counts_[i] = Get16(b, kFoodsAt + 2 * i);
    c.stickers_ = Get32(b, kStickersAt);
    c.habit_.streak = Get16(b, kHabitAt);
    c.habit_.best = Get16(b, kHabitAt + 2);
    c.habit_.total = Get16(b, kHabitAt + 4);
    c.habit_.last_day = Get32(b, kHabitAt + 8);
    c.totals_.feeds = Get16(b, kTotalsAt);
    c.totals_.baths = Get16(b, kTotalsAt + 2);
    c.totals_.brushes = Get16(b, kTotalsAt + 4);
    c.totals_.tickles = Get16(b, kTotalsAt + 6);
    c.totals_.lulls = Get16(b, kTotalsAt + 8);
    c.today_.day = Get32(b, kTodayAt);
    c.today_.foods = b[kTodayAt + 4];
    c.today_.last_food = b[kTodayAt + 5];
    c.today_.feeds = b[kTodayAt + 6];
    c.today_.baths = b[kTodayAt + 7];
    c.today_.brushes = b[kTodayAt + 8];
    c.today_.tickles = b[kTodayAt + 9];
    c.today_.lulls = b[kTodayAt + 10];
    if (b[kTodayAt + 11] > 1) return false;
    c.today_.habit_done = b[kTodayAt + 11] == 1;

    if (c.stickers_ & ~kAllStickers) return false;
    for (size_t i = 0; i < kWearSlots; ++i) {
        if (!c.wear_[i]) continue;
        const WardrobeItem* w = FindWardrobe(static_cast<WearSlot>(i), c.wear_[i]);
        if (!w || !WardrobeUnlocked(*w, c.stickers_)) return false;
    }
    const auto& h = c.habit_;
    if (h.habit == Habit::None && (h.streak || h.best || h.total || h.last_day)) return false;
    if (h.streak > h.best || h.best > h.total || (h.total != 0) != (h.last_day != 0) ||
        (h.streak == 0) != (h.last_day == 0))
        return false;
    const auto& t = c.today_;
    const uint8_t cap = kTodayCap;
    if (t.feeds > cap || t.baths > cap || t.brushes > cap || t.tickles > cap || t.lulls > cap ||
        t.last_food > kFoodCount || (t.foods & ~0x3Fu) || (t.foods == 0) != (t.last_food == 0) ||
        (t.last_food && !(t.foods & (1u << (t.last_food - 1)))) ||
        (t.habit_done && (!t.day || h.last_day != t.day)))
        return false;
    // Canonical form only: padding and reserved bytes are zero.
    uint8_t check[kBytes];
    if (!c.Encode(check, sizeof(check)) || std::memcmp(check, b, kBytes) != 0) return false;
    *out = c;
    return true;
}

bool CareStore::operator==(const CareStore& o) const {
    uint8_t a[kBytes], b[kBytes];
    return Encode(a, kBytes) && o.Encode(b, kBytes) && std::memcmp(a, b, kBytes) == 0;
}

}  // namespace kid_app
