#include "kid_app/memory_phrases.h"

namespace kid_app {
namespace {

const PhraseTemplate* SlotTemplate(Temperament t, PhraseContext context, PhraseSlot slot) {
    for (size_t i = 0; i < Personality::PhraseCount(); ++i) {
        const PhraseTemplate& p = Personality::PhraseAt(i);
        if (p.temperament == t && p.context == context && p.slot == slot) return &p;
    }
    return nullptr;
}

MemoryPhraseDecision No(MemoryPhraseReason reason) {
    MemoryPhraseDecision d;
    d.reason = reason;
    return d;
}

}  // namespace

const char* MemoryPhraseKindName(MemoryPhraseKind kind) {
    return kind == MemoryPhraseKind::MorningRecap ? "morning_recap" : "pickup_like";
}

const char* MemoryPhraseReasonName(MemoryPhraseReason reason) {
    switch (reason) {
        case MemoryPhraseReason::Used: return "used";
        case MemoryPhraseReason::NoMemory: return "no_memory";
        case MemoryPhraseReason::Quiet: return "quiet";
        case MemoryPhraseReason::TooSoon: return "too_soon";
        case MemoryPhraseReason::DayLimit: return "day_limit";
        case MemoryPhraseReason::NoTemplate: return "no_template";
    }
    return "none";
}

MemoryPhraseDecision MemoryPhraseSelector::Select(MemoryPhraseKind kind,
                                                  const MemoryStore& memory, uint32_t day,
                                                  bool quiet, uint32_t now_ms) {
    const Temperament t = memory.GetTemperament();
    MemoryPhraseDecision d;
    if (kind == MemoryPhraseKind::MorningRecap) {
        // The morning window is the parent's wake plan; it is never held back
        // by quiet hours or the pickup cap. Needs a book finished yesterday.
        const uint16_t book = day > 1 ? memory.BookReadOn(day - 1) : 0;
        if (!book) return No(MemoryPhraseReason::NoMemory);
        const PhraseTemplate* p = SlotTemplate(t, PhraseContext::MorningRecap, PhraseSlot::Book);
        if (!p) return No(MemoryPhraseReason::NoTemplate);
        d.use = true;
        d.reason = MemoryPhraseReason::Used;
        d.phrase_id = p->id;
        d.slot = PhraseSlot::Book;
        d.value = book;
        return d;
    }

    // PickupLike: likes only (they can only come from dial choice questions).
    if (memory.PreferenceCount() == 0) return No(MemoryPhraseReason::NoMemory);
    if (quiet) return No(MemoryPhraseReason::Quiet);
    if (day != pickup_day_) {  // unknown clock (0) shares one "day"
        pickup_day_ = day;
        pickup_today_ = 0;
    }
    if (pickup_today_ >= kPickupDayLimit) return No(MemoryPhraseReason::DayLimit);
    if (pickup_spoke_ && now_ms - pickup_last_ms_ < kPickupMinGapMs)
        return No(MemoryPhraseReason::TooSoon);
    const PhraseTemplate* p = SlotTemplate(t, PhraseContext::FavoriteMention, PhraseSlot::Like);
    if (!p) return No(MemoryPhraseReason::NoTemplate);
    const MemoryStore::Preference& pref =
        memory.PreferenceAt(pickup_total_ % memory.PreferenceCount());
    ++pickup_total_;
    ++pickup_today_;
    pickup_spoke_ = true;
    pickup_last_ms_ = now_ms;
    d.use = true;
    d.reason = MemoryPhraseReason::Used;
    d.phrase_id = p->id;
    d.slot = PhraseSlot::Like;
    d.value = pref.option;
    d.like_slot = pref.slot;
    return d;
}

}  // namespace kid_app
