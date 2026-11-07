#include "kid_app/song_player.h"

#include <cstring>

namespace kid_app {

namespace {
constexpr char kSongPrefix[] = "song_";
constexpr char kLullabyPrefix[] = "lullaby_";

bool StartsWith(const char* text, const char* prefix) {
    return std::strncmp(text, prefix, std::strlen(prefix)) == 0;
}

// Same id rule as story books / sound packs: [a-z0-9][a-z0-9_-]{2,62}.
bool ValidId(const char* id, size_t* length) {
    size_t n = 0;
    for (; id[n]; ++n) {
        const char c = id[n];
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!(alnum || (n > 0 && (c == '_' || c == '-')))) return false;
        if (n >= 63) return false;
    }
    *length = n;
    return n >= 3;
}
}  // namespace

const char* SongHintName(SongHint hint) {
    switch (hint) {
        case SongHint::None: return "none";
        case SongHint::Quiet: return "quiet";
        case SongHint::LowBattery: return "low_battery";
        case SongHint::Busy: return "busy";
    }
    return "none";
}

bool SongCatalog::Offer(const char* stem) {
    if (!stem || count_ >= kMaxSongs) return false;
    size_t length = 0;
    if (!ValidId(stem, &length) || length >= kSongIdBytes) return false;
    SongKind kind;
    if (StartsWith(stem, kLullabyPrefix)) kind = SongKind::Lullaby;
    else if (StartsWith(stem, kSongPrefix)) kind = SongKind::Song;
    else return false;
    // The prefix alone is not a name.
    const size_t prefix = std::strlen(kind == SongKind::Lullaby ? kLullabyPrefix : kSongPrefix);
    if (length == prefix || stem[prefix] == '_' || stem[prefix] == '-') return false;
    size_t at = 0;
    while (at < count_ && std::strcmp(entries_[at].id, stem) < 0) ++at;
    if (at < count_ && std::strcmp(entries_[at].id, stem) == 0) return false;
    for (size_t i = count_; i > at; --i) entries_[i] = entries_[i - 1];
    entries_[at] = SongEntry{};
    std::memcpy(entries_[at].id, stem, length);
    entries_[at].kind = kind;
    ++count_;
    return true;
}

bool SongPlayer::Open(uint32_t now_ms, const SongContext& c, const SongCatalog& catalog,
                      SongOutput* out) {
    SongOutput o;
    if (Active()) {
        if (out) *out = o;
        return false;
    }
    if (c.quiet_hours) o.hint = SongHint::Quiet;
    else if (c.low_battery) o.hint = SongHint::LowBattery;
    else if (c.voice_active) o.hint = SongHint::Busy;
    if (o.hint != SongHint::None) {
        if (out) *out = o;
        return false;
    }
    catalog_ = catalog;
    selected_ = 0;
    timer_choice_ = kDefaultTimer;
    timer_minutes_ = 0;
    pages_ = page_ = 0;
    phase_ms_ = last_input_ms_ = now_ms;
    if (catalog_.Count() == 0) {
        phase_ = SongPhase::NoSongs;
        missing_ = c.card_mounted ? SongMissing::NoSongs : SongMissing::NoCard;
    } else {
        phase_ = SongPhase::List;
        missing_ = SongMissing::None;
    }
    o.redraw = true;
    if (out) *out = o;
    return true;
}

SongOutput SongPlayer::Back(bool stop) {
    SongOutput o;
    o.stop_audio = stop;
    o.leave = true;
    phase_ = SongPhase::Idle;
    timer_minutes_ = 0;
    return o;
}

SongOutput SongPlayer::Start(uint32_t now_ms) {
    SongOutput o;
    phase_ = SongPhase::Loading;
    phase_ms_ = now_ms;
    timer_minutes_ = catalog_.At(selected_).kind == SongKind::Lullaby ? kTimerMinutes[timer_choice_] : 0;
    o.load = static_cast<uint8_t>(selected_ + 1);
    return o;
}

SongOutput SongPlayer::Input(GameInput input, uint32_t now_ms) {
    SongOutput o;
    last_input_ms_ = now_ms;
    switch (phase_) {
        case SongPhase::Idle:
            break;
        case SongPhase::NoSongs:
            if (input == GameInput::Press || input == GameInput::Long) return Back(false);
            break;
        case SongPhase::List: {
            const uint8_t n = static_cast<uint8_t>(catalog_.Count());
            if (input == GameInput::Long) return Back(false);
            if (input == GameInput::Up || input == GameInput::Down) {
                selected_ = input == GameInput::Down ? static_cast<uint8_t>((selected_ + 1) % n)
                                                     : static_cast<uint8_t>((selected_ + n - 1) % n);
                o.redraw = true;
            } else if (input == GameInput::Press) {
                if (catalog_.At(selected_).kind == SongKind::Lullaby) {
                    phase_ = SongPhase::Timer;  // bedtime: how long?
                    o.redraw = true;
                } else {
                    return Start(now_ms);
                }
            }
            break;
        }
        case SongPhase::Timer:
            if (input == GameInput::Long) return Back(false);
            if (input == GameInput::Up || input == GameInput::Down) {
                const size_t n = kTimerMinutes.size();
                timer_choice_ = static_cast<uint8_t>(input == GameInput::Down ? (timer_choice_ + 1) % n
                                                                              : (timer_choice_ + n - 1) % n);
                o.redraw = true;
            } else if (input == GameInput::Press) {
                return Start(now_ms);
            }
            break;
        case SongPhase::Loading:
            if (input == GameInput::Long) return Back(false);
            break;
        case SongPhase::Playing:
            if (input == GameInput::Long) return Back(true);
            if (input == GameInput::Press) {  // stop, back to the list
                o.stop_audio = true;
                o.redraw = true;
                phase_ = SongPhase::List;
                timer_minutes_ = 0;
            }
            break;
    }
    return o;
}

