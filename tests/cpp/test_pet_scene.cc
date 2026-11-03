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

    // Home action rail: five 160 px slots on y 380..479; the selected slot
    // gets a thick frame. Selection changes only touch the rail band.
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
        // Selected slot 1 (story): frame edges at x 160+6, y 386.
        assert(story.IsBlack(166, 430) && !feed.IsBlack(166, 430));
        assert(feed.IsBlack(486, 430) && !story.IsBlack(486, 430));
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
        assert(!none.IsBlack(166, 430) && !none.IsBlack(486, 430));
        // The pet face stays fully above the rail.
        for (int x = 0; x < MonoFrame::kWidth; ++x)
            assert(!story.IsBlack(x, SceneComposer::kRailTop - 2));
    }

    // Packed scene word shared between board tasks: every field survives.
    {
        for (SceneKind kind : {SceneKind::Home, SceneKind::Reader}) {
            for (Emotion emotion : {Emotion::Neutral, Emotion::Happy, Emotion::Curious,
                                    Emotion::Sleepy, Emotion::Surprised}) {
                for (uint16_t page : {uint16_t{1}, uint16_t{2}, uint16_t{65535}}) {
                    for (uint8_t sel = 0; sel < 5; ++sel) {
                        const SceneModel in{kind, emotion, page, sel};
                        const SceneModel out = kid_app::UnpackScene(kid_app::PackScene(in));
                        assert(out.kind == kind && out.emotion == emotion &&
                               out.book_page == page && out.home_selection == sel);
                    }
                }
            }
        }
        // Selection is 4 bits: larger values are masked, never spill.
        const SceneModel wide{SceneKind::Reader, Emotion::Happy, 7, 0x1F};
        const SceneModel masked = kid_app::UnpackScene(kid_app::PackScene(wide));
        assert(masked.kind == SceneKind::Reader && masked.home_selection == 0x0F);
        assert(masked.emotion == Emotion::Happy && masked.book_page == 7);
    }

    std::cout << "PASS: 1-bit frame size, clipping, deterministic scene changes, home rail\n";
    return 0;
}
