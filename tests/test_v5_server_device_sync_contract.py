"""Device <-> server sync contract (V5_V6 §5.1-5.3, §11.2): shared fixtures.

tests/fixtures/sync/ is the wire contract between the firmware and S1/S2:

* snapshot_*.json are what the C++ EncodeStateSnapshot produces
  (tests/cpp/test_server_sync.cc asserts it byte for byte). Here the real
  server must accept every one, and each must be the compact JSON of the
  reference encoding below, built from the S2 golden vectors in
  tests/fixtures/memory/.
* state_reply.json / commands_reply.json are what the real server answers;
  the C++ parsers are tested against these exact files.
* pairing_*.json / token_rotate.json have the shape S1 answers with (checked
  here against a real pairing and rotation); the C++ parsers read them.

WRITE_SYNC_FIXTURES=1 regenerates the snapshot and reply fixtures.
"""
import base64
import json
import os
from pathlib import Path
import re
import sys
import tempfile
import unittest
from unittest.mock import patch

from cryptography.fernet import Fernet
from fastapi.testclient import TestClient

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from services.pet_learning import commands, pairing  # noqa: E402
from services.pet_learning.api import create_app  # noqa: E402
from services.pet_learning.auth import ParentAuth  # noqa: E402
from services.pet_learning.config import Settings  # noqa: E402
from services.pet_learning.db import connect  # noqa: E402
from services.pet_learning.memory_codec import decode_memory, decode_parent_settings  # noqa: E402
from services.pet_learning.totp import code_at, new_secret  # noqa: E402

MEMORY = ROOT / "tests" / "fixtures" / "memory"
SYNC = ROOT / "tests" / "fixtures" / "sync"
WRITE = os.environ.get("WRITE_SYNC_FIXTURES") == "1"
TOKEN = "device-token-" + "a" * 32
PASSWORD = "correct-test-password-1234"
START = 1_793_600_000
MAX_ID = (1 << 53) - 1

# Mirrors kSnapshotCases in tests/cpp/test_server_sync.cc.
CASES = {
    "empty": dict(memory="memory_empty.bin", settings="settings_default.bin", generation=1,
                  rtc_day=0, pet=("IDLE", "NEUTRAL", 0, 1, False), battery=(0, False),
                  awaiting=[], receipts=[]),
    "rich": dict(memory="memory_rich.bin", settings="settings_custom.bin", generation=1842,
                 rtc_day=20761, pet=("STORY", "HAPPY", 57, 3, False), battery=(64, True),
                 awaiting=[4098], receipts=[(4097, "applied"), (4099, "rejected"),
                                            (4100, "expired")]),
    "wiped": dict(memory="memory_wiped.bin", settings="settings_default.bin", generation=77,
                  rtc_day=20762, pet=("INTERACT", "CURIOUS", 3, 1, False), battery=(100, True),
                  awaiting=[], receipts=[(9007199254740000, "applied")]),
    "worst": dict(memory="memory_full.bin", settings="settings_custom.bin",
                  generation=(1 << 63) - 1, rtc_day=0xFFFFFFFF,
                  pet=("QUIET", "SLEEPY", 0xFFFFFFFF, 0xFFFF, True), battery=(100, False),
                  awaiting=[MAX_ID - i for i in range(8)],
                  receipts=[(MAX_ID - 100 - i, "rejected") for i in range(16)]),
}


def reference_snapshot(case):
    """The device encoding, key order included (compact JSON)."""
    mode, mood, count, page, quiet = case["pet"]
    body = {
        "schema_version": "2.0",
        "generation": case["generation"],
        "ledger_schema": 3,
        "fw_version": "pet-2026.11.06",
        "rtc_day": case["rtc_day"],
        "pet": {"mode": mode, "mood": mood, "interaction_count": count, "active_page": page,
                "quiet": quiet},
        "memory_b64": base64.b64encode((MEMORY / case["memory"]).read_bytes()).decode(),
        "parent_settings_b64": base64.b64encode((MEMORY / case["settings"]).read_bytes()).decode(),
        "parent_settings_rev": 0,
        "battery": {"pct": case["battery"][0], "charging": case["battery"][1]},
        "inventory": [],
        "awaiting_confirm": case["awaiting"],
        "applied_commands": [{"command_id": c, "result": r} for c, r in case["receipts"]],
    }
    return json.dumps(body, separators=(",", ":"))


class ServerBase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name) / "pet.sqlite3"
        self.now = START
        patcher = patch.object(pairing, "now", lambda: self.now)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.settings = Settings(totp_key=Fernet.generate_key().decode())
        self.app = create_app(self.path, settings=self.settings)
        self.app.state.state_sync.clock = lambda: self.now
        self.app.state.pet_store.enroll_device("pet-001", TOKEN)
        self.client = TestClient(self.app, base_url="https://testserver")
        self.addCleanup(self.client.close)

    def put(self, text, *, token=TOKEN):
        self.now += 11
        return self.client.put("/v1/devices/pet-001/state", content=text.encode(),
                               headers={"Authorization": f"Bearer {token}",
                                        "Content-Type": "application/json"})