SongOutput SongPlayer::Loaded(uint8_t pages, uint32_t now_ms) {
    SongOutput o;
    if (phase_ != SongPhase::Loading) return o;
    if (pages == 0) {  // unreadable book: back to the list, nothing scolded
        phase_ = SongPhase::List;
        timer_minutes_ = 0;
        o.redraw = true;
        return o;
    }
    phase_ = SongPhase::Playing;
    pages_ = pages;
    page_ = 1;
    sway_ = 0;
    sway_ms_ = now_ms;
    end_ms_ = now_ms + static_cast<uint32_t>(timer_minutes_) * 60000u;
    o.play_page = page_;
    o.redraw = true;
    return o;
}

SongOutput SongPlayer::PageFinished(uint32_t now_ms) {
    SongOutput o;
    if (phase_ != SongPhase::Playing) return o;
    if (page_ < pages_) {
        o.play_page = ++page_;
        return o;
    }
    if (timer_minutes_ != 0) {  // lullaby: loop until the time is up
        if (Reached(now_ms, end_ms_)) return Tick(now_ms);
        page_ = 1;
        o.play_page = page_;
        return o;
    }
    phase_ = SongPhase::List;  // a song ends: back to the list
    o.redraw = true;
    return o;
}

SongOutput SongPlayer::Tick(uint32_t now_ms) {
    SongOutput o;
    switch (phase_) {
        case SongPhase::Idle:
            break;
        case SongPhase::NoSongs:
            if (static_cast<uint32_t>(now_ms - phase_ms_) >= kNoSongsMs) return Back(false);
            break;
        case SongPhase::List:
        case SongPhase::Timer:
            if (static_cast<uint32_t>(now_ms - last_input_ms_) >= kIdleExitMs) return Back(false);
            break;
        case SongPhase::Loading:
            if (static_cast<uint32_t>(now_ms - phase_ms_) >= kLoadTimeoutMs) {
                phase_ = SongPhase::List;
                timer_minutes_ = 0;
                o.redraw = true;
            }
            break;
        case SongPhase::Playing:
            if (timer_minutes_ != 0 && Reached(now_ms, end_ms_)) {
                // Time is up: silence first, then bed.
                o.stop_audio = true;
                o.sleep_now = true;
                o.leave = true;
                phase_ = SongPhase::Idle;
                timer_minutes_ = 0;
                break;
            }
            if (static_cast<uint32_t>(now_ms - sway_ms_) >= kSwayMs) {
                sway_ms_ = now_ms;
                sway_ = static_cast<uint8_t>((sway_ + 1) % 4);
                o.redraw = true;
            }
            break;
    }
    return o;
}

SongOutput SongPlayer::Interrupt() {
    if (!Active()) return SongOutput{};
    return Back(phase_ == SongPhase::Playing || phase_ == SongPhase::Loading);
}

uint8_t SongPlayer::VolumePct(uint32_t now_ms) const {
    if (phase_ != SongPhase::Playing || timer_minutes_ == 0) return 100;
    const uint32_t left = end_ms_ - now_ms;
    if (static_cast<int32_t>(left) <= 0) return kFadeFloorPct;
    if (left >= kFadeMs) return 100;
    return static_cast<uint8_t>(kFadeFloorPct + (100u - kFadeFloorPct) * left / kFadeMs);
}

SongView SongPlayer::View(uint32_t now_ms) const {
    SongView v;
    v.phase = phase_;
    v.missing = missing_;
    v.selected = selected_;
    v.count = static_cast<uint8_t>(catalog_.Count());
    if (catalog_.Count() != 0) v.kind = catalog_.At(selected_).kind;
    if (phase_ == SongPhase::Timer) v.timer_minutes = kTimerMinutes[timer_choice_];
    if (phase_ == SongPhase::Playing) {
        v.timer_minutes = timer_minutes_;
        v.sway = sway_;
        if (timer_minutes_ != 0) {
            const uint32_t left = end_ms_ - now_ms;
            v.minutes_left = static_cast<int32_t>(left) <= 0 ? 0 : static_cast<uint16_t>((left + 59999u) / 60000u);
        }
    }
    return v;
}

}  // namespace kid_app
