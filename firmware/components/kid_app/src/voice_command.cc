#include "kid_app/voice_command.h"

#include <cstring>

namespace kid_app {

namespace {
struct Entry {
    const char* name;
    VoiceCommand command;
};
constexpr Entry kCommands[] = {
    {"SLEEP_REQUEST", VoiceCommand::SleepRequest}, {"TELL_TIME", VoiceCommand::TellTime},
    {"TELL_BATTERY", VoiceCommand::TellBattery},   {"PAGE_NEXT", VoiceCommand::PageNext},
    {"OPEN_READER", VoiceCommand::OpenReader},     {"ASK_NEEDS", VoiceCommand::AskNeeds},
    {"OPEN_GAME", VoiceCommand::OpenGame},         {"PLAY_SONG", VoiceCommand::PlaySong},
    {"GOOD_NIGHT", VoiceCommand::GoodNight},       {"TELL_JOKE", VoiceCommand::TellJoke},
    {"START_BRUSH", VoiceCommand::StartBrush},     {"START_WASH", VoiceCommand::StartWash}};

VoiceDecision Done(VoiceAction action) {
    VoiceDecision d;
    d.action = action;
    d.result = "done";
    return d;
}
VoiceDecision Proposed(VoiceProposal proposal) {
    VoiceDecision d;
    d.proposal = proposal;
    d.result = "done";
    return d;
}
VoiceDecision Refused(VoiceHint hint, const char* result) {
    VoiceDecision d;
    d.hint = hint;
    d.result = result;
    return d;
}
VoiceDecision Ignored(const char* result) {
    VoiceDecision d;
    d.result = result;
    return d;
}
}  // namespace

VoiceCommand ParseVoiceCommand(const char* name) {
    if (!name) return VoiceCommand::Unknown;
    for (const auto& entry : kCommands)
        if (std::strcmp(name, entry.name) == 0) return entry.command;
    return VoiceCommand::Unknown;
}

const char* VoiceCommandName(VoiceCommand command) {
    for (const auto& entry : kCommands)
        if (entry.command == command) return entry.name;
    return "UNKNOWN";
}

const char* VoiceActionName(VoiceAction action) {
    switch (action) {
        case VoiceAction::None: return "none";
        case VoiceAction::BeginSleep: return "sleep";
        case VoiceAction::ShowTime: return "time";
        case VoiceAction::ShowBattery: return "battery";
        case VoiceAction::PageNext: return "page_next";
        case VoiceAction::OpenReader: return "open_reader";
    }
    return "none";
}

const char* VoiceHintName(VoiceHint hint) {
    switch (hint) {
        case VoiceHint::None: return "none";
        case VoiceHint::NotInReader: return "not_in_reader";
        case VoiceHint::Busy: return "busy";
        case VoiceHint::QuietHours: return "quiet";
        case VoiceHint::ClockUnknown: return "no_clock";
        case VoiceHint::BatteryUnknown: return "no_battery";
        case VoiceHint::LowBattery: return "low_battery";
    }
    return "none";
}

const char* VoiceProposalName(VoiceProposal proposal) {
    switch (proposal) {
        case VoiceProposal::None: return "none";
        case VoiceProposal::AskNeeds: return "ask_needs";
        case VoiceProposal::OpenGame: return "open_game";
        case VoiceProposal::PlaySong: return "play_song";
        case VoiceProposal::TellJoke: return "tell_joke";
        case VoiceProposal::StartBrush: return "start_brush";
        case VoiceProposal::StartWash: return "start_wash";
    }
    return "none";
}

VoiceDecision DecideVoiceCommand(VoiceCommand command, const VoiceContext& c) {
    if (command == VoiceCommand::Unknown) return Ignored("unknown");
    // With Pet mode off there is no pet; the parent menu takes every input.
    if (c.scene == VoiceScene::Off) return Ignored("off");
    if (c.scene == VoiceScene::Parent) return Ignored("parent");
    if (c.scene == VoiceScene::Sleeping)
        return Ignored(command == VoiceCommand::SleepRequest || command == VoiceCommand::GoodNight
                           ? "already"
                           : "ignored");

    const bool at_home = c.scene == VoiceScene::Home;
    switch (command) {
        case VoiceCommand::SleepRequest:
        case VoiceCommand::GoodNight:
            // Bedtime talk is welcome in quiet hours and on a low battery.
            return Done(VoiceAction::BeginSleep);
        case VoiceCommand::TellTime: {
            if (!at_home) return Refused(VoiceHint::Busy, "busy");
            if (!c.clock_known) return Refused(VoiceHint::ClockUnknown, "no_clock");
            VoiceDecision d = Done(VoiceAction::ShowTime);
            d.silent = c.quiet_hours;
            return d;
        }
        case VoiceCommand::TellBattery: {
            if (!at_home) return Refused(VoiceHint::Busy, "busy");
            if (!c.battery_known) return Refused(VoiceHint::BatteryUnknown, "no_battery");
            VoiceDecision d = Done(VoiceAction::ShowBattery);
            d.silent = c.quiet_hours;
            return d;
        }
        case VoiceCommand::PageNext:
            if (c.scene != VoiceScene::Reader)
                return Refused(VoiceHint::NotInReader, "not_in_reader");
            return Done(VoiceAction::PageNext);
        case VoiceCommand::OpenReader:
            if (c.scene == VoiceScene::Reader) return Ignored("already");
            if (c.scene == VoiceScene::Game) return Refused(VoiceHint::Busy, "busy");
            if (c.quiet_hours) return Refused(VoiceHint::QuietHours, "quiet");
            return Done(VoiceAction::OpenReader);
        case VoiceCommand::AskNeeds: {
            if (!at_home) return Refused(VoiceHint::Busy, "busy");
            VoiceDecision d = Proposed(VoiceProposal::AskNeeds);
            d.generic = !c.needs_known;
            d.silent = c.quiet_hours;
            return d;
        }
        case VoiceCommand::TellJoke: {
            if (!at_home) return Refused(VoiceHint::Busy, "busy");
            VoiceDecision d = Proposed(VoiceProposal::TellJoke);
            d.silent = c.quiet_hours;
            return d;
        }
        case VoiceCommand::OpenGame:
            if (c.scene == VoiceScene::Game) return Ignored("already");
            if (!at_home) return Refused(VoiceHint::Busy, "busy");
            if (c.quiet_hours) return Refused(VoiceHint::QuietHours, "quiet");
            if (c.low_battery) return Refused(VoiceHint::LowBattery, "low_battery");
            return Proposed(VoiceProposal::OpenGame);
        case VoiceCommand::PlaySong:
            if (!at_home) return Refused(VoiceHint::Busy, "busy");
            if (c.quiet_hours) return Refused(VoiceHint::QuietHours, "quiet");
            if (c.low_battery) return Refused(VoiceHint::LowBattery, "low_battery");
            return Proposed(VoiceProposal::PlaySong);
        case VoiceCommand::StartBrush:
        case VoiceCommand::StartWash: {
            if (!at_home) return Refused(VoiceHint::Busy, "busy");
            // Routines are display-led: bedtime brushing is normal, so quiet
            // hours only mute the cue sounds. The hourglass refreshes cost
            // power, so a low battery waits for a charge.
            if (c.low_battery) return Refused(VoiceHint::LowBattery, "low_battery");
            VoiceDecision d = Proposed(command == VoiceCommand::StartBrush
                                           ? VoiceProposal::StartBrush
                                           : VoiceProposal::StartWash);
            d.silent = c.quiet_hours;
            return d;
        }
        case VoiceCommand::Unknown:
            break;
    }
    return Ignored("ignored");
}

}  // namespace kid_app