class SnapshotFixtureTests(ServerBase):
    def test_snapshot_fixtures_are_the_reference_encoding_and_accepted(self):
        for name, case in CASES.items():
            with self.subTest(name=name):
                path = SYNC / f"snapshot_{name}.json"
                expected = reference_snapshot(case)
                if WRITE:
                    path.write_text(expected, encoding="ascii", newline="")
                text = path.read_text(encoding="ascii")
                self.assertEqual(text, expected)
                self.assertLessEqual(len(text.encode()), 5120)
                body = json.loads(text)
                self.assertEqual(decode_memory(base64.b64decode(body["memory_b64"])),
                                 decode_memory((MEMORY / case["memory"]).read_bytes()))
                decode_parent_settings(base64.b64decode(body["parent_settings_b64"]))
                # Fresh server per fixture: every one is a first snapshot.
                self.setUp()
                result = self.put(text)
                self.assertEqual(result.status_code, 200, result.text)
                self.assertEqual(result.json()["result"], "accepted")
                # The same bytes again: a duplicate, not a conflict.
                again = self.put(text)
                self.assertEqual(again.json()["result"], "duplicate")


class ReplyFixtureTests(ServerBase):
    def check(self, name, value):
        path = SYNC / name
        if WRITE:
            path.write_text(json.dumps(value, indent=1, sort_keys=True) + "\n", encoding="ascii",
                            newline="\n")
        self.assertEqual(json.loads(path.read_text(encoding="ascii")), value)

    def test_state_and_commands_replies(self):
        ids = iter([4097, 4098, 4099])
        with patch.object(commands.secrets, "randbits", lambda bits: next(ids)):
            with connect(self.path) as db:
                commands.enqueue(db, "pet-001", "MEMORY_CLEAR", {"clear_epoch": 3}, self.now)
                commands.enqueue(db, "pet-001", "SET_TEMPERAMENT", {"temperament": "lively"}, self.now)
                commands.enqueue(db, "pet-001", "SYNC_CONTENT", {}, self.now)
        case = dict(CASES["rich"], generation=1, awaiting=[4098], receipts=[(4099, "rejected")])
        result = self.put(reference_snapshot(case))
        self.assertEqual(result.status_code, 200, result.text)
        reply = result.json()
        self.assertEqual(reply["acked_commands"], [4099])
        self.assertEqual([c["command_id"] for c in reply["commands"]], [4097, 4098])
        self.check("state_reply.json", reply)
        polled = self.client.get("/v1/devices/pet-001/commands",
                                 headers={"Authorization": f"Bearer {TOKEN}"})
        self.assertEqual(polled.status_code, 200)
        self.check("commands_reply.json", polled.json())


class PairingShapeTests(ServerBase):
    """The hand-written pairing fixtures have exactly the keys S1 sends."""

    def setUp(self):
        super().setUp()
        auth = ParentAuth(self.path, self.settings, clock=lambda: self.now)
        self.secret = new_secret()
        auth.create_account("parent", PASSWORD, self.secret, code_at(self.secret, self.now))
        self.app.state.parent_auth = auth
        self.now += 30
        r = self.client.post("/v1/parent/login", json={
            "username": "parent", "password": PASSWORD, "otp": code_at(self.secret, self.now)})
        self.assertEqual(r.status_code, 200)
        self.csrf = r.json()["csrf_token"]

    def fixture(self, name):
        return json.loads((SYNC / name).read_text(encoding="ascii"))

    def same_shape(self, live, name):
        fixture = self.fixture(name)
        self.assertEqual(set(live), set(fixture), name)
        for key, value in live.items():
            self.assertIs(type(value), type(fixture[key]), (name, key))

    def test_pairing_and_rotation_replies(self):
        urlsafe = re.compile(r"^[A-Za-z0-9_-]+$")
        device = "tt-0123456789ab-1f2e"  # the firmware's id shape
        pid = self.client.post("/v1/parent/pairings", headers={"X-CSRF-Token": self.csrf}).json()["pairing_id"]
        started = self.client.post("/v1/pairing/start", json={"device_id": device})
        self.assertEqual(started.status_code, 200)
        info = started.json()
        self.same_shape(info, "pairing_start.json")
        self.assertRegex(info["code"], r"^[0-9]{6}$")
        self.assertTrue(urlsafe.match(info["claim_secret"]) and 16 <= len(info["claim_secret"]) <= 64)
        pending = self.client.post("/v1/pairing/claim", json={
            "device_id": device, "claim_secret": info["claim_secret"]}).json()
        self.same_shape(pending, "pairing_claim_pending.json")
        self.assertEqual(self.client.post(f"/v1/parent/pairings/{pid}/redeem", json={"code": info["code"]},
                                          headers={"X-CSRF-Token": self.csrf}).status_code, 200)
        claimed = self.client.post("/v1/pairing/claim", json={
            "device_id": device, "claim_secret": info["claim_secret"]}).json()
        self.same_shape(claimed, "pairing_claim_claimed.json")
        self.assertEqual(self.fixture("pairing_claim_claimed.json")["device_id"], device)
        token = claimed["token"]
        self.assertTrue(urlsafe.match(token) and 32 <= len(token) <= 190)
        auth = {"Authorization": f"Bearer {token}"}
        self.assertEqual(self.client.post("/v1/pairing/confirm", json={"device_id": device},
                                          headers=auth).status_code, 200)
        rotated = self.client.post(f"/v1/devices/{device}/token/rotate", headers=auth)
        self.assertEqual(rotated.status_code, 200)
        self.same_shape(rotated.json(), "token_rotate.json")
        # The old token may not rotate again (S1 via_previous): 409, no new token.
        again = self.client.post(f"/v1/devices/{device}/token/rotate", headers=auth)
        self.assertEqual(again.status_code, 409)
        self.assertEqual(self.client.post(f"/v1/devices/{device}/token/confirm", headers={
            "Authorization": f"Bearer {rotated.json()['token']}"}).status_code, 200)


if __name__ == "__main__":
    unittest.main()
