#pragma once

#include "kid_app/mini_game.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace kid_app {

// V8-02 nursery-rhyme and lullaby player (pure core; the board streams the
// PCM through the existing local-PCM path, see docs/V8_SOUND_AND_ROUTINES.md).
//
// Format: no new one. A song is an ordinary KPB book on the TF card
// (/sdcard/books/<id>.kpb, PCM 24 kHz mono, at most 60 s per page, up to 32
// pages = a song of up to 32 minutes cut into pages; the page picture is
// ignored). The file NAME marks it: `song_<name>` is a nursery rhyme,
// `lullaby_<name>` a lullaby (id rule [a-z0-9][a-z0-9_-]{2,62}). The pages
// are read with the same StoryPackReader/CRC check as stories; the player
// only needs the page count.
//
// Bedtime mode: a lullaby asks for 10 / 20 / 30 minutes; it loops until the
// time is up, fades the last 30 s, stops, then the board runs the normal
// sleep flow (3 s cancel window). Quiet hours and a low battery refuse the
// player. Quiet hours (decided by the owner, V8 review): only lullabies are
// offered, at a low volume (kQuietVolumePct), and the timer still ends in
// the sleep flow; nursery rhymes and everything else stay closed.
enum class SongKind : uint8_t { Song, Lullaby };

constexpr size_t kMaxSongs = 12;
constexpr size_t kSongIdBytes = 40;

struct SongEntry {
    char id[kSongIdBytes] = {};
    SongKind kind = SongKind::Song;
};

// Collects the files the board saw in /sdcard/books: Offer() each stem.
class SongCatalog {
public:
    // Accepts only `song_...` / `lullaby_...` with a valid id; sorted by id
    // so the order never depends on directory order. False if rejected,
    // duplicate or full.
    bool Offer(const char* stem);
    size_t Count() const { return count_; }
    const SongEntry& At(size_t i) const { return entries_[i]; }
    void Clear() { count_ = 0; }
    // Quiet hours: only lullabies stay.
    SongCatalog LullabiesOnly() const;

private:
    std::array<SongEntry, kMaxSongs> entries_{};
    size_t count_ = 0;
};

enum class SongPhase : uint8_t { Idle, NoSongs, List, Timer, Loading, Playing };
enum class SongMissing : uint8_t { None, NoCard, NoSongs };
enum class SongHint : uint8_t { None, Quiet, LowBattery, Busy };
const char* SongHintName(SongHint hint);

struct SongContext {
    bool quiet_hours = false;
    bool low_battery = false;
    bool voice_active = false;
    bool card_mounted = true;
};

struct SongOutput {
    bool redraw = false;      // draw View() (one partial refresh)
    uint8_t load = 0;         // open this list index's book and call Loaded(); 0 = none, else index+1
    uint8_t play_page = 0;    // stream this page (1-based) of the open book
    bool stop_audio = false;  // stop the stream now
    bool sleep_now = false;   // run the sleep flow (timer ran out)
    bool leave = false;       // back home
    SongHint hint = SongHint::None;
};

struct SongView {
    SongPhase phase = SongPhase::Idle;
    SongMissing missing = SongMissing::None;
    SongKind kind = SongKind::Song;
    uint8_t selected = 0;      // list cursor
    uint8_t count = 0;         // songs in the list
    uint8_t timer_minutes = 0; // 0 = none (songs); 10 / 20 / 30 for a lullaby
    uint8_t sway = 0;          // 0..3: TuanTuan leans left, centre, right, centre
    uint16_t minutes_left = 0; // lullaby timer
};

class SongPlayer {
public:
    static constexpr uint32_t kSwayMs = 6000;        // one partial refresh per step
    // The panel budgets partial refreshes per region (refresh_planner.h): after
    // this many frames TuanTuan sits still, so a 30-minute lullaby never forces
    // a flashing full refresh while the child falls asleep.
    static constexpr uint8_t kMaxSwayFrames = 24;
    static constexpr uint32_t kFadeMs = 30000;       // last 30 s of a lullaby
    static constexpr uint8_t kFadeFloorPct = 35;
    static constexpr uint8_t kQuietVolumePct = 30;   // same as the quiet prompt volume
    static constexpr uint32_t kNoSongsMs = 15000;    // hint picture, then quietly home
    static constexpr uint32_t kIdleExitMs = 120000;  // list or timer left alone
    static constexpr uint32_t kLoadTimeoutMs = 90000;  // opening checks every CRC of the book
    static constexpr std::array<uint8_t, 3> kTimerMinutes = {10, 20, 30};
    static constexpr size_t kDefaultTimer = 1;       // 20 minutes

    // The catalogue is copied: later scans do not disturb a running player.
    bool Open(uint32_t now_ms, const SongContext& context, const SongCatalog& catalog,
              SongOutput* out);
    // Dial: Up/Down move the cursor (list) or the timer choice; Press picks;
    // Long = home. While playing, Press stops (back to the list), Up/Down are
    // ignored (no accidental skips).
    SongOutput Input(GameInput input, uint32_t now_ms);
    // The board opened the chosen book (pages >= 1) or failed (0 -> list again).
    SongOutput Loaded(uint8_t pages, uint32_t now_ms);
    // The stream of `page` ended on its own.
    SongOutput PageFinished(uint32_t now_ms);
    SongOutput Tick(uint32_t now_ms);
    // BOOT talk, PWR sleep, voice session, Pet mode off.
    SongOutput Interrupt();
    // Quiet hours begin / end while the player is open. Beginning: a nursery
    // rhyme stops (leave); a lullaby keeps playing; a list is cut down to the
    // lullabies (none left: leave).
    SongOutput SetQuiet(bool quiet, uint32_t now_ms);
    bool Quiet() const { return quiet_; }

    bool Active() const { return phase_ != SongPhase::Idle; }
    SongView View(uint32_t now_ms) const;
    // Software volume for the current block (100 except the fade-out).
    uint8_t VolumePct(uint32_t now_ms) const;
    uint8_t Pages() const { return pages_; }
    uint8_t Page() const { return page_; }
    const SongEntry& Chosen() const { return catalog_.At(selected_); }
    // True while the chosen song is a lullaby that will end with sleep.
    bool Bedtime() const { return phase_ == SongPhase::Playing && timer_minutes_ != 0; }

private:
    static bool Reached(uint32_t now_ms, uint32_t at_ms) {
        return static_cast<int32_t>(now_ms - at_ms) >= 0;
    }
    SongOutput Start(uint32_t now_ms);
    SongOutput Back(bool stop);

    SongCatalog catalog_;
    bool quiet_ = false;
    SongPhase phase_ = SongPhase::Idle;
    SongMissing missing_ = SongMissing::None;
    uint8_t selected_ = 0;
    uint8_t timer_choice_ = kDefaultTimer;
    uint8_t timer_minutes_ = 0;
    uint8_t pages_ = 0;
    uint8_t page_ = 0;
    uint8_t sway_ = 0;
    uint8_t sway_frames_ = 0;
    uint32_t phase_ms_ = 0;     // when the phase began
    uint32_t last_input_ms_ = 0;
    uint32_t sway_ms_ = 0;
    uint32_t end_ms_ = 0;       // lullaby: when to stop
};

}  // namespace kid_app
