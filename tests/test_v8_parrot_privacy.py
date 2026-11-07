"""V8 parrot: the recording exists only in RAM and is wiped after playback.

Behaviour (wipe on every exit path, 5 s cap, level cap) is in
tests/cpp/test_parrot_session.cc. These tests fail the build if the parrot
module ever gains a file, flash, network or logging dependency, or if its
sources start to keep a second copy of the samples.
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
KID = ROOT / "firmware/components/kid_app"
PARROT = [KID / "include/kid_app/parrot_session.h", KID / "src/parrot_session.cc"]

FORBIDDEN = [
    r"\bfopen\b", r"\bfwrite\b", r"\bfread\b", r"\bFILE\b", r"\bofstream\b", r"\bfstream\b",
    r"\bstd::filesystem\b", r"\bopen\s*\(", r"\bwrite\s*\(", r"\bsend\s*\(", r"\bsocket\b",
    r"\bnvs_", r"\bNvs", r"\bpartition\b", r"\bhttp", r"\bHttp", r"\bwifi\b", r"\bWiFi\b",
    r"\blwip\b", r"\bmqtt\b", r"\bwebsocket\b", r"\bopus\b", r"\bOpus\b", r"\bupload\b",
    r"\bheap_caps", r"\bmalloc\b", r"\bnew\s+\w", r"\bstd::vector\b", r"\bstd::string\b",
    r"\bprintf\b", r"\bESP_LOG", r"\bstatic\s+int16_t\b", r"\bmemcpy\b",
]
ALLOWED_STD_INCLUDES = {"cstddef", "cstdint"}


def code_only(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return re.sub(r'"(?:\\.|[^"\\])*"', '""', text)


class ParrotPrivacy(unittest.TestCase):
    def test_no_storage_network_logging_or_second_copy(self):
        for path in PARROT:
            code = code_only(path.read_text(encoding="utf-8"))
            for pattern in FORBIDDEN:
                self.assertIsNone(re.search(pattern, code), f"{path.name}: forbidden {pattern!r}")

    def test_only_kid_app_and_plain_std_headers(self):
        for path in PARROT:
            for inc in re.findall(r'#include\s+[<"]([^>"]+)[>"]', path.read_text(encoding="utf-8")):
                if "/" in inc:
                    self.assertTrue(inc.startswith("kid_app/"), f"{path.name}: {inc}")
                else:
                    self.assertIn(inc, ALLOWED_STD_INCLUDES, f"{path.name}: {inc}")

    def test_wipe_uses_a_volatile_pointer_and_runs_on_every_exit(self):
        code = code_only((KID / "src/parrot_session.cc").read_text(encoding="utf-8"))
        self.assertRegex(code, r"volatile\s+int16_t\s*\*")
        header = code_only(PARROT[0].read_text(encoding="utf-8"))
        self.assertRegex(header, r"~ParrotSession\(\)\s*\{\s*Wipe\(\);\s*\}")
        scrub = code[code.index("void ParrotSession::Scrub"):]
        scrub = scrub[:scrub.index("}" + chr(10))]
        self.assertRegex(scrub, r"volatile\s+int16_t\s*\*")
        # Stop(), PlaybackRefused() and the last Render block all wipe.
        for fn in ("ParrotOutput ParrotSession::Stop", "void ParrotSession::PlaybackRefused",
                   "size_t ParrotSession::Render"):
            body = code[code.index(fn):]
            body = body[:body.index("\n}\n")]
            self.assertIn("Wipe();", body, fn)

    def test_only_the_parrot_owns_the_microphone_buffer(self):
        """No other firmware source includes the parrot header yet; when the board
        wiring lands it must pass this review point to the privacy doc."""
        users = [p for p in (KID / "src").glob("*.cc") if "parrot_session.h" in p.read_text(encoding="utf-8")
                 and p.name != "parrot_session.cc"]
        self.assertEqual(users, [])


if __name__ == "__main__":
    unittest.main()
