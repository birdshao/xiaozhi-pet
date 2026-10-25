// Host-only PBM preview. The ePaper driver is NOT required for this preview.
#include "kid_app/scene_model.h"

#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: pet_scene_preview <happy|sleepy|curious|reader> output.pbm\n";
        return 2;
    }
    const std::string mode = argv[1];
    kid_app::SceneModel model;
    if (mode == "happy") model.emotion = kid_app::Emotion::Happy;
    else if (mode == "sleepy") model.emotion = kid_app::Emotion::Sleepy;
    else if (mode == "curious") model.emotion = kid_app::Emotion::Curious;
    else if (mode == "reader") {
        model.kind = kid_app::SceneKind::Reader;
        model.emotion = kid_app::Emotion::Happy;
        model.book_page = 2;
    } else {
        std::cerr << "Unsupported scene: " << mode << "\n";
        return 2;
    }
    kid_app::MonoFrame frame;
    kid_app::SceneComposer::Render(model, frame);
    std::ofstream out(argv[2], std::ios::binary);
    if (!out) {
        std::cerr << "Cannot open output " << argv[2] << "\n";
        return 1;
    }
    out << "P4\n" << kid_app::MonoFrame::kWidth << " " << kid_app::MonoFrame::kHeight << "\n";
    // Portable Bitmap format P4: 1=black, while our physical ePaper buffer uses 0=black.
    for (auto b : frame.Data()) out.put(static_cast<char>(~b));
    if (!out) return 1;
    std::cout << "Rendered " << mode << " -> " << argv[2] << "\n";
    return 0;
}
