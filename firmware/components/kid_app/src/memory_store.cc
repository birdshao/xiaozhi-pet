#include "kid_app/memory_store.h"

#include <cstdio>
#include <cstring>

namespace kid_app {
namespace {
// Encoding (little endian, 288 bytes, unused entries and reserved zero):
//   0 format=1, 1 temperament, 2 preference count, 3 recent book count
//   4 today (14), 18 yesterday (14), 32 growth (12), 44 books 8x u16,
//   60 preferences 16x {slot u16, option u16, source_event u64}, 252 reserved
constexpr uint8_t kFormat = 1;
constexpr size_t kTodayAt = 4;
constexpr size_t kYesterdayAt = 18;
constexpr size_t kGrowthAt = 32;
constexpr size_t kBooksAt = 44;
constexpr size_t kPrefsAt = 60;
constexpr size_t kPrefBytes = 12;
static_assert(kPrefsAt + kPrefBytes * MemoryStore::kPreferenceCapacity <=
              MemoryStore::kBytes, "memory layout overflow");

void Put16(uint8_t* b, size_t p, uint16_t n) {
    b[p] = static_cast<uint8_t>(n);
    b[p + 1] = static_cast<uint8_t>(n >> 8);
}
void Put32(uint8_t* b, size_t p, uint32_t n) {
    for (size_t i = 0; i < 4; ++i) b[p + i] = static_cast<uint8_t>(n >> (8 * i));
}
void Put64(uint8_t* b, size_t p, uint64_t n) {
    for (size_t i = 0; i < 8; ++i) b[p + i] = static_cast<uint8_t>(n >> (8 * i));
}
uint16_t Get16(const uint8_t* b, size_t p) {
    return static_cast<uint16_t>(b[p] | (b[p + 1] << 8));
}
uint32_t Get32(const uint8_t* b, size_t p) {
    uint32_t n = 0;
    for (size_t i = 0; i < 4; ++i) n |= uint32_t(b[p + i]) << (8 * i);
    return n;
}
uint64_t Get64(const uint8_t* b, size_t p) {
    uint64_t n = 0;
    for (size_t i = 0; i < 8; ++i) n |= uint64_t(b[p + i]) << (8 * i);
    return n;
}

void PutDay(uint8_t* b, size_t p, const MemoryStore::Day& d) {
    Put32(b, p, d.day);
    b[p + 4] = d.feeds;
    b[p + 5] = d.greetings;
    b[p + 6] = d.books;
    b[p + 7] = d.games;
    b[p + 8] = d.choices;
    Put16(b, p + 10, d.last_book);
    Put16(b, p + 12, d.last_game);
}
MemoryStore::Day GetDay(const uint8_t* b, size_t p) {
    MemoryStore::Day d{};
    d.day = Get32(b, p);
    d.feeds = b[p + 4];
    d.greetings = b[p + 5];
    d.books = b[p + 6];
    d.games = b[p + 7];
    d.choices = b[p + 8];
    d.last_book = Get16(b, p + 10);
    d.last_game = Get16(b, p + 12);
    return d;
}
bool DayValid(const MemoryStore::Day& d) {
    const uint8_t cap = MemoryStore::kTodayCap;
    return d.feeds <= cap && d.greetings <= cap && d.books <= cap &&
           d.games <= cap && d.choices <= cap &&
           (d.books || !d.last_book) && (d.games || !d.last_game);
}

void Saturate(uint16_t* n) { if (*n != UINT16_MAX) ++*n; }
}  // namespace

bool MemoryStore::Day::operator==(const Day& o) const {
    return day == o.day && feeds == o.feeds && greetings == o.greetings &&
           books == o.books && games == o.games && choices == o.choices &&
           last_book == o.last_book && last_game == o.last_game;
}

bool MemoryStore::SourceAllowed(MemoryEventKind kind, MemorySource source) {
    switch (kind) {
    case MemoryEventKind::PreferenceChosen:
        // V5-03: the ONLY path that writes likes.
        return source == MemorySource::DialChoice;
    case MemoryEventKind::BookFinished:
    case MemoryEventKind::GameCompleted:
        return source == MemorySource::DialChoice ||
               source == MemorySource::Button ||
               source == MemorySource::Device;
    case MemoryEventKind::MemoryCleared:
    case MemoryEventKind::TemperamentSet:
        return source == MemorySource::Parent;
    }
    return false;  // Voice / Network never write memory.
}

MemoryResult MemoryStore::Apply(const MemoryEvent& e) {
    if (!e.id || !SourceAllowed(e.kind, e.source)) return MemoryResult::Rejected;
    MemoryStore next = *this;
    switch (e.kind) {
    case MemoryEventKind::MemoryCleared:
        next.Wipe();
        break;
    case MemoryEventKind::TemperamentSet:
        if (e.option > static_cast<uint16_t>(Temperament::Lively))
            return MemoryResult::Rejected;
        next.temperament_ = static_cast<Temperament>(e.option);
        break;
    case MemoryEventKind::PreferenceChosen:
        if (!e.slot || !e.option) return MemoryResult::Rejected;
        next.RollTo(e.day);
        if (next.CountsToday(e.day) && next.today_.choices < kTodayCap) {
            if (!next.today_.Active()) Saturate(&next.growth_.active_days);
            ++next.today_.choices;
        }
        Saturate(&next.growth_.preferences_chosen);
        next.ChoosePreference(e.slot, e.option, e.id);
        break;
    case MemoryEventKind::BookFinished:
        if (!e.option) return MemoryResult::Rejected;
        next.RollTo(e.day);
        if (next.CountsToday(e.day) && next.today_.books < kTodayCap) {
            if (!next.today_.Active()) Saturate(&next.growth_.active_days);
            ++next.today_.books;
            next.today_.last_book = e.option;
        }
        Saturate(&next.growth_.books_finished);
        next.RememberBook(e.option);
        break;
    case MemoryEventKind::GameCompleted:
        if (!e.option) return MemoryResult::Rejected;
        next.RollTo(e.day);
        if (next.CountsToday(e.day) && next.today_.games < kTodayCap) {
            if (!next.today_.Active()) Saturate(&next.growth_.active_days);
            ++next.today_.games;
            next.today_.last_game = e.option;
        }
        Saturate(&next.growth_.games_completed);
        break;
    default:
        return MemoryResult::Rejected;
    }
    next.UpdateMilestones();
    *this = next;
    return MemoryResult::Applied;
}

void MemoryStore::NotePetAction(PetAction action) {
    uint8_t* counter = nullptr;
    if (action == PetAction::Feed) counter = &today_.feeds;
    else if (action == PetAction::Pickup) counter = &today_.greetings;
    if (!counter || *counter >= kTodayCap) return;
    if (!today_.Active()) Saturate(&growth_.active_days);
    ++*counter;
    UpdateMilestones();
}

void MemoryStore::Wipe() {
    // Back to "just met"; the parent's temperament choice is kept.
    const Temperament keep = temperament_;
    *this = MemoryStore{};
    temperament_ = keep;
}

void MemoryStore::RollTo(uint32_t day) {
    if (!day) return;
    if (!today_.day) {           // undated activity belongs to the first
        today_.day = day;        // day the clock becomes known
        return;
    }
    if (day <= today_.day) return;  // same day, or clock went backwards
    if (today_.Active()) yesterday_ = today_;  // last active day, for recaps
    today_ = Day{};
    today_.day = day;
}

bool MemoryStore::CountsToday(uint32_t day) const {
    return !day || day == today_.day;
}

void MemoryStore::RememberBook(uint16_t book) {
    // Most recent first; a re-read moves the book to the front, the oldest
    // entry falls off when full.
    size_t at = recent_book_count_;
    for (size_t i = 0; i < recent_book_count_; ++i)
        if (recent_books_[i] == book) at = i;
    if (at == recent_book_count_) {
        if (recent_book_count_ < kRecentBookCapacity) ++recent_book_count_;
        at = recent_book_count_ - 1;
    }
    for (size_t i = at; i > 0; --i) recent_books_[i] = recent_books_[i - 1];
    recent_books_[0] = book;
}

void MemoryStore::ChoosePreference(uint16_t slot, uint16_t option, uint64_t id) {
    // One answer per topic slot; re-answering replaces it. When all 16 slots
    // are used the least recently chosen topic is evicted.
    size_t at = preference_count_;
    for (size_t i = 0; i < preference_count_; ++i)
        if (preferences_[i].slot == slot) at = i;
    if (at == preference_count_) {
        if (preference_count_ < kPreferenceCapacity) ++preference_count_;
        at = preference_count_ - 1;
    }
    for (size_t i = at; i > 0; --i) preferences_[i] = preferences_[i - 1];
    preferences_[0] = {slot, option, id};
}

void MemoryStore::UpdateMilestones() {
    const auto& g = growth_;
    uint32_t m = g.milestones;
    if (g.books_finished >= 1) m |= kFirstBook;
    if (g.books_finished >= 5) m |= kFiveBooks;
    if (g.books_finished >= 10) m |= kTenBooks;
    if (g.games_completed >= 1) m |= kFirstGame;
    if (g.games_completed >= 10) m |= kTenGames;
    if (g.preferences_chosen >= 1) m |= kFirstPreference;
    if (g.active_days >= 7) m |= kSevenDays;
    growth_.milestones = m;
}

bool MemoryStore::FindPreference(uint16_t slot, uint16_t* option) const {
    for (size_t i = 0; i < preference_count_; ++i) {
        if (preferences_[i].slot == slot) {
            if (option) *option = preferences_[i].option;
            return true;
        }
    }
    return false;
}

bool MemoryStore::Empty() const {
    MemoryStore blank{};
    blank.temperament_ = temperament_;
    return *this == blank;
}

bool MemoryStore::Encode(uint8_t* b, size_t size) const {
    if (!b || size != kBytes) return false;
    std::memset(b, 0, size);
    b[0] = kFormat;
    b[1] = static_cast<uint8_t>(temperament_);
    b[2] = preference_count_;
    b[3] = recent_book_count_;
    PutDay(b, kTodayAt, today_);
    PutDay(b, kYesterdayAt, yesterday_);
    Put16(b, kGrowthAt, growth_.books_finished);
    Put16(b, kGrowthAt + 2, growth_.games_completed);
    Put16(b, kGrowthAt + 4, growth_.preferences_chosen);
    Put16(b, kGrowthAt + 6, growth_.active_days);
    Put32(b, kGrowthAt + 8, growth_.milestones);
    for (size_t i = 0; i < recent_book_count_; ++i)
        Put16(b, kBooksAt + 2 * i, recent_books_[i]);
    for (size_t i = 0; i < preference_count_; ++i) {
        const size_t p = kPrefsAt + kPrefBytes * i;
        Put16(b, p, preferences_[i].slot);
        Put16(b, p + 2, preferences_[i].option);
        Put64(b, p + 4, preferences_[i].source_event);
    }
    return true;
}

bool MemoryStore::Decode(const uint8_t* b, size_t size, MemoryStore* out) {
    if (!b || !out || size != kBytes || b[0] != kFormat ||
        b[1] > static_cast<uint8_t>(Temperament::Lively) ||
        b[2] > kPreferenceCapacity || b[3] > kRecentBookCapacity)
        return false;
    MemoryStore m{};
    m.temperament_ = static_cast<Temperament>(b[1]);
    m.preference_count_ = b[2];
    m.recent_book_count_ = b[3];
    m.today_ = GetDay(b, kTodayAt);
    m.yesterday_ = GetDay(b, kYesterdayAt);
    m.growth_.books_finished = Get16(b, kGrowthAt);
    m.growth_.games_completed = Get16(b, kGrowthAt + 2);
    m.growth_.preferences_chosen = Get16(b, kGrowthAt + 4);
    m.growth_.active_days = Get16(b, kGrowthAt + 6);
    m.growth_.milestones = Get32(b, kGrowthAt + 8);
    if (!DayValid(m.today_) || !DayValid(m.yesterday_) ||
        (m.growth_.milestones & ~static_cast<uint32_t>(kAllMilestones)) ||
        (m.yesterday_.day && m.yesterday_.day >= m.today_.day) ||
        (!m.yesterday_.day && !(m.yesterday_ == Day{})))
        return false;
    for (size_t i = 0; i < m.recent_book_count_; ++i) {
        m.recent_books_[i] = Get16(b, kBooksAt + 2 * i);
        if (!m.recent_books_[i]) return false;
        for (size_t j = 0; j < i; ++j)
            if (m.recent_books_[j] == m.recent_books_[i]) return false;
    }
    for (size_t i = 0; i < m.preference_count_; ++i) {
        const size_t p = kPrefsAt + kPrefBytes * i;
        auto& pref = m.preferences_[i];
        pref = {Get16(b, p), Get16(b, p + 2), Get64(b, p + 4)};
        if (!pref.slot || !pref.option || !pref.source_event) return false;
        for (size_t j = 0; j < i; ++j)
            if (m.preferences_[j].slot == pref.slot) return false;
    }
    // Canonical form only: unused entries, padding and reserved are zero.
    uint8_t check[kBytes];
    if (!m.Encode(check, sizeof(check)) || std::memcmp(check, b, kBytes) != 0)
        return false;
    *out = m;
    return true;
}

bool MemoryStore::operator==(const MemoryStore& o) const {
    uint8_t a[kBytes], b[kBytes];
    return Encode(a, kBytes) && o.Encode(b, kBytes) &&
           std::memcmp(a, b, kBytes) == 0;
}

const char* TemperamentName(Temperament t) {
    switch (t) {
    case Temperament::Curious: return "curious";
    case Temperament::Gentle: return "gentle";
    case Temperament::Lively: return "lively";
    }
    return "gentle";
}

MinimalSummary MakeMinimalSummary(const MemoryStore& memory) {
    MinimalSummary s{};
    s.temperament = memory.GetTemperament();
    s.preference_count = static_cast<uint8_t>(memory.PreferenceCount());
    for (size_t i = 0; i < memory.PreferenceCount(); ++i) {
        s.preferences[i] = {memory.PreferenceAt(i).slot,
                            memory.PreferenceAt(i).option};
    }
    const auto& today = memory.Today();
    s.feeds = today.feeds;
    s.greetings = today.greetings;
    s.books = today.books;
    s.games = today.games;
    s.last_book = memory.RecentBookCount() ? memory.RecentBookAt(0) : 0;
    s.milestones = memory.GetGrowth().milestones;
    return s;
}

bool FormatMinimalSummary(const MinimalSummary& s, char* out, size_t size) {
    if (!out || !size) return false;
    size_t used = 0;
    auto append = [&](const char* fmt, auto... args) {
        if (used >= size) return false;
        const int n = std::snprintf(out + used, size - used, fmt, args...);
        if (n < 0 || static_cast<size_t>(n) >= size - used) {
            used = size;
            return false;
        }
        used += static_cast<size_t>(n);
        return true;
    };
    bool ok = append("{\"trait\":\"%s\",\"preferences\":[",
                     TemperamentName(s.temperament));
    for (size_t i = 0; ok && i < s.preference_count &&
                       i < MemoryStore::kPreferenceCapacity; ++i) {
        ok = append("%s{\"slot\":%u,\"option\":%u}", i ? "," : "",
                    unsigned(s.preferences[i][0]), unsigned(s.preferences[i][1]));
    }
    ok = ok && append("],\"today\":{\"feed\":%u,\"greet\":%u,\"books\":%u,"
                      "\"games\":%u},\"last_book\":%u,\"milestones\":%lu}",
                      unsigned(s.feeds), unsigned(s.greetings), unsigned(s.books),
                      unsigned(s.games), unsigned(s.last_book),
                      static_cast<unsigned long>(s.milestones));
    if (!ok) out[0] = '\0';
    return ok;
}

}  // namespace kid_app
