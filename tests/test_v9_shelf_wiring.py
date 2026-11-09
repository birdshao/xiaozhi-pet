"""V9 1B bookshelf: board wiring in the p1 overlay (no ESP-IDF needed)."""
import unittest
from pathlib import Path

from scripts.p1_integrate import patch_board

ROOT = Path(__file__).resolve().parent.parent
KID = ROOT / "firmware/components/kid_app"


def _board() -> str:
    return patch_board('#include <stdio.h>\n'
                       'class Board {\n'
                       '    void InitializeButtons() {\n'
                       '        boot_button_.OnClick([this]() {\n'
                       '            app.ToggleChatState();\n'
                       '        });\n'
                       '        pwr_button_.OnClick([this]() {\n'
                       '        });\n'
                       '    }\n'
                       '};\n')


def _body(text: str, start: str) -> str:
    body = text[text.index(start):]
    return body[:body.index('\n    }\n')]


class ShelfWiringTest(unittest.TestCase):
    def setUp(self):
        self.patched = _board()

    def test_include_and_members(self):
        self.assertIn('#include "kid_app/book_shelf.h"', self.patched)
        for member in ('kid_app::BookShelf shelf_;', 'kid_app::ShelfView shelf_view_{};',
                       'bool selftest_book_fixed_ = false;',
                       'enum class StoryPhase : uint8_t { Pick, Shelf, Opening, Leaving, Book };'):
            self.assertIn(member, self.patched)

    def test_old_direct_open_is_kept(self):
        # The self-test "book" step keeps the old direct open, and the card is
        # only scanned when no book was named.
        step = self.patched[self.patched.index('case kid_app::SelfTestOp::Book:'):]
        step = step[:step.index('break;')]
        self.assertIn('selftest_book_fixed_ = true;', step)
        init = _body(self.patched, 'bool InitializePetRuntime()')
        self.assertIn('if (!selftest_book_fixed_ && story_sd_.Mounted()) ScanShelf();', init)
        # One book (or none) never shows the shelf.
        phase = _body(self.patched, 'bool UpdateShelfPhase()')
        self.assertIn('if (!shelf_.UseShelf())', phase)
        self.assertLess(phase.index('if (!shelf_.UseShelf())'), phase.index('PET_SHELF_OPEN'))

    def test_story_opens_shelf_before_book(self):
        update = _body(self.patched, 'void UpdateBook()')
        self.assertLess(update.index('UpdateShelfPhase()'), update.index('book_.Open('))
        # Leaving Story starts the next visit on the shelf again.
        self.assertIn('StoryPhase::Pick', update[:update.index('book_.Close()')])

    def test_dial_goes_to_shelf_until_a_book_is_picked(self):
        handle = _body(self.patched, 'void HandleInputEvent(')
        self.assertIn('HandleShelfInput(event.kind)', handle)
        self.assertLess(handle.index('book_.OnInput(event.kind'), handle.index('HandleShelfInput'))
        # pet_input never reads the card: a short task does the switch.
        trigger = _body(self.patched, 'void OpenShelfBook(uint8_t index)')
        self.assertNotIn('SelectBook', trigger)
        self.assertIn('StoryPhase::Opening', trigger)
        self.assertIn('ShelfPickWork()', trigger)
        # The dial does nothing while the book is opening.
        self.assertIn('event.kind == kid_app::InputKind::DialLong)', handle)
        self.assertIn('ShelfGoHome();', handle)
        pick = _body(self.patched, 'void ShelfPickWork()')
        # The card is switched under the ePaper screen lock, never without it.
        self.assertLess(pick.index('xSemaphoreTake(pet_screen_mutex_'), pick.index('SelectBook(id)'))
        self.assertLess(pick.index('SelectBook(id)'), pick.index('xSemaphoreGive(pet_screen_mutex_)'))
        self.assertIn('book_panel_valid_ = false;', pick)
        self.assertIn('story_pcm_.RequestBook(id)', pick)
        # A failure goes back to the shelf: only success moves on to the book.
        self.assertIn('MoveStoryPhase(StoryPhase::Opening, opened ? StoryPhase::Book : StoryPhase::Shelf)', pick)
        self.assertNotIn('compare_exchange', self.patched)
        # The shelf never writes memory or a like.
        shelf_code = self.patched[self.patched.index('void ScanShelf()'):
                                  self.patched.index('void UpdateBook()')]
        self.assertNotIn('RecordMemory', shelf_code)
        self.assertNotIn('PostMemory', shelf_code)

    def test_shelf_scene_survives_republish(self):
        unpack = _body(self.patched, 'kid_app::SceneModel UnpackPetScene(uint32_t bits) const')
        self.assertIn('scene.shelf = shelf_view_', unpack)

    def test_firmware_sources(self):
        worker = (KID / "src/freertos_story_pcm_worker.cc").read_text(encoding="utf-8")
        # Start() takes no book; a pending book is applied before any request.
        self.assertIn('with_book', worker)
        self.assertLess(worker.index('book_pending_.exchange(false)'),
                        worker.index('if (!request.page) continue;'))
        sd = (KID / "src/waveshare_story_sd.cc").read_text(encoding="utf-8")
        self.assertIn('size_t WaveshareStorySd::ScanShelf', sd)
        self.assertIn('bool WaveshareStorySd::SelectBook', sd)
        scene = (KID / "src/scene_model.cc").read_text(encoding="utf-8")
        self.assertIn('#include "shelf_draw.inc"', scene)

    def test_review_fixes(self):
        # Book names up to 63 chars: every buffer on the shelf path is 64.
        for decl in ('char shelf_last_[64]', 'char shelf_pick_[64]', 'char shelf_book_[64]'):
            self.assertIn(decl, self.patched)
        self.assertIn('char id[sizeof(shelf_pick_)];', self.patched)
        # A card whose only book is not the default one opens it through
        # SelectBook, and its narration worker starts on that book.
        scan = _body(self.patched, 'void ScanShelf()')
        self.assertIn('count == 1 && !story_sd_.Ready() && story_sd_.SelectBook(shelf_.At(0).id)',
                      scan)
        self.assertIn('story_pcm_.Start("/sdcard/books", shelf_book_,', self.patched)
        # Home from the shelf never goes back to Pick before Story is left.
        home = _body(self.patched, 'void ShelfGoHome()')
        self.assertIn('StoryPhase::Leaving', home)
        self.assertNotIn('StoryPhase::Pick', home)
        # Only one pick task at a time.
        self.assertIn('shelf_pick_busy_.exchange(true)', self.patched)
        # Voice "next page" / "previous page" turn the shelf.
        self.assertIn('HandleShelfInput(kid_app::InputKind::DialDown);  // V9', self.patched)
        self.assertIn('HandleShelfInput(kid_app::InputKind::DialUp);  // V9', self.patched)

    def test_selftest_scenario_lines(self):
        path = ROOT / "tools/selftest/v9_shelf.pettest"
        text = path.read_text(encoding="ascii")
        self.assertTrue(text.startswith("PETSELFTEST1\n"))
        for line in text.splitlines():
            self.assertLessEqual(len(line), 80, line)
        # Same rule as the old reader scenarios, but the shelf one never names
        # a book (that step keeps the direct open).
        steps = [l.split()[0] for l in text.splitlines() if l and not l.startswith("#")]
        self.assertNotIn("book", steps)


if __name__ == "__main__":
    unittest.main()
