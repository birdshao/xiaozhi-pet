// Host-only PBM preview. The ePaper driver is NOT required for this preview.
#include "kid_app/scene_model.h"

#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: pet_scene_preview <happy|sleepy|curious|surprised|listening|thinking|speaking|reader|sleep|sunrise|morning|status|charging|yawn|parent|parent_edit|game|game_right|game_count> output.pbm\n";
        return 2;
    }
    const std::string mode = argv[1];
    kid_app::SceneModel model;
    if (mode == "happy") model.emotion = kid_app::Emotion::Happy;
    else if (mode == "sleepy") model.emotion = kid_app::Emotion::Sleepy;
    else if (mode == "curious") model.emotion = kid_app::Emotion::Curious;
    else if (mode == "surprised") model.emotion = kid_app::Emotion::Surprised;
    else if (mode == "listening") model.emotion = kid_app::Emotion::Listening;
    else if (mode == "thinking") model.emotion = kid_app::Emotion::Thinking;
    else if (mode == "speaking") model.emotion = kid_app::Emotion::Speaking;
    else if (mode == "sleep") model.kind = kid_app::SceneKind::Sleep;
    else if (mode == "sunrise" || mode == "morning") {  // V3-04 good morning
        model.emotion = kid_app::Emotion::Happy;
        model.pose = mode == "sunrise" ? kid_app::ScenePose::Sunrise : kid_app::ScenePose::Morning;
    }
    else if (mode == "charging" || mode == "yawn") {  // V3-05 battery reactions
        model.pose = mode == "charging" ? kid_app::ScenePose::Charging : kid_app::ScenePose::Yawn;
        model.status.battery_bars = mode == "charging" ? 2 : 0;
        model.status.charging = mode == "charging";
    }
    else if (mode == "status") {  // V2-05 status band with every field shown
        model.status.hour = 7;
        model.status.minute = 42;
        model.status.temp_c = 24;
        model.status.battery_bars = 3;
        model.status.charging = true;
        model.status.quiet = true;
        model.status.network = kid_app::NetworkState::Online;
    }
    else if (mode == "parent" || mode == "parent_edit") {  // V3-06 parent menu
        model.kind = kid_app::SceneKind::Parent;
        model.parent.settings.schedule.nap_enabled = true;
        model.parent.settings.volume_cap_pct = 70;
        model.parent.settings.motion_enabled = false;
        model.parent.cursor = mode == "parent" ? 1 : 0;
        model.parent.editing = mode == "parent_edit";
    }
    else if (mode == "game" || mode == "game_right" || mode == "game_count") {  // V4-06
        using kid_app::GameArt;
        auto id = [](GameArt art) { return static_cast<uint16_t>(art); };
        model.kind = kid_app::SceneKind::Game;
        model.game.round = 2;
        model.game.rounds = 3;
        model.game.option_count = 3;
        if (mode == "game_count") {
            model.game.question_art = id(GameArt::Blocks4);
            model.game.options = {id(GameArt::Dots2), id(GameArt::Dots4), id(GameArt::Dots5)};
            model.game.highlight = 0;
        } else {
            model.game.question_art = id(GameArt::Listen);
            model.game.options = {id(GameArt::Cat), id(GameArt::Dog), id(GameArt::Bird)};
            model.game.highlight = 1;
        }
        if (mode == "game_right") model.game.feedback = kid_app::GameFeedback::Right;
    }
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
