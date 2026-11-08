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
    for (const char* bad : {"", "sleep_request", "page_prev", "PET_FEED ", "GO_HOME\n", "open_game",
                            "START_BRUSH ", "SLEEP_REQUEST ", "rm -rf", "TELL_TIME\n", "PET_GROOMX"})
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
        // Songs: at night the player still opens (lullabies only, quietly).
        assert(Proposal("PLAY_SONG", night) == VoiceProposal::PlaySong && Run("PLAY_SONG", night).silent);
        for (const char* n : {"OPEN_GAME", "PLAY_SONG"}) {
            const VoiceProposal want =
                std::strcmp(n, "OPEN_GAME") == 0 ? VoiceProposal::OpenGame : VoiceProposal::PlaySong;
            assert(Is(Run(n, home), VoiceAction::None, VoiceHint::None, "done"));
            assert(Proposal(n, home) == want);
            if (std::strcmp(n, "OPEN_GAME") == 0) {
                assert(Is(Run(n, night), VoiceAction::None, VoiceHint::QuietHours, "quiet"));
                assert(Run(n, night).proposal == VoiceProposal::None);
            }
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

    // V6-02 rest: the server's PAGE_PREV, PET_FEED, PET_GROOM and GO_HOME.
    for (const char* n : {"PAGE_PREV", "PET_FEED", "PET_GROOM", "GO_HOME"})
        assert(std::strcmp(VoiceCommandName(ParseVoiceCommand(n)), n) == 0);
    {
        auto home = Ctx(VoiceScene::Home);
        auto night = home;
        night.quiet_hours = true;
        auto low = home;
        low.low_battery = true;
        for (const char* n : {"PAGE_PREV", "PET_FEED", "PET_GROOM", "GO_HOME"}) {
            assert(Is(Run(n, Ctx(VoiceScene::Off)), VoiceAction::None, VoiceHint::None, "off"));
            assert(Is(Run(n, Ctx(VoiceScene::Parent)), VoiceAction::None, VoiceHint::None, "parent"));
            assert(Is(Run(n, Ctx(VoiceScene::Sleeping)), VoiceAction::None, VoiceHint::None,
                      "ignored"));
            assert(Run(n, Ctx(VoiceScene::Sleeping)).proposal == VoiceProposal::None);
        }

        // Previous page: the dial's "up" in the reader, whatever the hour or battery.
        assert(Is(Run("PAGE_PREV", Ctx(VoiceScene::Reader)), VoiceAction::PagePrev,
                  VoiceHint::None, "done"));
        auto reader_night = Ctx(VoiceScene::Reader);
        reader_night.quiet_hours = reader_night.low_battery = true;
        assert(Run("PAGE_PREV", reader_night).action == VoiceAction::PagePrev);
        for (VoiceScene s : {VoiceScene::Home, VoiceScene::Game})
            assert(Is(Run("PAGE_PREV", Ctx(s)), VoiceAction::None, VoiceHint::NotInReader,
                      "not_in_reader"));

        // Home: the dial's long press from the reader or a game; at home already.
        for (VoiceScene s : {VoiceScene::Reader, VoiceScene::Game}) {
            assert(Is(Run("GO_HOME", Ctx(s)), VoiceAction::GoHome, VoiceHint::None, "done"));
            auto c = Ctx(s);
            c.quiet_hours = c.low_battery = c.chat_active = true;  // going home is always fine
            assert(Is(Run("GO_HOME", c), VoiceAction::GoHome, VoiceHint::None, "done"));
        }
        assert(Is(Run("GO_HOME", home), VoiceAction::None, VoiceHint::None, "already"));

        // Feed / groom: they only open the care session the menu card opens (the
        // food picker / the brushing); the child's dial does the rest, so voice
        // itself never posts a Feed and never writes care or memory. Refused like
        // the care menu: home only, not at night, not on a low battery.
        const struct {
            const char* name;
            VoiceProposal want;
        } care[] = {{"PET_FEED", VoiceProposal::CareFeed}, {"PET_GROOM", VoiceProposal::Groom}};
        for (const auto& row : care) {
            auto d = Run(row.name, home);
            assert(Is(d, VoiceAction::None, VoiceHint::None, "done") && d.proposal == row.want);
            assert(!d.after_chat);
            assert(Is(Run(row.name, night), VoiceAction::None, VoiceHint::QuietHours, "quiet"));
            assert(Run(row.name, night).proposal == VoiceProposal::None);
            assert(Is(Run(row.name, low), VoiceAction::None, VoiceHint::LowBattery, "low_battery"));
            assert(Run(row.name, low).proposal == VoiceProposal::None);
            for (VoiceScene s : {VoiceScene::Reader, VoiceScene::Game})
                assert(Is(Run(row.name, Ctx(s)), VoiceAction::None, VoiceHint::Busy, "busy"));
        }

        // A screen activity asked for while XiaoZhi is still talking waits for the
        // chat to end (the board ends a game the moment XiaoZhi listens): result
        // "deferred", the same proposal, after_chat set. The board decides again
        // once XiaoZhi is idle, with every rule above.
        auto chat = home;
        chat.chat_active = true;
        for (const char* n : {"OPEN_GAME", "PLAY_SONG", "START_BRUSH", "START_WASH", "PET_FEED",
                              "PET_GROOM"}) {
            const auto now = Run(n, home);
            const auto later = Run(n, chat);
            assert(later.after_chat && std::strcmp(later.result, "deferred") == 0);
            assert(later.proposal == now.proposal && later.action == VoiceAction::None);
            auto chat_low = chat;
            chat_low.low_battery = true;  // a refusal is said now, not deferred
            assert(!Run(n, chat_low).after_chat);
            assert(Run(n, chat_low).proposal == VoiceProposal::None);
        }
        // Everything else runs at once, also mid-chat.
        auto chat_reader = Ctx(VoiceScene::Reader);
        chat_reader.chat_active = true;
        assert(!Run("PAGE_NEXT", chat_reader).after_chat);
        assert(!Run("PAGE_PREV", chat_reader).after_chat);
        for (const char* n : {"SLEEP_REQUEST", "TELL_TIME", "ASK_NEEDS", "TELL_JOKE", "OPEN_READER"})
            assert(!Run(n, chat).after_chat && std::strcmp(Run(n, chat).result, "deferred") != 0);
    }

    // The MCP tool self.pet.request accepts exactly the server's fixed-command
    // names (services/pet_learning/intent.py ACTIONS); the device-only answers
    // and anything else are refused before they reach the pet.
    {
        const char* allowed[] = {"SLEEP_REQUEST", "PAGE_NEXT", "PAGE_PREV", "OPEN_READER",
                                 "OPEN_GAME",     "PET_FEED",  "PET_GROOM", "GO_HOME",
                                 "ASK_NEEDS",     "PLAY_SONG", "TELL_JOKE", "START_BRUSH",
                                 "START_WASH"};
        for (const char* n : allowed) {
            assert(IsVoiceRequestAction(n));
            assert(ParseVoiceCommand(n) != VoiceCommand::Unknown);
        }
        assert(sizeof(allowed) / sizeof(allowed[0]) == kVoiceRequestActionCount);
        for (const char* bad : {"TELL_TIME", "TELL_BATTERY", "", "pet_feed", "PET_FEED ",
                                "UNKNOWN", "OPEN_GAME\n", "SLEEP_REQUEST;", "FLY_TO_MOON"})
            assert(!IsVoiceRequestAction(bad));
        assert(!IsVoiceRequestAction(nullptr));
    }

    // Result tokens travel back to the MCP caller as small codes; every token
    // the dispatcher or the board can produce round-trips, anything else is 0.
    for (const char* token : {"done", "deferred", "unknown", "off", "parent", "already", "ignored",
                              "busy", "not_in_reader", "quiet", "no_clock", "no_battery",
                              "low_battery", "rejected", "no_book", "expired"}) {
        const uint8_t code = VoiceResultCode(token);
        assert(code != 0);
        assert(std::strcmp(VoiceResultName(code), token) == 0);
    }
    assert(VoiceResultCode("whatever") == 0 && VoiceResultCode(nullptr) == 0);
    assert(std::strcmp(VoiceResultName(0), "pending") == 0);
    assert(std::strcmp(VoiceResultName(250), "pending") == 0);

    // The board's deferral slot (pure, time passed in): a screen activity asked
    // for mid-chat waits for XiaoZhi to go idle, at most kHoldMs.
    {
        auto home = Ctx(VoiceScene::Home);
        auto chat = home;
        chat.chat_active = true;
        const auto deferred = Run("OPEN_GAME", chat);
        VoiceCommand expired = VoiceCommand::Unknown;

        // Nothing waiting: nothing to run, nothing expires.
        VoiceDeferral slot;
        assert(slot.Pending() == VoiceCommand::Unknown);
        assert(slot.Poll(false, 1000, &expired) == VoiceCommand::Unknown);
        assert(expired == VoiceCommand::Unknown);

        // Deferred at t=1000: held while XiaoZhi talks, handed back once idle (once).
        slot.OnDecision(VoiceCommand::OpenGame, deferred, 1000);
        assert(slot.Pending() == VoiceCommand::OpenGame);
        assert(slot.Poll(true, 2000, &expired) == VoiceCommand::Unknown);
        assert(slot.Poll(true, 1000 + VoiceDeferral::kHoldMs, &expired) == VoiceCommand::Unknown);
        assert(expired == VoiceCommand::Unknown && slot.Pending() == VoiceCommand::OpenGame);
        assert(slot.Poll(false, 1000 + VoiceDeferral::kHoldMs, &expired) == VoiceCommand::OpenGame);
        assert(slot.Pending() == VoiceCommand::Unknown);
        assert(slot.Poll(false, 30000, &expired) == VoiceCommand::Unknown);

        // The chat never ends: dropped after kHoldMs, reported once.
        slot.OnDecision(VoiceCommand::PetFeed, Run("PET_FEED", chat), 5000);
        assert(slot.Poll(true, 5001 + VoiceDeferral::kHoldMs, &expired) == VoiceCommand::Unknown);
        assert(expired == VoiceCommand::PetFeed && slot.Pending() == VoiceCommand::Unknown);
        expired = VoiceCommand::Unknown;
        assert(slot.Poll(false, 5002 + VoiceDeferral::kHoldMs, &expired) == VoiceCommand::Unknown);
        assert(expired == VoiceCommand::Unknown);

        // The millisecond clock wraps: still measured as elapsed time.
        slot.OnDecision(VoiceCommand::OpenGame, deferred, 0xFFFFFF00u);
        assert(slot.Poll(true, 0x00000100u, &expired) == VoiceCommand::Unknown);
        assert(expired == VoiceCommand::Unknown && slot.Pending() == VoiceCommand::OpenGame);
        assert(slot.Poll(false, 0x00000200u, &expired) == VoiceCommand::OpenGame);

        // A newer deferred command replaces the waiting one.
        slot.OnDecision(VoiceCommand::OpenGame, deferred, 100);
        slot.OnDecision(VoiceCommand::PetGroom, Run("PET_GROOM", chat), 200);
        assert(slot.Pending() == VoiceCommand::PetGroom);
        // A refused or ignored command leaves it alone (the promise still holds) ...
        slot.OnDecision(VoiceCommand::PagePrev, Run("PAGE_PREV", chat), 300);       // not_in_reader
        slot.OnDecision(VoiceCommand::GoHome, Run("GO_HOME", chat), 300);           // already
        slot.OnDecision(VoiceCommand::Unknown, Run("NOPE", chat), 300);             // unknown
        auto quiet_chat = chat;
        quiet_chat.quiet_hours = true;
        slot.OnDecision(VoiceCommand::OpenReader, Run("OPEN_READER", quiet_chat), 300);  // quiet
        assert(slot.Pending() == VoiceCommand::PetGroom);
        // ... an action that ran supersedes it (the child asked for something else).
        slot.OnDecision(VoiceCommand::TellTime, Run("TELL_TIME", chat), 400);
        assert(slot.Pending() == VoiceCommand::Unknown);
        slot.OnDecision(VoiceCommand::OpenGame, deferred, 500);
        slot.OnDecision(VoiceCommand::SleepRequest, Run("SLEEP_REQUEST", chat), 600);
        assert(slot.Pending() == VoiceCommand::Unknown);

        // Decided again once idle with the rules of that moment: e.g. quiet hours
        // began meanwhile -> refused then, nothing opens.
        slot.OnDecision(VoiceCommand::OpenGame, deferred, 700);
        const VoiceCommand again = slot.Poll(false, 800, &expired);
        auto now_quiet = home;
        now_quiet.quiet_hours = true;
        assert(again == VoiceCommand::OpenGame);
        assert(std::strcmp(DecideVoiceCommand(again, now_quiet).result, "quiet") == 0);
        assert(std::strcmp(DecideVoiceCommand(again, home).result, "done") == 0);
    }

    // Names are never empty (they go into the log).
    for (VoiceAction a : {VoiceAction::None, VoiceAction::BeginSleep, VoiceAction::ShowTime,
                          VoiceAction::ShowBattery, VoiceAction::PageNext, VoiceAction::OpenReader,
                          VoiceAction::PagePrev, VoiceAction::GoHome})
        assert(*VoiceActionName(a));
    for (VoiceHint h : {VoiceHint::None, VoiceHint::NotInReader, VoiceHint::Busy,
                        VoiceHint::QuietHours, VoiceHint::ClockUnknown, VoiceHint::BatteryUnknown,
                        VoiceHint::LowBattery})
        assert(*VoiceHintName(h));
    for (VoiceProposal p : {VoiceProposal::None, VoiceProposal::AskNeeds, VoiceProposal::OpenGame,
                            VoiceProposal::PlaySong, VoiceProposal::TellJoke,
                            VoiceProposal::StartBrush, VoiceProposal::StartWash,
                            VoiceProposal::CareFeed, VoiceProposal::Groom})
        assert(*VoiceProposalName(p));
    assert(std::strcmp(VoiceCommandName(VoiceCommand::Unknown), "UNKNOWN") == 0);

    std::cout << "voice command tests passed\n";
    return 0;
}
