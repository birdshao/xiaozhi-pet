#include "kid_app/scene_model.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>

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
