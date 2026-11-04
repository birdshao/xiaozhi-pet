#include "kid_app/scene_model.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

using kid_app::Emotion;
using kid_app::MonoFrame;
using kid_app::SceneComposer;
using kid_app::SceneKind;
using kid_app::SceneModel;

int main() {
    static_assert(MonoFrame::kSize == 48000, "Expected 800*480/8");
    MonoFrame frame;
    assert(frame.Data().size() == 48000);
    assert(std::all_of(frame.Data().begin(), frame.Data().end(),
                       [](uint8_t n) { return n == 0xFF; }));
    frame.SetPixel(0, 0, true);
    frame.SetPixel(799, 479, true);
    frame.SetPixel(-1, 0, true);  // Bounds safety.
    frame.SetPixel(800, 10, true);
    assert(frame.IsBlack(0, 0));
    assert(frame.IsBlack(799, 479));
    assert(!frame.IsBlack(-1, 0));

    // Byte-wise spans: exact inclusive ends inside and across bytes, clipping,
    // repeating masks, and painting white back.
    {
        MonoFrame span;
        span.PaintSpan(3, 20, 5, 0xFF, true);
        for (int x = 0; x < 30; ++x) assert(span.IsBlack(x, 5) == (x >= 3 && x <= 20));
        assert(!span.IsBlack(10, 4) && !span.IsBlack(10, 6));
        span.PaintSpan(-50, 2000, 7, 0xAA, true);  // every other pixel, clipped
        for (int x = 0; x < MonoFrame::kWidth; ++x) assert(span.IsBlack(x, 7) == (x % 2 == 0));
        span.PaintSpan(5, 5, 5, 0xFF, false);
        assert(!span.IsBlack(5, 5) && span.IsBlack(4, 5) && span.IsBlack(6, 5));
        span.PaintSpan(9, 2, 9, 0xFF, true);  // empty
        span.PaintSpan(0, 799, 480, 0xFF, true);  // off-frame row
        for (int x = 0; x < MonoFrame::kWidth; ++x) assert(!span.IsBlack(x, 9));
    }

    MonoFrame happy, sleepy, book;
    SceneComposer::Render(SceneModel{SceneKind::Home, Emotion::Happy, 1}, happy);
    SceneComposer::Render(SceneModel{SceneKind::Home, Emotion::Sleepy, 1}, sleepy);
    SceneComposer::Render(SceneModel{SceneKind::Reader, Emotion::Curious, 2}, book);
    assert(happy.Data() != sleepy.Data());
    assert(happy.Data() != book.Data());
    assert(sleepy.Data() != book.Data());
    assert(book.IsBlack(38, 30));     // Reader outer frame.

    SceneComposer::Render(SceneModel{SceneKind::Home, Emotion::Happy, 1}, book);
    assert(book.Data() == happy.Data());  // Rendering is deterministic.

    // Home action rail: five 160 px card slots on y 380..479; the selected
    // card is inverted (solid black). Selection changes only touch the rail.
    {
        static_assert(SceneComposer::kRailTop == 380, "rail band");
        static_assert(SceneComposer::kRailSlotWidth * 5 == MonoFrame::kWidth, "slots");
        MonoFrame story, feed;
        SceneComposer::Render(SceneModel{SceneKind::Home, Emotion::Neutral, 1, 1}, story);
        SceneComposer::Render(SceneModel{SceneKind::Home, Emotion::Neutral, 1, 3}, feed);
        assert(story.Data() != feed.Data());
        for (int y = 0; y < SceneComposer::kRailTop; ++y) {
            for (int x = 0; x < MonoFrame::kWidth; ++x)
                assert(story.IsBlack(x, y) == feed.IsBlack(x, y));
        }
        // Card interiors near the top-left corner: black only when selected.
        assert(story.IsBlack(180, 400) && !feed.IsBlack(180, 400));
        assert(feed.IsBlack(500, 400) && !story.IsBlack(500, 400));
        // Every slot shows an icon (some ink near its centre band).
        for (int slot = 0; slot < 5; ++slot) {
            bool ink = false;
            for (int y = 395; y < 465 && !ink; ++y)
                for (int x = slot * 160 + 40; x < slot * 160 + 120 && !ink; ++x)
                    ink = story.IsBlack(x, y);
            assert(ink);
        }
        // Out-of-range selection draws no highlight instead of crashing.
        MonoFrame none;
        SceneComposer::Render(SceneModel{SceneKind::Home, Emotion::Neutral, 1, 9}, none);
        assert(!none.IsBlack(180, 400) && !none.IsBlack(500, 400));
        // The pet face stays fully above the rail.
        for (int x = 0; x < MonoFrame::kWidth; ++x)
            assert(!story.IsBlack(x, SceneComposer::kRailTop - 2));
    }

    // Expression and animation poses only touch the pet area: the action
    // rail band is identical (RefreshPlanner keeps separate budgets).
    {
        MonoFrame still;
        SceneComposer::Render(SceneModel{SceneKind::Home, Emotion::Neutral, 1, 2}, still);
        for (Emotion emotion : {Emotion::Neutral, Emotion::Happy, Emotion::Curious,
                                Emotion::Sleepy, Emotion::Surprised}) {
            for (kid_app::ScenePose pose :
                 {kid_app::ScenePose::Still, kid_app::ScenePose::Blink, kid_app::ScenePose::Wave,
                  kid_app::ScenePose::Hearts1, kid_app::ScenePose::Hearts2,
                  kid_app::ScenePose::Giggle, kid_app::ScenePose::Dizzy1,
                  kid_app::ScenePose::Dizzy2, kid_app::ScenePose::Sunrise,
                  kid_app::ScenePose::Morning}) {
                MonoFrame other;
                SceneComposer::Render(SceneModel{SceneKind::Home, emotion, 1, 2, pose}, other);
                for (int y = SceneComposer::kRailTop - 2; y < MonoFrame::kHeight; ++y)
                    for (int x = 0; x < MonoFrame::kWidth; ++x)
                        assert(other.IsBlack(x, y) == still.IsBlack(x, y));
            }
        }
        // Each pose is visibly different from the resting picture.
        for (kid_app::ScenePose pose :
             {kid_app::ScenePose::Blink, kid_app::ScenePose::Wave, kid_app::ScenePose::Hearts1,
              kid_app::ScenePose::Hearts2, kid_app::ScenePose::Giggle, kid_app::ScenePose::Dizzy1,
              kid_app::ScenePose::Dizzy2, kid_app::ScenePose::Sunrise,
              kid_app::ScenePose::Morning}) {
            MonoFrame other;
            SceneComposer::Render(SceneModel{SceneKind::Home, Emotion::Neutral, 1, 2, pose}, other);
            assert(other.Data() != still.Data());
        }
        // Good morning (V3-04): a sun in the top-left corner (rising behind
        // a cloud, then up), the stretch differs from the wave, and the pet
        // still clears the rail.
        {
            MonoFrame sunrise, morning;
            SceneComposer::Render(SceneModel{SceneKind::Home, Emotion::Happy, 1, 2,
                                             kid_app::ScenePose::Sunrise}, sunrise);
            SceneComposer::Render(SceneModel{SceneKind::Home, Emotion::Happy, 1, 2,
                                             kid_app::ScenePose::Morning}, morning);
            assert(sunrise.Data() != morning.Data());
            auto ink = [](const MonoFrame& f, int x0, int y0, int x1, int y1) {
                int n = 0;
                for (int y = y0; y < y1; ++y)
                    for (int x = x0; x < x1; ++x) n += f.IsBlack(x, y) ? 1 : 0;
                return n;
            };
            // Sun outline and rays near (132, 140); none on the resting home.
            assert(ink(morning, 40, 50, 230, 230) > 1500 && ink(sunrise, 40, 50, 230, 230) > 1500);
            assert(ink(still, 60, 60, 210, 210) < 600);
            // Rays above the sun: longer once it is up.
            assert(ink(morning, 60, 50, 210, 92) > ink(sunrise, 60, 50, 210, 92));
            // The cloud's lower outline only while rising.
            assert(ink(sunrise, 60, 196, 210, 216) > ink(morning, 60, 196, 210, 216));
            // Stretching: both paws up beside the head; waving: right paw only.
            assert(ink(sunrise, 216, 90, 272, 140) > 300 && ink(sunrise, 528, 90, 584, 140) > 300);
            assert(ink(morning, 216, 90, 272, 140) < 100);
            for (int x = 0; x < MonoFrame::kWidth; ++x)
                assert(!sunrise.IsBlack(x, SceneComposer::kRailTop - 2) &&
                       !morning.IsBlack(x, SceneComposer::kRailTop - 2));
        }
        // Falling asleep: Zzz1 < Zzz2 < Still (all three letters).
        MonoFrame z1, z2, z3;
        SceneComposer::Render(SceneModel{SceneKind::Sleep, Emotion::Sleepy, 1, 1,
                                         kid_app::ScenePose::Zzz1}, z1);
        SceneComposer::Render(SceneModel{SceneKind::Sleep, Emotion::Sleepy, 1, 1,
                                         kid_app::ScenePose::Zzz2}, z2);
        SceneComposer::Render(SceneModel{SceneKind::Sleep, Emotion::Sleepy, 1, 1}, z3);
        assert(z1.Data() != z2.Data() && z2.Data() != z3.Data());
        auto white = [](const MonoFrame& f) {
            int n = 0;
            for (int y = 0; y < 300; ++y)
                for (int x = 0; x < 300; ++x) n += f.IsBlack(x, y) ? 0 : 1;
            return n;
        };
        assert(white(z1) < white(z2) && white(z2) < white(z3));  // white letters on the sky
    }

    // Sleep scene: what the ePaper keeps after power-off. No action rail
    // (nothing to press while asleep), sleeping eyes, a moon and "Zzz".
    {
        MonoFrame sleep, home;
        SceneComposer::Render(SceneModel{SceneKind::Sleep, Emotion::Happy, 3, 2}, sleep);
        SceneComposer::Render(SceneModel{SceneKind::Home, Emotion::Sleepy, 1, 1}, home);
        assert(sleep.Data() != home.Data());
        for (int x = 0; x < MonoFrame::kWidth; ++x)
            assert(!sleep.IsBlack(x, SceneComposer::kRailTop));  // no rail line
        // Always the sleeping face, whatever emotion/page/selection it gets.
        MonoFrame other;
        SceneComposer::Render(SceneModel{SceneKind::Sleep, Emotion::Surprised, 9, 0}, other);
        assert(other.Data() == sleep.Data());
        bool moon = false;  // top-right corner holds the moon
        for (int y = 30; y < 140 && !moon; ++y)
            for (int x = 600; x < 760 && !moon; ++x) moon = sleep.IsBlack(x, y);
        assert(moon);
    }

    // Status band (V2-05): two top corner boxes. Status changes only touch
    // pixels inside them (never the pet or the rail), nothing else on the
    // home screen ever draws there, and reader/sleep ignore the status.
    {
        using kid_app::NetworkState;
        using kid_app::ScenePose;
        using kid_app::StatusBar;
        static_assert(SceneComposer::kStatusBottom <= 40, "thin band");
        static_assert(SceneComposer::kStatusLeftEnd < SceneComposer::kStatusRightStart, "boxes");
        assert(SceneComposer::InStatusBox(0, 0) && SceneComposer::InStatusBox(799, 39));
        assert(!SceneComposer::InStatusBox(400, 10) && !SceneComposer::InStatusBox(10, 40));
        auto home = [](const StatusBar& status, Emotion emotion = Emotion::Neutral,
                       ScenePose pose = ScenePose::Still) {
            SceneModel model{SceneKind::Home, emotion, 1, 1, pose};
            model.status = status;
            return model;
        };
        auto ink_in = [](const MonoFrame& f, int x0, int x1, int y0, int y1) {
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                    if (f.IsBlack(x, y)) return true;
            return false;
        };
        StatusBar full;
        full.hour = 21;
        full.minute = 58;
        full.temp_c = -8;
        full.battery_bars = 4;
        full.charging = true;
        full.quiet = true;
        full.network = NetworkState::Offline;
        StatusBar other;
        other.hour = 10;
        other.minute = 36;
        other.temp_c = 47;
        other.battery_bars = 2;
        other.network = NetworkState::Online;
        MonoFrame none, a, b;
        SceneComposer::Render(home(StatusBar{}), none);
        SceneComposer::Render(home(full), a);
        SceneComposer::Render(home(other), b);
        assert(a.Data() != none.Data() && b.Data() != none.Data() && a.Data() != b.Data());
        for (int y = 0; y < MonoFrame::kHeight; ++y)
            for (int x = 0; x < MonoFrame::kWidth; ++x)
                if (!SceneComposer::InStatusBox(x, y))
                    assert(a.IsBlack(x, y) == none.IsBlack(x, y) &&
                           b.IsBlack(x, y) == none.IsBlack(x, y));

        // The boxes are reserved: blank for every expression and pose while
        // the status is unknown (so the planner's regions never overlap).
        for (Emotion emotion : {Emotion::Neutral, Emotion::Happy, Emotion::Curious,
                                Emotion::Sleepy, Emotion::Surprised}) {
            for (ScenePose pose : {ScenePose::Still, ScenePose::Blink, ScenePose::Wave,
                                   ScenePose::Hearts1, ScenePose::Hearts2, ScenePose::Giggle,
                                   ScenePose::Dizzy1, ScenePose::Dizzy2, ScenePose::Sunrise,
                                   ScenePose::Morning}) {
                MonoFrame f;
                SceneComposer::Render(home(StatusBar{}, emotion, pose), f);
                for (int y = 0; y < SceneComposer::kStatusBottom; ++y)
                    for (int x = 0; x < MonoFrame::kWidth; ++x)
                        if (SceneComposer::InStatusBox(x, y)) assert(!f.IsBlack(x, y));
            }
        }

        // Each field appears in its own place and only when known.
        auto only = [&](const StatusBar& s, int x0, int x1) {
            MonoFrame f;
            SceneComposer::Render(home(s), f);
            for (int y = 0; y < SceneComposer::kStatusBottom; ++y)
                for (int x = 0; x < MonoFrame::kWidth; ++x)
                    if (SceneComposer::InStatusBox(x, y) && (x < x0 || x > x1))
                        assert(!f.IsBlack(x, y));
            assert(ink_in(f, x0, x1, 0, SceneComposer::kStatusBottom - 1));
        };
        StatusBar s;
        s.hour = 7;
        s.minute = 42;
        only(s, 0, 115);  // clock
        s = StatusBar{};
        s.quiet = true;
        only(s, 120, 156);  // moon
        s = StatusBar{};
        s.network = NetworkState::Online;
        only(s, 160, 215);  // Wi-Fi
        s.network = NetworkState::Offline;
        only(s, 160, 215);
        s = StatusBar{};
        s.temp_c = 24;
        only(s, 632, 715);  // temperature
        s = StatusBar{};
        s.battery_bars = 0;
        only(s, 740, 799);  // empty battery outline
        s.charging = true;
        only(s, 716, 799);  // bolt next to it
        s = StatusBar{};
        s.charging = true;  // no battery reading: no bolt either
        MonoFrame f;
        SceneComposer::Render(home(s), f);
        assert(f.Data() == none.Data());
        s = StatusBar{};
        s.hour = 7;  // a clock needs both hour and minute
        SceneComposer::Render(home(s), f);
        assert(f.Data() == none.Data());

        // Every digit, battery level, sign and network state looks different.
        std::vector<MonoFrame> digits(10);  // heap: 48 KB each
        for (int d = 0; d < 10; ++d) {
            s = StatusBar{};
            s.hour = 12;
            s.minute = static_cast<uint8_t>(50 + d);
            SceneComposer::Render(home(s), digits[d]);
            for (int e = 0; e < d; ++e) assert(digits[d].Data() != digits[e].Data());
        }
        std::vector<MonoFrame> levels(StatusBar::kBatteryBars + 1);
        for (int n = 0; n <= StatusBar::kBatteryBars; ++n) {
            s = StatusBar{};
            s.battery_bars = static_cast<uint8_t>(n);
            SceneComposer::Render(home(s), levels[n]);
            for (int m = 0; m < n; ++m) assert(levels[n].Data() != levels[m].Data());
        }
        MonoFrame minus, plus, teen;
        s = StatusBar{};
        s.temp_c = -5;
        SceneComposer::Render(home(s), minus);
        s.temp_c = 5;
        SceneComposer::Render(home(s), plus);
        s.temp_c = 15;
        SceneComposer::Render(home(s), teen);
        assert(minus.Data() != plus.Data() && plus.Data() != teen.Data());
        s.temp_c = 100;  // cannot be drawn: hidden
        SceneComposer::Render(home(s), f);
        assert(f.Data() == none.Data());

        // Reader and sleep pictures never show the band.
        for (SceneKind kind : {SceneKind::Reader, SceneKind::Sleep}) {
            SceneModel plain{kind, Emotion::Happy, 2, 1};
            SceneModel with = plain;
            with.status = full;
            MonoFrame p, w;
            SceneComposer::Render(plain, p);
            SceneComposer::Render(with, w);
            assert(p.Data() == w.Data());
        }
    }

    // Packed scene word shared between board tasks: every field survives.
    {
        for (SceneKind kind : {SceneKind::Home, SceneKind::Reader, SceneKind::Sleep}) {
            for (Emotion emotion : {Emotion::Neutral, Emotion::Happy, Emotion::Curious,
                                    Emotion::Sleepy, Emotion::Surprised}) {
                for (uint16_t page : {uint16_t{1}, uint16_t{2}, uint16_t{65535}}) {
                    for (uint8_t sel = 0; sel < 5; ++sel) {
                        for (uint8_t pose = 0; pose <= static_cast<uint8_t>(kid_app::ScenePose::Morning);
                             ++pose) {
                            const SceneModel in{kind, emotion, page, sel,
                                                static_cast<kid_app::ScenePose>(pose)};
                            const SceneModel out = kid_app::UnpackScene(kid_app::PackScene(in));
                            assert(out.kind == kind && out.emotion == emotion &&
                                   out.book_page == page && out.home_selection == sel &&
                                   out.pose == in.pose);
                        }
                    }
                }
            }
        }
        // Selection is 4 bits: larger values are masked, never spill.
        const SceneModel wide{SceneKind::Reader, Emotion::Happy, 7, 0x1F};
        const SceneModel masked = kid_app::UnpackScene(kid_app::PackScene(wide));
        assert(masked.kind == SceneKind::Reader && masked.home_selection == 0x0F);
        assert(masked.emotion == Emotion::Happy && masked.book_page == 7);
        assert(masked.pose == kid_app::ScenePose::Still);
    }

    std::cout << "PASS: 1-bit frame size, clipping, deterministic scene changes, home rail\n";
    return 0;
}
