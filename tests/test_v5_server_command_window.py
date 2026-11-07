"""Command delivery window: at most 8 per reply, awaiting-confirmation ones are not resent."""
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from s3_support import DEVICE, S3Base, TOKEN  # noqa: E402
from services.pet_learning import commands  # noqa: E402
from services.pet_learning.db import connect  # noqa: E402


class CommandWindowTests(S3Base):
    def queue(self, count):
        ids = []
        with connect(self.path) as db:
            db.execute("BEGIN IMMEDIATE")
            for _ in range(count):
                ids.append(commands.enqueue(db, DEVICE, "SYNC_CONTENT", {}, self.now)["command_id"])
            db.commit()
        return ids

    def test_thirty_five_pending_commands_arrive_in_order_eight_at_a_time(self):
        ids = self.queue(35)
        first = self.put_state(self.snapshot())["commands"]
        self.assertEqual([c["command_id"] for c in first], ids[:8])
        # The pull route obeys the same window.
        pulled = self.client.get(f"/v1/devices/{DEVICE}/commands", headers={"Authorization": f"Bearer {TOKEN}"}).json()
        self.assertEqual([c["command_id"] for c in pulled["commands"]], ids[:8])
        # Until the device reports receipts the same eight are offered again; then the window moves on.
        sent = ids[:8]
        seen = list(sent)
        for _ in range(6):
            body = self.put_state(self.snapshot(receipts=[(c, "applied") for c in sent]))
            sent = [c["command_id"] for c in body["commands"]]
            self.assertLessEqual(len(sent), 8)
            seen += sent
        self.assertEqual(list(dict.fromkeys(seen)), ids)  # every command delivered once, in creation order
        self.assertEqual(self.table("SELECT COUNT(*) FROM commands WHERE state='pending'"), [(0,)])

    def test_commands_awaiting_parent_confirmation_do_not_starve_later_ones(self):
        ids = self.queue(12)
        self.put_state(self.snapshot())
        waiting = ids[:8]  # the device holds eight requests waiting for the parent
        body = self.put_state(self.snapshot(awaiting=waiting))
        self.assertEqual([c["command_id"] for c in body["commands"]], ids[8:])
        # They stay pending (the panel shows "awaiting confirmation") and come back if the device forgets them.
        view = self.table("SELECT COUNT(*) FROM commands WHERE state='pending'")
        self.assertEqual(view, [(12,)])
        body = self.put_state(self.snapshot(awaiting=[]))
        self.assertEqual([c["command_id"] for c in body["commands"]], ids[:8])

    def test_undelivered_commands_beyond_the_window_are_not_marked_delivered(self):
        self.queue(10)
        self.put_state(self.snapshot())
        delivered = self.table("SELECT COUNT(*) FROM commands WHERE delivered_at IS NOT NULL")
        self.assertEqual(delivered, [(8,)])


if __name__ == "__main__":
    unittest.main()
