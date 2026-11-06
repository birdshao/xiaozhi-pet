#pragma once

#include <cstdint>

namespace kid_app {

// V6-02 (device side): fixed voice commands. The words are matched by the
// server (docs/V5_V6_SERVER_PLAN.md §6.2); what reaches the device is only
// one of these wire names, as the argument of the MCP tool
// `self.pet.request(action)` (S5). Anything else is ignored. The pet decides
// here whether and how to act, with the same rules as the buttons; voice
// never writes memory and never adds a reward.
//
//   SLEEP_REQUEST  the V2-09 sleep flow (3 s cancel window stays)
//   TELL_TIME      show the clock band and acknowledge (the server's TTS
//                  says the digits; the device has no number clips)
//   TELL_BATTERY   same for the battery level and charging state
//   PAGE_NEXT      next page, only inside the reader
//   OPEN_READER    open the picture-book reader (no book: the #75 hint)
//
// The server's other planned names (PAGE_PREV, OPEN_GAME, PET_FEED,
// PET_GROOM, GO_HOME) are not built yet: they are Unknown here.
enum class VoiceCommand : uint8_t {
    Unknown, SleepRequest, TellTime, TellBattery, PageNext, OpenReader
};

// Exact, case-sensitive wire name; nullptr or any other text is Unknown.
VoiceCommand ParseVoiceCommand(const char* name);
const char* VoiceCommandName(VoiceCommand command);

// Where the pet is, as the board sees it right now.
enum class VoiceScene : uint8_t {
    Off,       // Pet mode is not running
    Home,      // awake on the home screen (also the good-morning picture)
    Reader,    // a book is open (or its "no book" picture)
    Game,      // a mini game
    Parent,    // the parent menu has every input
    Sleeping,  // sleep flow running, or napping
};

struct VoiceContext {
    VoiceScene scene = VoiceScene::Off;
    bool quiet_hours = false;
    bool low_battery = false;    // BatteryPolicy::LowPower()
    bool clock_known = false;    // RTC / SNTP gave a plausible time
    bool battery_known = false;  // a PMIC reading exists
};

enum class VoiceAction : uint8_t {
    None, BeginSleep, ShowTime, ShowBattery, PageNext, OpenReader
};
// The gentle reply for a command the pet cannot do now: it says hi instead
// (like the rail's refused Game item); never an error.
enum class VoiceHint : uint8_t {
    None, NotInReader, Busy, QuietHours, ClockUnknown, BatteryUnknown
};

struct VoiceDecision {
    VoiceAction action = VoiceAction::None;
    VoiceHint hint = VoiceHint::None;
    // Quiet hours: show only, no sound (time / battery answers).
    bool silent = false;
    // Log token for PET_VOICE_CMD result=...: done, unknown, off, parent,
    // already, ignored, busy, not_in_reader, quiet, no_clock, no_battery.
    const char* result = "ignored";
};

const char* VoiceActionName(VoiceAction action);
const char* VoiceHintName(VoiceHint hint);

// Pure: no state, no clock, no IO.
VoiceDecision DecideVoiceCommand(VoiceCommand command, const VoiceContext& context);

}  // namespace kid_app
