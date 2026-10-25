#include "kid_app/audio_coordinator.h"

#include <cassert>
#include <iostream>

using namespace kid_app;

int main() {
    AudioCoordinator audio;
    assert(audio.Active() == AudioStream::None);
    auto p = audio.PlayCue(1);
    assert(p.accepted && p.count == 1 && p.actions[0].verb == AudioVerb::Start);
    assert(audio.Active() == AudioStream::PetCue);
    assert(!audio.PlayCue(2).accepted);
    assert(!audio.Finished(99).accepted);

    p = audio.StartStory(10);
    assert(p.accepted && p.count == 2);
    assert(p.actions[0].verb == AudioVerb::Stop && p.actions[0].stream == AudioStream::PetCue);
    assert(p.actions[1].verb == AudioVerb::Start && p.actions[1].stream == AudioStream::Story);
    assert(audio.StartStory(10).accepted && audio.ActiveToken() == 10);

    p = audio.BeginVoice(20);
    assert(p.accepted && p.count == 2);
    assert(p.actions[0].verb == AudioVerb::Stop && p.actions[0].stream == AudioStream::Story);
    assert(p.actions[1].verb == AudioVerb::Start && p.actions[1].stream == AudioStream::Voice);
    assert(!audio.PlayCue(3).accepted);
    assert(!audio.StartStory(11).accepted);
    assert(!audio.EndVoice(19).accepted);
    assert(audio.BeginVoice(20).accepted);
    assert(!audio.BeginVoice(21).accepted);
    assert(!audio.Finished(10).accepted);
    assert(audio.Active() == AudioStream::Voice);

    p = audio.EndVoice(20);
    assert(p.accepted && p.count == 2);
    assert(p.actions[0].verb == AudioVerb::Stop && p.actions[0].stream == AudioStream::Voice);
    assert(p.actions[1].verb == AudioVerb::Resume && p.actions[1].stream == AudioStream::Story);
    assert(p.actions[1].token == 10);
    assert(audio.Active() == AudioStream::Story);
    assert(!audio.EndVoice(20).accepted);
    assert(audio.Finished(10).accepted);
    assert(audio.Active() == AudioStream::None);

    p = audio.BeginVoice(21);
    assert(p.accepted && p.count == 1);
    assert(audio.Active() == AudioStream::Voice);
    p = audio.SetQuiet(true);
    assert(p.accepted && p.count == 1 && p.actions[0].verb == AudioVerb::Stop);
    assert(audio.Active() == AudioStream::None);
    assert(!audio.BeginVoice(22).accepted);
    assert(!audio.StartStory(12).accepted);
    assert(!audio.PlayCue(5).accepted);
    assert(audio.SetQuiet(true).accepted && audio.SetQuiet(true).count == 0);
    assert(audio.SetQuiet(false).accepted);
    assert(audio.Active() == AudioStream::None); // no unexpected replay.
    assert(audio.StartStory(12).accepted);
    assert(audio.Finished(12).accepted);
    assert(!audio.Finished(12).accepted);

    assert(!audio.PlayCue(0).accepted);
    assert(!audio.BeginVoice(0).accepted);
    assert(!audio.StartStory(0).accepted);
    std::cout << "PASS: voice-first priority, story resume, quiet gate and stale completion suppression\n";
}
