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
    {"TELL_JOKE", VoiceCommand::TellJoke},
    {"START_BRUSH", VoiceCommand::StartBrush},     {"START_WASH", VoiceCommand::StartWash},
    {"PAGE_PREV", VoiceCommand::PagePrev},         {"PET_FEED", VoiceCommand::PetFeed},
    {"PET_GROOM", VoiceCommand::PetGroom},         {"GO_HOME", VoiceCommand::GoHome}};

// self.pet.request whitelist = intent.py ACTIONS (contract-tested).
constexpr const char* kRequestActions[] = {
    "SLEEP_REQUEST", "PAGE_NEXT", "PAGE_PREV", "OPEN_READER", "OPEN_GAME",
    "PET_FEED", "PET_GROOM", "GO_HOME", "ASK_NEEDS", "PLAY_SONG",
    "TELL_JOKE", "START_BRUSH", "START_WASH"};
static_assert(sizeof(kRequestActions) / sizeof(kRequestActions[0]) == kVoiceRequestActionCount,
              "self.pet.request whitelist size");

// Index = code; 0 is "pending" (no answer yet / unknown token).
constexpr const char* kResults[] = {
    "pending", "done", "deferred", "unknown", "off", "parent", "already", "ignored", "busy",
    "not_in_reader", "quiet", "no_clock", "no_battery", "low_battery", "rejected", "no_book",
    "expired"};

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
// A proposal that opens a screen activity: deferred while XiaoZhi talks.
VoiceDecision Screen(VoiceProposal proposal, const VoiceContext& c) {
    VoiceDecision d = Proposed(proposal);
    if (c.chat_active) {
        d.after_chat = true;
        d.result = "deferred";
    }
    return d;
}
}  // namespace

VoiceCommand ParseVoiceCommand(const char* name) {
    if (!name) return VoiceCommand::Unknown;
    for (const auto& entry : kCommands)
        if (std::strcmp(name, entry.name) == 0) return entry.command;
    return VoiceCommand::Unknown;
}

bool IsVoiceRequestAction(const char* name) {
    if (!name) return false;
    for (const char* allowed : kRequestActions)
        if (std::strcmp(name, allowed) == 0) return true;
    return false;
}

uint8_t VoiceResultCode(const char* token) {
    if (!token) return 0;
    for (size_t i = 1; i < sizeof(kResults) / sizeof(kResults[0]); ++i)
        if (std::strcmp(token, kResults[i]) == 0) return static_cast<uint8_t>(i);
    return 0;
}

const char* VoiceResultName(uint8_t code) {
    return code < sizeof(kResults) / sizeof(kResults[0]) ? kResults[code] : kResults[0];
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
        case VoiceAction::PagePrev: return "page_prev";
        case VoiceAction::GoHome: return "go_home";
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
        case VoiceProposal::CareFeed: return "care_feed";
        case VoiceProposal::Groom: return "groom";
    }
    return "none";
}

void VoiceDeferral::OnDecision(VoiceCommand command, const VoiceDecision& decision,
                               uint32_t now_ms) {
    if (decision.after_chat) {
        command_ = command;
        at_ms_ = now_ms;
    } else if (std::strcmp(decision.result, "done") == 0) {
        command_ = VoiceCommand::Unknown;
    }
}

VoiceCommand VoiceDeferral::Poll(bool chat_active, uint32_t now_ms, VoiceCommand* expired) {
    if (command_ == VoiceCommand::Unknown) return VoiceCommand::Unknown;
    const VoiceCommand command = command_;
    if (now_ms - at_ms_ > kHoldMs) {
        command_ = VoiceCommand::Unknown;
        if (expired) *expired = command;
        return VoiceCommand::Unknown;
    }
    if (chat_active) return VoiceCommand::Unknown;
    command_ = VoiceCommand::Unknown;
    return command;
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
            return Screen(VoiceProposal::OpenGame, c);
        case VoiceCommand::PlaySong: {
            if (!at_home) return Refused(VoiceHint::Busy, "busy");
            if (c.low_battery) return Refused(VoiceHint::LowBattery, "low_battery");
            // Quiet hours: the player opens with lullabies only, at low volume.
            VoiceDecision d = Screen(VoiceProposal::PlaySong, c);
            d.silent = c.quiet_hours;
            return d;
        }
        case VoiceCommand::StartBrush:
        case VoiceCommand::StartWash: {
            if (!at_home) return Refused(VoiceHint::Busy, "busy");
            // Routines are display-led: bedtime brushing is normal, so quiet
            // hours only mute the cue sounds. The hourglass refreshes cost
            // power, so a low battery waits for a charge.
            if (c.low_battery) return Refused(VoiceHint::LowBattery, "low_battery");
            VoiceDecision d = Screen(command == VoiceCommand::StartBrush
                                         ? VoiceProposal::StartBrush
                                         : VoiceProposal::StartWash,
                                     c);
            d.silent = c.quiet_hours;
            return d;
        }
        case VoiceCommand::PagePrev:
            if (c.scene != VoiceScene::Reader)
                return Refused(VoiceHint::NotInReader, "not_in_reader");
            return Done(VoiceAction::PagePrev);
        case VoiceCommand::GoHome:
            // The dial's long press; always allowed (it only ever calms things).
            if (at_home) return Ignored("already");
            return Done(VoiceAction::GoHome);
        case VoiceCommand::PetFeed:
        case VoiceCommand::PetGroom:
            // The care menu's rules (V8_CARE_AND_GROWTH §2): home only, not in
            // quiet hours, not on a low battery. Voice opens the session only;
            // the meal / the strokes come from the dial (ADR-012).
            if (!at_home) return Refused(VoiceHint::Busy, "busy");
            if (c.quiet_hours) return Refused(VoiceHint::QuietHours, "quiet");
            if (c.low_battery) return Refused(VoiceHint::LowBattery, "low_battery");
            return Screen(command == VoiceCommand::PetFeed ? VoiceProposal::CareFeed
                                                           : VoiceProposal::Groom,
                          c);
        case VoiceCommand::Unknown:
            break;
    }
    return Ignored("ignored");
}

}  // namespace kid_app
