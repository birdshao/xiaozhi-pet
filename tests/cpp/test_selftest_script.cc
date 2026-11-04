#include "kid_app/selftest_script.h"

#include "kid_app/gesture_detector.h"
#include "kid_app/voice_face.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <string>

using namespace kid_app;

int main() {
    // A complete scenario: header, comments, every op, explicit end.
    {
        const char* text =
            "PETSELFTEST1\n"
            "# V2 home rail\n"
            "wait 15000\n"
            "pet\n"
            "tap down\n"
            "tap up 2500\n"
            "hold press 1500\n"
            "hold boot 1200 3000\n"
            "tap pwr\n"
            "end\n";
        SelfTestScript script;
        assert(script.Parse(text));
        assert(script.Count() == 7);
        const auto& s = script.Steps();
        assert(s[0].op == SelfTestOp::Wait && s[0].ms == 15000);
        assert(s[1].op == SelfTestOp::TogglePet);
        assert(s[2].op == SelfTestOp::Hold && s[2].key == InputKey::DialDown &&
               s[2].ms == SelfTestScript::kTapMs && s[2].gap_ms == SelfTestScript::kDefaultGapMs);
        assert(s[3].key == InputKey::DialUp && s[3].gap_ms == 2500);
        assert(s[4].op == SelfTestOp::Hold && s[4].key == InputKey::DialPress &&
               s[4].ms == 1500 && s[4].gap_ms == SelfTestScript::kDefaultGapMs);
        assert(s[5].key == InputKey::Boot && s[5].ms == 1200 && s[5].gap_ms == 3000);
        assert(s[6].key == InputKey::Power);
        assert(script.TotalMs() == 15000 + 0 + (80 + 1000) + (80 + 2500) + (1500 + 1000) +
                                       (1200 + 3000) + (80 + 1000));
    }

    // mark <name>: a log marker the host report counts from.
    {
        SelfTestScript script;
        assert(script.Parse("PETSELFTEST1\nmark begin\nend\n"));
        assert(script.Count() == 1 && script.Steps()[0].op == SelfTestOp::Mark);
        assert(std::strcmp(script.Steps()[0].name, "begin") == 0);
        assert(script.TotalMs() == 0);
        assert(!script.Parse("PETSELFTEST1\nmark\nend\n"));                     // no name
        assert(!script.Parse("PETSELFTEST1\nmark a b\nend\n"));                 // two names
        assert(!script.Parse("PETSELFTEST1\nmark name_is_far_too_long\nend\n")); // > 15
        assert(!script.Parse("PETSELFTEST1\nmark be-gin\nend\n"));              // [a-z0-9_]
    }

    // alarm <seconds>: RTC wake-up for the next sleep, so an unattended
    // trial can power off and come back without a person pressing PWR.
    {
        SelfTestScript script;
        assert(script.Parse("PETSELFTEST1\nalarm 45\nend\n"));
        assert(script.Count() == 1 && script.Steps()[0].op == SelfTestOp::Alarm);
        assert(script.Steps()[0].ms == 45);  // seconds for this op
        assert(script.TotalMs() == 0);
        assert(!script.Parse("PETSELFTEST1\nalarm 9\nend\n"));    // < 10 s
        assert(!script.Parse("PETSELFTEST1\nalarm 601\nend\n"));  // > 10 min
        assert(!script.Parse("PETSELFTEST1\nalarm\nend\n"));
    }

    // morning: the RTC-alarm wake path without a power cycle. It ends in a
    // power-off, so it is only accepted after an alarm step.
    {
        SelfTestScript script;
        assert(script.Parse("PETSELFTEST1\nalarm 60\nmorning\nwait 90000\nend\n"));
        assert(script.Count() == 3 && script.Steps()[1].op == SelfTestOp::Morning);
        assert(script.TotalMs() == 90000);  // not a delay itself
        assert(!script.Parse("PETSELFTEST1\nmorning\nend\n"));               // no alarm
        assert(!script.Parse("PETSELFTEST1\nmorning\nalarm 60\nend\n"));     // alarm too late
        assert(!script.Parse("PETSELFTEST1\nalarm 60\nmorning now\nend\n")); // extra field
        assert(script.Count() == 0);
    }

    // volume <percent>: quiet prompts during unattended trials (RAM only).
    {
        SelfTestScript script;
        assert(script.Parse("PETSELFTEST1\nvolume 15\nwait 100\nend\n"));
        assert(script.Count() == 2 && script.Steps()[0].op == SelfTestOp::Volume);
        assert(script.Steps()[0].ms == 15);  // percent for this op
        assert(script.TotalMs() == 100);     // a setting, not a delay
        assert(script.Parse("PETSELFTEST1\nvolume 0\nend\n"));
        assert(!script.Parse("PETSELFTEST1\nvolume 101\nend\n"));
        assert(!script.Parse("PETSELFTEST1\nvolume\nend\n"));
        assert(!script.Parse("PETSELFTEST1\nvolume -5\nend\n"));
    }

    // gesture <name>: inject an IMU gesture to test the board wiring.
    {
        SelfTestScript script;
        assert(script.Parse("PETSELFTEST1\ngesture pickup\ngesture gentle\ngesture hard\n"
                            "gesture facedown\ngesture faceup\nwait 10\nend\n"));
        assert(script.Count() == 6 && script.TotalMs() == 10);
        const kid_app::Gesture want[] = {kid_app::Gesture::Pickup, kid_app::Gesture::GentleShake,
                                         kid_app::Gesture::HardShake, kid_app::Gesture::FaceDown,
                                         kid_app::Gesture::FaceUp};
        for (int i = 0; i < 5; ++i) {
            assert(script.Steps()[i].op == SelfTestOp::Gesture);
            assert(script.Steps()[i].ms == static_cast<uint32_t>(want[i]));
        }
        assert(!script.Parse("PETSELFTEST1\ngesture\nend\n"));
        assert(!script.Parse("PETSELFTEST1\ngesture spin\nend\n"));
        assert(!script.Parse("PETSELFTEST1\ngesture pickup now\nend\n"));
    }

    // voice <phase>: inject XiaoZhi voice states for the V6-01 faces.
    {
        SelfTestScript script;
        assert(script.Parse("PETSELFTEST1\nvoice listen\nvoice think\nvoice speak\n"
                            "voice off\nwait 10\nend\n"));
        assert(script.Count() == 5 && script.TotalMs() == 10);  // phases are not delays
        const kid_app::VoiceFace want[] = {kid_app::VoiceFace::Listening,
                                           kid_app::VoiceFace::Thinking,
                                           kid_app::VoiceFace::Speaking, kid_app::VoiceFace::None};
        for (int i = 0; i < 4; ++i) {
            assert(script.Steps()[i].op == SelfTestOp::Voice);
            assert(script.Steps()[i].ms == static_cast<uint32_t>(want[i]));
        }
        assert(!script.Parse("PETSELFTEST1\nvoice\nend\n"));
        assert(!script.Parse("PETSELFTEST1\nvoice sing\nend\n"));
        assert(!script.Parse("PETSELFTEST1\nvoice listen now\nend\n"));
    }

    // combo <ms> [gap]: BOOT + dial press together (V3-06 parent entry).
    {
        SelfTestScript script;
        assert(script.Parse("PETSELFTEST1\ncombo 5500\ncombo 6000 7000\nend\n"));
        assert(script.Count() == 2);
        assert(script.Steps()[0].op == SelfTestOp::Combo && script.Steps()[0].ms == 5500 &&
               script.Steps()[0].gap_ms == SelfTestScript::kDefaultGapMs);
        assert(script.Steps()[1].ms == 6000 && script.Steps()[1].gap_ms == 7000);
        assert(script.TotalMs() == 5500 + 1000 + 6000 + 7000);
        assert(!script.Parse("PETSELFTEST1\ncombo\nend\n"));
        assert(!script.Parse("PETSELFTEST1\ncombo 0\nend\n"));
        assert(!script.Parse("PETSELFTEST1\ncombo boot 5000\nend\n"));
        assert(!script.Parse("PETSELFTEST1\ncombo 5000 10 20\nend\n"));
        assert(!script.Parse("PETSELFTEST1\ncombo 600001\nend\n"));
    }

    // Raw flash sector: text followed by erased 0xFF bytes.
    {
        char sector[4096];
        std::memset(sector, 0xFF, sizeof(sector));
        const char* text = "PETSELFTEST1\r\nwait 10\r\nend\r\n";
        std::memcpy(sector, text, std::strlen(text));
        SelfTestScript script;
        assert(script.ParseSector(reinterpret_cast<const uint8_t*>(sector), sizeof(sector)));
        assert(script.Count() == 1 && script.Steps()[0].ms == 10);
    }

    // Anything not exactly valid is ignored: the firmware then does nothing.
    {
        SelfTestScript script;
        char erased[64];
        std::memset(erased, 0xFF, sizeof(erased));
        assert(!script.ParseSector(reinterpret_cast<const uint8_t*>(erased), sizeof(erased)));
        assert(script.Count() == 0);
        assert(!script.Parse(""));
        assert(!script.Parse("PETSELFTEST2\nend\n"));            // wrong version
        assert(!script.Parse("wait 10\nend\n"));                  // no header
        assert(!script.Parse("PETSELFTEST1\nwait 10\n"));         // truncated: no end
        assert(!script.Parse("PETSELFTEST1\nspin 3\nend\n"));     // unknown op
        assert(!script.Parse("PETSELFTEST1\ntap left\nend\n"));   // unknown key
        assert(!script.Parse("PETSELFTEST1\nwait abc\nend\n"));   // bad number
        assert(!script.Parse("PETSELFTEST1\nwait 600001\nend\n"));  // > 10 min
        assert(!script.Parse("PETSELFTEST1\nhold press\nend\n"));   // missing ms
        assert(!script.Parse("PETSELFTEST1\nhold press 0\nend\n")); // zero hold
        assert(!script.Parse("PETSELFTEST1\npet 5\nend\n"));      // extra field
        assert(script.Count() == 0);  // a failed parse never leaves steps
    }

    // battery <pct> <0|1> / battery off (V3-05): PMIC readings for the
    // status band and the battery policy. Critical on battery powers off,
    // so it needs an earlier alarm step, like "morning".
    {
        SelfTestScript script;
        assert(script.Parse("PETSELFTEST1\nbattery 60 1\nbattery 15 0\nbattery 100 0\n"
                            "battery off\nwait 10\nend\n"));
        assert(script.Count() == 5 && script.TotalMs() == 10);  // settings, not delays
        for (int i = 0; i < 4; ++i) assert(script.Steps()[i].op == SelfTestOp::Battery);
        assert(script.Steps()[0].ms == (60 | SelfTestScript::kBatteryCharging));
        assert(script.Steps()[1].ms == 15 && script.Steps()[2].ms == 100);
        assert(script.Steps()[3].ms == SelfTestScript::kBatteryOff);
        assert(script.Parse("PETSELFTEST1\nbattery 8 0\nbattery 3 1\nend\n"));  // not critical
        assert(!script.Parse("PETSELFTEST1\nbattery 7 0\nend\n"));               // no alarm
        assert(!script.Parse("PETSELFTEST1\nbattery 5 0\nalarm 60\nend\n"));     // alarm too late
        assert(script.Parse("PETSELFTEST1\nalarm 60\nbattery 5 0\nwait 90000\nend\n"));
        assert(script.Steps()[1].ms == 5 && script.TotalMs() == 90000);
        assert(!script.Parse("PETSELFTEST1\nbattery 101 0\nend\n"));
        assert(!script.Parse("PETSELFTEST1\nbattery 50 2\nend\n"));
        assert(!script.Parse("PETSELFTEST1\nbattery 50\nend\n"));
        assert(!script.Parse("PETSELFTEST1\nbattery on\nend\n"));
        assert(!script.Parse("PETSELFTEST1\nbattery off 1\nend\n"));
        assert(!script.Parse("PETSELFTEST1\nbattery -5 0\nend\n"));
        assert(script.Count() == 0);
    }

    // Bounded: at most kMaxSteps steps.
    {
        std::string text = "PETSELFTEST1\n";
        for (size_t i = 0; i < SelfTestScript::kMaxSteps; ++i) text += "tap down\n";
        SelfTestScript script;
        assert(script.Parse((text + "end\n").c_str()));
        assert(script.Count() == SelfTestScript::kMaxSteps);
        assert(!script.Parse((text + "tap up\nend\n").c_str()));
    }

    std::cout << "PASS: self-test script parser (ops, keys, bounds, sector, rejection)\n";
}
