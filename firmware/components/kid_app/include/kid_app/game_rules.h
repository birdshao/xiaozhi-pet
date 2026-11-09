#pragma once

#include "kid_app/mini_game.h"

#include <cstdint>

namespace kid_app {

// V9 1E (G4): the spoken rule a V8 play game says when it opens (and once
// more after kRepeatPromptMs of silence). Wording: tools/game_prompts.py,
// single source PlayGamePromptText() in play_content.cc.
//
// Two id spaces, both keyed by the game's growth memory id (PlayGameInfo::id,
// 21..39; 30 is unused):
//  * game clip id (GameOutput::clip, PromptRequest::game):
//    kGameRuleClipBase + game id. Above every picture tone (kGameToneBase +
//    art) and never a tone itself, so the player cannot beep it by mistake.
//  * KPS1 sound pack id: kGameRuleKpsBase + game id, in the free 600..999
//    range (pet lines use 100..399, care lines 1000..1999). Memory 4 and 6
//    say the same words and share one clip (621).
// Without a recording the player falls back to the old prompt tone
// (kGameTonePrompt), so a missing sound pack changes nothing.
constexpr uint16_t kGameRuleKpsBase = 600;
constexpr uint16_t kGameRuleClipBase = 0x4C00;
constexpr uint16_t kGameRuleFirstId = 21;
constexpr uint16_t kGameRuleLastId = 39;
constexpr uint16_t kGameRuleNoId = 30;
constexpr uint16_t kGameRuleMemory4 = 21, kGameRuleMemory6 = 22;

constexpr bool GameHasRule(uint16_t game_id) {
    return game_id >= kGameRuleFirstId && game_id <= kGameRuleLastId &&
           game_id != kGameRuleNoId;
}

// KPS1 clip id of a game's rule; 0 for a game without one.
constexpr uint16_t GameRuleKpsId(uint16_t game_id) {
    return !GameHasRule(game_id)
               ? uint16_t{0}
               : static_cast<uint16_t>(
                     kGameRuleKpsBase + (game_id == kGameRuleMemory6 ? kGameRuleMemory4 : game_id));
}

// What a session puts in GameOutput::clip; 0 for a game without a rule.
constexpr uint16_t GameRuleClip(uint16_t game_id) {
    return GameHasRule(game_id) ? static_cast<uint16_t>(kGameRuleClipBase + game_id)
                                : uint16_t{0};
}

constexpr bool IsGameRuleClip(uint16_t clip) {
    return clip >= kGameRuleClipBase + kGameRuleFirstId &&
           clip <= kGameRuleClipBase + kGameRuleLastId &&
           GameHasRule(static_cast<uint16_t>(clip - kGameRuleClipBase));
}

// Game id behind a rule clip; 0 if it is not one.
constexpr uint16_t GameRuleGameId(uint16_t clip) {
    return IsGameRuleClip(clip) ? static_cast<uint16_t>(clip - kGameRuleClipBase) : uint16_t{0};
}

// What the prompt player does with a game clip request (pure, host-tested;
// the board only supplies `recorded`: the sound pack really holds the rule).
//  Pack: stream the recording. Tone: no recording, play the old prompt tone
//  (game_clip). None: not a rule, play game_clip as it is.
enum class RuleSource : uint8_t { None, Pack, Tone };
struct RulePlan {
    RuleSource source = RuleSource::None;
    uint16_t game_clip = 0;
};
constexpr RulePlan PlanGameRule(uint16_t clip, bool recorded) {
    return !IsGameRuleClip(clip) ? RulePlan{RuleSource::None, clip}
           : recorded            ? RulePlan{RuleSource::Pack, clip}
                                 : RulePlan{RuleSource::Tone, kGameTonePrompt};
}

}  // namespace kid_app
