#include "kid_app/voice_command.h"

#include <cassert>
#include <cstring>
#include <iostream>

using namespace kid_app;

namespace {
VoiceContext Ctx(VoiceScene scene) {
    VoiceContext c;
    c.scene = scene;
    c.clock_known = true;
    c.battery_known = true;
    return c;
}
VoiceDecision Run(const char* name, const VoiceContext& c) {
    return DecideVoiceCommand(ParseVoiceCommand(name), c);
}
bool Is(const VoiceDecision& d, VoiceAction action, VoiceHint hint, const char* result) {
    return d.action == action && d.hint == hint && std::strcmp(d.result, result) == 0;
}
}  // namespace

int main() {
    // Wire names: exact and case-sensitive; everything else is Unknown.
    assert(ParseVoiceCommand("SLEEP_REQUEST") == VoiceCommand::SleepRequest);
    assert(ParseVoiceCommand("TELL_TIME") == VoiceCommand::TellTime);
    assert(ParseVoiceCommand("TELL_BATTERY") == VoiceCommand::TellBattery);
    assert(ParseVoiceCommand("PAGE_NEXT") == VoiceCommand::PageNext);
    assert(ParseVoiceCommand("OPEN_READER") == VoiceCommand::OpenReader);
    for (const char* bad : {"", "sleep_request", "PAGE_PREV", "PET_FEED", "GO_HOME", "open_game", "START_BRUSH ",
                            "SLEEP_REQUEST ", "rm -rf", "TELL_TIME\n"})
        assert(ParseVoiceCommand(bad) == VoiceCommand::Unknown);
    assert(ParseVoiceCommand(nullptr) == VoiceCommand::Unknown);
    for (VoiceCommand c : {VoiceCommand::SleepRequest, VoiceCommand::TellTime,
                           VoiceCommand::TellBattery, VoiceCommand::PageNext,
                           VoiceCommand::OpenReader})
        assert(ParseVoiceCommand(VoiceCommandName(c)) == c);

    // Unknown: ignored in every scene, no action, no hint.
    for (VoiceScene s : {VoiceScene::Off, VoiceScene::Home, VoiceScene::Reader, VoiceScene::Game,
                         VoiceScene::Parent, VoiceScene::Sleeping})
        assert(Is(Run("DROP_TABLE", Ctx(s)), VoiceAction::None, VoiceHint::None, "unknown"));

    // Pet mode off / parent menu open: nothing happens.
    for (const char* n : {"SLEEP_REQUEST", "TELL_TIME", "TELL_BATTERY", "PAGE_NEXT", "OPEN_READER"}) {
        assert(Is(Run(n, Ctx(VoiceScene::Off)), VoiceAction::None, VoiceHint::None, "off"));
        assert(Is(Run(n, Ctx(VoiceScene::Parent)), VoiceAction::None, VoiceHint::None, "parent"));
    }

    // Sleep: from home, reader and game; also in quiet hours / on a low battery.
    for (VoiceScene s : {VoiceScene::Home, VoiceScene::Reader, VoiceScene::Game}) {
        auto c = Ctx(s);
        assert(Is(Run("SLEEP_REQUEST", c), VoiceAction::BeginSleep, VoiceHint::None, "done"));
        c.quiet_hours = true;
        c.low_battery = true;
        assert(Run("SLEEP_REQUEST", c).action == VoiceAction::BeginSleep);
    }
    // Already going to sleep (flow or nap): no second flow, nothing else either.
    assert(Is(Run("SLEEP_REQUEST", Ctx(VoiceScene::Sleeping)), VoiceAction::None, VoiceHint::None,
              "already"));
    for (const char* n : {"TELL_TIME", "TELL_BATTERY", "PAGE_NEXT", "OPEN_READER"})
        assert(Is(Run(n, Ctx(VoiceScene::Sleeping)), VoiceAction::None, VoiceHint::None, "ignored"));

    // Time / battery: answered at home, never over a book or a game.
    {
        auto c = Ctx(VoiceScene::Home);
        auto d = Run("TELL_TIME", c);
        assert(Is(d, VoiceAction::ShowTime, VoiceHint::None, "done") && !d.silent);
        d = Run("TELL_BATTERY", c);
        assert(Is(d, VoiceAction::ShowBattery, VoiceHint::None, "done") && !d.silent);
        c.low_battery = true;  // asked because it is low: still answers
        assert(Run("TELL_BATTERY", c).action == VoiceAction::ShowBattery);
        c = Ctx(VoiceScene::Home);
        c.quiet_hours = true;  // visual only at night
        assert(Run("TELL_TIME", c).silent && Run("TELL_BATTERY", c).silent);
        assert(Run("TELL_TIME", c).action == VoiceAction::ShowTime);

        c = Ctx(VoiceScene::Home);
        c.clock_known = false;
        assert(Is(Run("TELL_TIME", c), VoiceAction::None, VoiceHint::ClockUnknown, "no_clock"));
        assert(Run("TELL_BATTERY", c).action == VoiceAction::ShowBattery);  // independent
        c = Ctx(VoiceScene::Home);
        c.battery_known = false;
        assert(Is(Run("TELL_BATTERY", c), VoiceAction::None, VoiceHint::BatteryUnknown,
                  "no_battery"));
        assert(Run("TELL_TIME", c).action == VoiceAction::ShowTime);

        for (VoiceScene s : {VoiceScene::Reader, VoiceScene::Game}) {
            assert(Is(Run("TELL_TIME", Ctx(s)), VoiceAction::None, VoiceHint::Busy, "busy"));
            assert(Is(Run("TELL_BATTERY", Ctx(s)), VoiceAction::None, VoiceHint::Busy, "busy"));
        }
    }

    // Next page: only in the reader, whatever the hour or the battery.
    {
        assert(Is(Run("PAGE_NEXT", Ctx(VoiceScene::Reader)), VoiceAction::PageNext,
                  VoiceHint::None, "done"));
        for (VoiceScene s : {VoiceScene::Home, VoiceScene::Game})
            assert(Is(Run("PAGE_NEXT", Ctx(s)), VoiceAction::None, VoiceHint::NotInReader,
                      "not_in_reader"));
        auto c = Ctx(VoiceScene::Reader);
        c.quiet_hours = true;
        c.low_battery = true;
        assert(Run("PAGE_NEXT", c).action == VoiceAction::PageNext);
    }

    // Tell a story: from home; not at night; not over a game; no-op in the reader.
    {
        auto c = Ctx(VoiceScene::Home);
        assert(Is(Run("OPEN_READER", c), VoiceAction::OpenReader, VoiceHint::None, "done"));
        c.low_battery = true;  // reading is quiet, unlike the rail's Game item
        assert(Run("OPEN_READER", c).action == VoiceAction::OpenReader);
        c.quiet_hours = true;
        assert(Is(Run("OPEN_READER", c), VoiceAction::None, VoiceHint::QuietHours, "quiet"));
        assert(Is(Run("OPEN_READER", Ctx(VoiceScene::Game)), VoiceAction::None, VoiceHint::Busy,
                  "busy"));
        assert(Is(Run("OPEN_READER", Ctx(VoiceScene::Reader)), VoiceAction::None, VoiceHint::None,
                  "already"));
    }

    // V8-03: the new fixed commands. Wire names round-trip.
    for (const char* n : {"ASK_NEEDS", "OPEN_GAME", "PLAY_SONG", "TELL_JOKE",
                          "START_BRUSH", "START_WASH"})
        assert(std::strcmp(VoiceCommandName(ParseVoiceCommand(n)), n) == 0);

    // Everything below only proposes: VoiceAction stays None, and nothing writes memory.
    {
        auto home = Ctx(VoiceScene::Home);
        auto Proposal = [&](const char* n, const VoiceContext& c) { return Run(n, c).proposal; };
        auto NoAction = [&](const char* n, const VoiceContext& c) {
            return Run(n, c).action == VoiceAction::None;
        };
        // Off / parent menu / sleeping: nothing happens.
        for (const char* n : {"ASK_NEEDS", "OPEN_GAME", "PLAY_SONG", "TELL_JOKE", "START_BRUSH",
                              "START_WASH"}) {
            assert(Is(Run(n, Ctx(VoiceScene::Off)), VoiceAction::None, VoiceHint::None, "off"));
            assert(Is(Run(n, Ctx(VoiceScene::Parent)), VoiceAction::None, VoiceHint::None, "parent"));
            assert(Run(n, Ctx(VoiceScene::Sleeping)).proposal == VoiceProposal::None);
            assert(Run(n, Ctx(VoiceScene::Sleeping)).action == VoiceAction::None);
        }
        assert(std::strcmp(Run("SLEEP_REQUEST", Ctx(VoiceScene::Sleeping)).result, "already") == 0);

        // Good night ("晚安" = SLEEP_REQUEST on the server) is the one sleep flow, also at night and on a low battery.
        for (VoiceScene s : {VoiceScene::Home, VoiceScene::Reader, VoiceScene::Game}) {
            auto c = Ctx(s);
            c.quiet_hours = c.low_battery = true;
            assert(Is(Run("SLEEP_REQUEST", c), VoiceAction::BeginSleep, VoiceHint::None, "done"));
        }

        // Are you hungry? At home; generic until the care state is readable.
        auto d = Run("ASK_NEEDS", home);
        assert(d.proposal == VoiceProposal::AskNeeds && d.generic && !d.silent && NoAction("ASK_NEEDS", home));
        home.needs_known = true;
        assert(!Run("ASK_NEEDS", home).generic);
        home.needs_known = false;
        auto night = home;
        night.quiet_hours = true;
        assert(Run("ASK_NEEDS", night).silent && Run("TELL_JOKE", night).silent);
        for (VoiceScene s : {VoiceScene::Reader, VoiceScene::Game}) {
            assert(Is(Run("ASK_NEEDS", Ctx(s)), VoiceAction::None, VoiceHint::Busy, "busy"));
            assert(Is(Run("TELL_JOKE", Ctx(s)), VoiceAction::None, VoiceHint::Busy, "busy"));
        }
        assert(Proposal("TELL_JOKE", home) == VoiceProposal::TellJoke);

        // Game menu / songs: home only, not at night, not on a low battery.
        for (const char* n : {"OPEN_GAME", "PLAY_SONG"}) {
            const VoiceProposal want =
                std::strcmp(n, "OPEN_GAME") == 0 ? VoiceProposal::OpenGame : VoiceProposal::PlaySong;
            assert(Is(Run(n, home), VoiceAction::None, VoiceHint::None, "done"));
            assert(Proposal(n, home) == want);
            assert(Is(Run(n, night), VoiceAction::None, VoiceHint::QuietHours, "quiet"));
            assert(Run(n, night).proposal == VoiceProposal::None);
            auto low = home;
            low.low_battery = true;
            assert(Is(Run(n, low), VoiceAction::None, VoiceHint::LowBattery, "low_battery"));
            assert(Is(Run(n, Ctx(VoiceScene::Reader)), VoiceAction::None, VoiceHint::Busy, "busy"));
        }
        assert(Is(Run("OPEN_GAME", Ctx(VoiceScene::Game)), VoiceAction::None, VoiceHint::None,
                  "already"));
        assert(Is(Run("PLAY_SONG", Ctx(VoiceScene::Game)), VoiceAction::None, VoiceHint::Busy,
                  "busy"));

        // Routines: home only; bedtime brushing is fine (quiet = no cue sounds); low battery waits.
        for (const char* n : {"START_BRUSH", "START_WASH"}) {
            const VoiceProposal want = std::strcmp(n, "START_BRUSH") == 0 ? VoiceProposal::StartBrush
                                                                            : VoiceProposal::StartWash;
            assert(Proposal(n, home) == want && !Run(n, home).silent);
            assert(Proposal(n, night) == want && Run(n, night).silent);
            auto low = home;
            low.low_battery = true;
            assert(Is(Run(n, low), VoiceAction::None, VoiceHint::LowBattery, "low_battery"));
            for (VoiceScene s : {VoiceScene::Reader, VoiceScene::Game})
                assert(Is(Run(n, Ctx(s)), VoiceAction::None, VoiceHint::Busy, "busy"));
        }
    }

    // Names are never empty (they go into the log).
    for (VoiceAction a : {VoiceAction::None, VoiceAction::BeginSleep, VoiceAction::ShowTime,
                          VoiceAction::ShowBattery, VoiceAction::PageNext, VoiceAction::OpenReader})
        assert(*VoiceActionName(a));
    for (VoiceHint h : {VoiceHint::None, VoiceHint::NotInReader, VoiceHint::Busy,
                        VoiceHint::QuietHours, VoiceHint::ClockUnknown, VoiceHint::BatteryUnknown,
                        VoiceHint::LowBattery})
        assert(*VoiceHintName(h));
    for (VoiceProposal p : {VoiceProposal::None, VoiceProposal::AskNeeds, VoiceProposal::OpenGame,
                            VoiceProposal::PlaySong, VoiceProposal::TellJoke,
                            VoiceProposal::StartBrush, VoiceProposal::StartWash})
        assert(*VoiceProposalName(p));
    assert(std::strcmp(VoiceCommandName(VoiceCommand::Unknown), "UNKNOWN") == 0);

    std::cout << "voice command tests passed\n";
    return 0;
}
