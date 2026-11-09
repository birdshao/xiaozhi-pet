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


# --- V9 S1 (voice effects): assertions added, none of the above relaxed -------
# The effects (pitch, robot ring modulation, echo) are computed on the fly in
# Render() from the one recording; they may not introduce a second buffer, any
# persistent state beyond a few integers, or any way out of RAM.
NEW_DRAW = KID / "src/sound_draw_v9.inc"
# A constexpr lookup table (kPermille) is not a copy of the voice; any other
# array of sample-sized integers or floats is refused.
SAMPLE_ARRAY = re.compile(
    r"(?<!constexpr )\b(?:u?int(?:8|16|32|64)_t|short|float|double)\s+\w+\s*\[\s*[^\]]+\]\s*[;=]")
# Every way of storing into buffer_: =, op=, ++/--, *buffer_ = ..., *(buffer_ + i) = ...
BUFFER_WRITE = re.compile(
    r"\bbuffer_\s*\[[^\]]*\]\s*(?:(?:[-+*/%&|^]|<<|>>)?=(?!=)|\+\+|--)"
    r"|(?:\+\+|--)\s*buffer_\s*\["
    r"|\*\s*\(?\s*buffer_\b[^;=]*=(?!=)")


def body_of(code: str, signature: str) -> str:
    start = code.index(signature)
    return code[start:code.index("\n}\n", start)]


class ParrotVoiceEffectsPrivacy(unittest.TestCase):
    def test_effects_add_no_second_copy_buffer_or_container(self):
        extra = [r"\bstd::array\b", r"\bcalloc\b", r"\brealloc\b", r"\balloca\b",
                 r"\bthread_local\b", r"\bmemmove\b", r"\bstd::copy\b", r"\bstd::unique_ptr\b",
                 r"\bstd::make_", r"\bstd::deque\b", r"\bstd::queue\b",
                 r"\bmemset\b", r"\bstd::fill", r"\bstd::transform\b", r"\bstd::generate",
                 r"\bstd::replace", r"\bstd::swap", r"\bstd::reverse\b"]
        for path in PARROT:
            code = code_only(path.read_text(encoding="utf-8"))
            for pattern in extra:
                self.assertIsNone(re.search(pattern, code), f"{path.name}: forbidden {pattern!r}")
            self.assertIsNone(SAMPLE_ARRAY.search(code), f"{path.name}: a local/static sample array")

    def test_the_session_owns_exactly_one_buffer_pointer(self):
        header = code_only(PARROT[0].read_text(encoding="utf-8"))
        self.assertEqual(len(re.findall(r"int16_t\s*\*\s*\w+_\s*[=;]", header)), 1)
        self.assertRegex(header, r"int16_t\*\s+buffer_\s*=\s*nullptr;")
        self.assertNotRegex(header, r"\bint16_t\s+\w+_\s*\[")

    def test_the_write_and_array_detectors_actually_bite(self):
        for bad in ("buffer_[i] += 1;", "buffer_[i]++;", "--buffer_[i];", "*buffer_ = 0;",
                    "*(buffer_ + i) = 3;", "buffer_[i] <<= 1;", "buffer_[i] = 2;"):
            self.assertIsNotNone(BUFFER_WRITE.search(bad), bad)
        for fine in ("if (buffer_[i] != 0)", "bool b = buffer_[i] == 3;", "const int32_t a = buffer_[i];"):
            self.assertIsNone(BUFFER_WRITE.search(fine), fine)
        for bad in ("uint32_t x[4];", "int32_t y[8] = {};", "uint16_t z[2];", "int16_t w[1];"):
            self.assertIsNotNone(SAMPLE_ARRAY.search(bad), bad)
        self.assertIsNone(SAMPLE_ARRAY.search("static constexpr uint32_t kPermille[5] = {1};"))

    def test_only_feed_writes_the_recording_effects_only_read_it(self):
        code = code_only((KID / "src/parrot_session.cc").read_text(encoding="utf-8"))
        writes = [m.start() for m in BUFFER_WRITE.finditer(code)]
        feed_start = code.index("size_t ParrotSession::Feed")
        feed_end = feed_start + len(body_of(code, "size_t ParrotSession::Feed"))
        self.assertEqual(len(writes), 1)
        for at in writes:
            self.assertTrue(feed_start <= at < feed_end, "buffer_ written outside Feed()")
        render = body_of(code, "size_t ParrotSession::Render")
        self.assertIsNone(BUFFER_WRITE.search(render))
        self.assertIn("Wipe();", render)  # the last block still wipes

    def test_new_view_and_drawing_code_is_pure_drawing(self):
        banned = [r"\bfopen\b", r"\bfwrite\b", r"\bnvs_", r"\bhttp", r"\bwifi\b", r"\bsocket\b",
                  r"\bheap_caps", r"\bmalloc\b", r"\bESP_LOG", r"\bprintf\b", r"\bmemcpy\b"]
        for path in (NEW_DRAW, KID / "src/sound_view.cc", KID / "include/kid_app/sound_view.h"):
            self.assertTrue(path.exists(), path.name)
            code = code_only(path.read_text(encoding="utf-8"))
            for pattern in banned:
                self.assertIsNone(re.search(pattern, code), f"{path.name}: forbidden {pattern!r}")
            self.assertNotIn("parrot_buffer_", code)
        view = code_only((KID / "src/sound_view.cc").read_text(encoding="utf-8"))
        self.assertNotIn("Recorded(", view)
        self.assertNotIn("Render(", view)

    def test_board_parrot_wiring_logs_counts_only_and_has_no_exit_path(self):
        src = (ROOT / "scripts/p1_integrate.py").read_text(encoding="utf-8")
        parrot = src[src.index("void ApplyParrotOutput("):src.index("// ---- songs ----")]
        for bad in ("fopen", "fwrite", "nvs_", "esp_http", "esp_websocket", "SendAudio", "Opus"):
            self.assertNotIn(bad, parrot)
        for line in re.findall(r'ESP_LOG\w\(TAG, "PET_PARROT[^;]*;', parrot):
            self.assertNotRegex(line, r"chunk|buffer|samples\[|parrot_buffer_|Render\(")


if __name__ == "__main__":
    unittest.main()
