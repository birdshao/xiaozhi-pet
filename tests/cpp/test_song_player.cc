#include "kid_app/audio_coordinator.h"
#include "kid_app/song_player.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>

using namespace kid_app;

namespace {
SongCatalog Catalog() {
    SongCatalog c;
    assert(c.Offer("song_twinkle"));
    assert(c.Offer("lullaby_moon"));
    assert(c.Offer("song_abc"));
    return c;
}
constexpr uint32_t kMin = 60000;
}  // namespace

int main() {
    // Catalogue: only song_/lullaby_ names with a valid id; sorted; no duplicates.
    {
        SongCatalog c;
        for (const char* bad : {"", "forest_friends", "song_", "lullaby_", "song_A", "song_a b",
                                "../song_x", "song_x/y", "Song_abc", "song__", "songs_abc",
                                "song_ü", "lullaby"})
            assert(!c.Offer(bad));
        assert(!c.Offer(nullptr));
        assert(c.Offer("song_zebra") && c.Offer("lullaby_moon") && c.Offer("song_apple"));
        assert(!c.Offer("song_apple"));  // duplicate
        assert(c.Count() == 3);
        assert(std::strcmp(c.At(0).id, "lullaby_moon") == 0 && c.At(0).kind == SongKind::Lullaby);
        assert(std::strcmp(c.At(1).id, "song_apple") == 0 && c.At(1).kind == SongKind::Song);
        assert(std::strcmp(c.At(2).id, "song_zebra") == 0);
        // Same set in another order: same list (directory order never matters).
        SongCatalog d;
        d.Offer("song_apple");
        d.Offer("song_zebra");
        d.Offer("lullaby_moon");
        for (size_t i = 0; i < 3; ++i) assert(std::strcmp(c.At(i).id, d.At(i).id) == 0);
        SongCatalog full;
        for (size_t i = 0; i < kMaxSongs; ++i) {
            char name[20];
            std::snprintf(name, sizeof(name), "song_n%02zu", i);
            assert(full.Offer(name));
        }
        assert(!full.Offer("song_extra"));
    }

    // Refusals: quiet hours, low battery, a voice session.
    {
        SongPlayer p;
        SongOutput o;
        SongContext c;
        c.quiet_hours = true;
        assert(!p.Open(0, c, Catalog(), &o) && o.hint == SongHint::Quiet && !p.Active());
        c = SongContext{};
        c.low_battery = true;
        assert(!p.Open(0, c, Catalog(), &o) && o.hint == SongHint::LowBattery);
        c = SongContext{};
        c.voice_active = true;
        assert(!p.Open(0, c, Catalog(), &o) && o.hint == SongHint::Busy);
    }

    // No songs on the card: the no_book style picture, then quietly home.
    {
        SongPlayer p;
        SongOutput o;
        SongContext c;
        assert(p.Open(0, c, SongCatalog{}, &o) && o.redraw);
        SongView v = p.View(0);
        assert(v.phase == SongPhase::NoSongs && v.missing == SongMissing::NoSongs);
        assert(!p.Tick(SongPlayer::kNoSongsMs - 1).leave);
        assert(p.Tick(SongPlayer::kNoSongsMs).leave && !p.Active());
        c.card_mounted = false;  // no card at all is told apart
        assert(p.Open(0, c, SongCatalog{}, &o));
        assert(p.View(0).missing == SongMissing::NoCard);
        assert(p.Input(GameInput::Press, 5).leave);  // a press also leaves
    }

    // A nursery rhyme: pick from the list, plays page by page, ends back in the list.
    {
        SongPlayer p;
        SongOutput o;
        assert(p.Open(0, SongContext{}, Catalog(), &o));
        assert(p.View(0).phase == SongPhase::List && p.View(0).count == 3);
        // Sorted: lullaby_moon, song_abc, song_twinkle. Wrap-around both ways.
        assert(p.Input(GameInput::Up, 10).redraw && p.View(10).selected == 2);
        assert(p.Input(GameInput::Down, 20).redraw && p.View(20).selected == 0);
        p.Input(GameInput::Down, 30);
        assert(std::strcmp(p.Chosen().id, "song_abc") == 0);
        o = p.Input(GameInput::Press, 40);  // a song needs no timer
        assert(o.load == 2 && p.View(40).phase == SongPhase::Loading);
        o = p.Loaded(3, 50);
        assert(o.play_page == 1 && p.View(50).phase == SongPhase::Playing && !p.Bedtime());
        assert(p.VolumePct(50) == 100);
        assert(p.PageFinished(60).play_page == 2);
        assert(p.PageFinished(70).play_page == 3);
        o = p.PageFinished(80);  // the end of a song: back to the list, no sleep
        assert(o.play_page == 0 && !o.sleep_now && !o.leave && p.View(80).phase == SongPhase::List);
        // Long = home, stops audio.
        p.Input(GameInput::Press, 90);
        p.Loaded(1, 95);
        o = p.Input(GameInput::Long, 100);
        assert(o.leave && o.stop_audio && !p.Active());
    }

    // Sway: one low-rate frame every 6 s, never faster.
    {
        SongPlayer p;
        p.Open(0, SongContext{}, Catalog(), nullptr);
        p.Input(GameInput::Down, 1);
        p.Input(GameInput::Press, 2);
        p.Loaded(1, 1000);
        int redraws = 0;
        for (uint32_t t = 1000; t <= 1000 + 60000; t += 500)
            if (p.Tick(t).redraw) ++redraws;
        assert(redraws == 10);  // 60 s / 6 s
        assert(p.View(61000).sway < 4);
    }

    // Lullaby: pick, choose 10/20/30 min, loops, fades, then the sleep flow.
    {
        SongPlayer p;
        SongOutput o;
        p.Open(0, SongContext{}, Catalog(), &o);
        o = p.Input(GameInput::Press, 10);  // cursor 0 = lullaby_moon -> timer screen
        assert(o.redraw && o.load == 0 && p.View(10).phase == SongPhase::Timer);
        assert(p.View(10).timer_minutes == 20);  // default
        p.Input(GameInput::Down, 11);
        assert(p.View(11).timer_minutes == 30);
        p.Input(GameInput::Down, 12);
        assert(p.View(12).timer_minutes == 10);  // wraps
        p.Input(GameInput::Up, 13);
        p.Input(GameInput::Up, 14);
        assert(p.View(14).timer_minutes == 20);
        p.Input(GameInput::Down, 15);
        p.Input(GameInput::Up, 16);
        p.Input(GameInput::Up, 17);
        assert(p.View(17).timer_minutes == 10);
        o = p.Input(GameInput::Press, 20);
        assert(o.load == 1 && !o.sleep_now);
        const uint32_t t0 = 100000;
        o = p.Loaded(2, t0);
        assert(o.play_page == 1 && p.Bedtime());
        assert(p.View(t0).minutes_left == 10);
        assert(p.PageFinished(t0 + 1000).play_page == 2);
        o = p.PageFinished(t0 + 2000);  // end of the lullaby before the timer: loop
        assert(o.play_page == 1 && !o.sleep_now);
        // Fade only in the last 30 s.
        assert(p.VolumePct(t0 + 10 * kMin - 31000) == 100);
        const uint8_t mid = p.VolumePct(t0 + 10 * kMin - 15000);
        assert(mid < 100 && mid > SongPlayer::kFadeFloorPct);
        assert(p.VolumePct(t0 + 10 * kMin - 1) <= SongPlayer::kFadeFloorPct + 1);
        assert(!p.Tick(t0 + 10 * kMin - 1).sleep_now);
        o = p.Tick(t0 + 10 * kMin);
        assert(o.sleep_now && o.stop_audio && o.leave && !p.Active());
    }

    // The timer runs out exactly when a loop ends: still one sleep request.
    {
        SongPlayer p;
        p.Open(0, SongContext{}, Catalog(), nullptr);
        p.Input(GameInput::Press, 1);
        p.Input(GameInput::Press, 2);  // 20 minutes
        p.Loaded(1, 0);
        const SongOutput o = p.PageFinished(20 * kMin + 5);
        assert(o.sleep_now && o.stop_audio && o.leave);
        assert(!p.Tick(20 * kMin + 6).sleep_now);  // not twice
    }

    // Interrupt (BOOT / PWR / voice / quiet hours): stops audio, no sleep request here
    // (PWR runs its own sleep flow), and everything is idle afterwards.
    {
        SongPlayer p;
        p.Open(0, SongContext{}, Catalog(), nullptr);
        p.Input(GameInput::Press, 1);
        p.Input(GameInput::Press, 2);
        p.Loaded(1, 3);
        SongOutput o = p.Interrupt();
        assert(o.stop_audio && o.leave && !o.sleep_now && !p.Active() && !p.Bedtime());
        assert(!p.Interrupt().leave);  // nothing left to interrupt
        p.Open(0, SongContext{}, Catalog(), nullptr);  // list screen: no audio to stop
        o = p.Interrupt();
        assert(!o.stop_audio && o.leave);
    }

    // Stop with Press returns to the list; an unreadable book never scolds; a stuck load times out.
    {
        SongPlayer p;
        p.Open(0, SongContext{}, Catalog(), nullptr);
        p.Input(GameInput::Down, 1);
        p.Input(GameInput::Press, 2);
        SongOutput o = p.Loaded(0, 3);  // bad book
        assert(o.redraw && p.View(3).phase == SongPhase::List && !o.leave);
        p.Input(GameInput::Press, 4);
        assert(!p.Tick(4 + SongPlayer::kLoadTimeoutMs - 1).redraw);
        assert(p.Tick(4 + SongPlayer::kLoadTimeoutMs).redraw && p.View(5).phase == SongPhase::List);
        p.Input(GameInput::Press, 20);
        p.Loaded(1, 21);
        o = p.Input(GameInput::Press, 22);
        assert(o.stop_audio && p.View(22).phase == SongPhase::List);
        assert(!p.Input(GameInput::Down, 23).stop_audio);
        // Idle on the list for 2 minutes: quietly home.
        assert(p.Tick(23 + SongPlayer::kIdleExitMs).leave);
    }

    // Audio arbitration with the real coordinator: a song is a Story-class stream;
    // BOOT (voice) preempts it and it does NOT resume by itself after the
    // player stopped; PWR (StopAll) kills it; quiet blocks it.
    {
        AudioCoordinator audio;
        assert(audio.StartStory(10).accepted);
        AudioPlan plan = audio.BeginVoice(11);
        assert(plan.accepted && plan.actions[0].stream == AudioStream::Story);
        SongPlayer p;
        p.Open(0, SongContext{}, Catalog(), nullptr);
        p.Input(GameInput::Press, 1);
        p.Input(GameInput::Press, 2);
        p.Loaded(1, 3);
        assert(p.Interrupt().stop_audio);        // the board stops the song when BOOT is pressed
        audio.StopAll();                         // ...and tells the arbiter it is gone
        assert(audio.Active() == AudioStream::None);
        assert(!audio.EndVoice(11).accepted);    // nothing is resumed behind the player's back
        assert(audio.StartStory(12).accepted);
        plan = audio.StopAll();                  // PWR
        assert(plan.count == 1 && audio.Active() == AudioStream::None);
        audio.SetQuiet(true);
        assert(!audio.StartStory(13).accepted);
    }

    std::cout << "song player tests passed\n";
    return 0;
}
