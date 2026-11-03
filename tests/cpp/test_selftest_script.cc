#include "kid_app/selftest_script.h"

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
