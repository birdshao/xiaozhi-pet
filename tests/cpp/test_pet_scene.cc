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
    assert(happy.IsBlack(400, 425));  // Footer rule.
    assert(book.IsBlack(38, 30));     // Reader outer frame.

    SceneComposer::Render(SceneModel{SceneKind::Home, Emotion::Happy, 1}, book);
    assert(book.Data() == happy.Data());  // Rendering is deterministic.

    std::cout << "PASS: 1-bit frame size, clipping, deterministic scene changes\n";
    return 0;
}
