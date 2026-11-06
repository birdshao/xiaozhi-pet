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
    {"OPEN_READER", VoiceCommand::OpenReader}};

VoiceDecision Done(VoiceAction action) {
    VoiceDecision d;
    d.action = action;
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
    }
    return "none";
}

VoiceDecision DecideVoiceCommand(VoiceCommand command, const VoiceContext& c) {
    if (command == VoiceCommand::Unknown) return Ignored("unknown");
    // With Pet mode off there is no pet; the parent menu takes every input.
    if (c.scene == VoiceScene::Off) return Ignored("off");
    if (c.scene == VoiceScene::Parent) return Ignored("parent");
    if (c.scene == VoiceScene::Sleeping)
        return Ignored(command == VoiceCommand::SleepRequest ? "already" : "ignored");

    const bool at_home = c.scene == VoiceScene::Home;
    switch (command) {
        case VoiceCommand::SleepRequest:
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
        case VoiceCommand::Unknown:
            break;
    }
    return Ignored("ignored");
}

}  // namespace kid_app
