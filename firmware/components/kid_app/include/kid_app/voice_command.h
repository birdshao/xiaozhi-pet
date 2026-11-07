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
// V8-03 additions (docs/V8_SOUND_AND_ROUTINES.md). They only PROPOSE an
// activity (VoiceProposal): the board starts it through the same path as the
// home rail / the dial, and the activity decides for itself. Voice never
// writes memory or growth and never rewards.
//
//   ASK_NEEDS      "is TuanTuan hungry?": report a need (generic line until
//                  the care state of V8-01 is wired: needs_known)
//   OPEN_GAME      the game menu (the home rail's Game item)
//   PLAY_SONG      the nursery-rhyme / lullaby player
//   (good night: the server maps "晚安" to SLEEP_REQUEST, one sleep flow only)
//   TELL_JOKE      a reviewed joke; the server's TTS speaks it, the device
//                  only laughs (Giggle face)
//   START_BRUSH    2-minute tooth-brushing companion
//   START_WASH     20-second hand-washing companion
//
// The server's PAGE_PREV, PET_FEED, PET_GROOM and GO_HOME are not built yet:
// they are Unknown here.
enum class VoiceCommand : uint8_t {
    Unknown, SleepRequest, TellTime, TellBattery, PageNext, OpenReader,
    AskNeeds, OpenGame, PlaySong, TellJoke, StartBrush, StartWash
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
    // V8-01 care state is readable (false until that session merges: the
    // ASK_NEEDS answer is then the generic line).
    bool needs_known = false;
};

enum class VoiceAction : uint8_t {
    None, BeginSleep, ShowTime, ShowBattery, PageNext, OpenReader
};
// V8-03 proposals. Kept apart from VoiceAction so the board's existing
// switch over VoiceAction keeps compiling; the board acts on a proposal
// only once the matching activity is wired (until then it is just logged).
enum class VoiceProposal : uint8_t {
    None, AskNeeds, OpenGame, PlaySong, TellJoke, StartBrush, StartWash
};
// The gentle reply for a command the pet cannot do now: it says hi instead
// (like the rail's refused Game item); never an error.
enum class VoiceHint : uint8_t {
    None, NotInReader, Busy, QuietHours, ClockUnknown, BatteryUnknown,
    LowBattery  // V8-03: songs, games and routines wait for a charge
};

struct VoiceDecision {
    VoiceAction action = VoiceAction::None;
    VoiceHint hint = VoiceHint::None;
    VoiceProposal proposal = VoiceProposal::None;
    // ASK_NEEDS only: the care state is not available, say the generic line.
    bool generic = false;
    // Quiet hours: show only, no sound (time / battery answers).
    bool silent = false;
    // Log token for PET_VOICE_CMD result=...: done, unknown, off, parent,
    // already, ignored, busy, not_in_reader, quiet, no_clock, no_battery,
    // low_battery.
    const char* result = "ignored";
};

const char* VoiceActionName(VoiceAction action);
const char* VoiceHintName(VoiceHint hint);
const char* VoiceProposalName(VoiceProposal proposal);

// Pure: no state, no clock, no IO.
VoiceDecision DecideVoiceCommand(VoiceCommand command, const VoiceContext& context);

}  // namespace kid_app
