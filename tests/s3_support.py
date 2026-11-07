"""Shared fixtures for the S3 tests: app, parent session, device snapshots."""
import base64
from pathlib import Path
import sys
import tempfile
import unittest

from cryptography.fernet import Fernet
from fastapi.testclient import TestClient

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from services.pet_learning.api import create_app  # noqa: E402
from services.pet_learning.auth import ParentAuth  # noqa: E402
from services.pet_learning.config import Settings  # noqa: E402
from services.pet_learning.db import connect  # noqa: E402
from services.pet_learning.memory_codec import Day, Growth, Memory, ParentSettings  # noqa: E402
from services.pet_learning.totp import code_at, new_secret  # noqa: E402

PASSWORD = "correct-test-password-1234"
TOKEN = "device-token-" + "a" * 32
VOICE_TOKEN = "voice-token-" + "c" * 32
START = 1_793_600_000
DEVICE = "pet-001"


class S3Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name) / "pet.sqlite3"
        self.now = START
        self.settings = Settings(totp_key=Fernet.generate_key().decode(), voice_token=VOICE_TOKEN,
                                 notes_key=Fernet.generate_key().decode())
        self.app = create_app(self.path, settings=self.settings)
        self.app.state.state_sync.clock = lambda: self.now
        self.app.state.profile_service.clock = lambda: self.now
        self.app.state.pet_store.enroll_device(DEVICE, TOKEN)
        self.app.state.pet_store.enroll_device("pet-002", "device-token-" + "b" * 32)
        self.client = TestClient(self.app, base_url="https://testserver", client=("127.0.0.1", 5000))
        self.addCleanup(self.client.close)
        self.generation = 0
        self.csrf = None
        self.secret = new_secret()

    # -- parent ----------------------------------------------------------

    def login(self):
        auth = ParentAuth(self.path, self.settings, clock=lambda: self.now)
        auth.create_account("parent", PASSWORD, self.secret, code_at(self.secret, self.now))
        self.app.state.parent_auth = auth
        self.now += 30
        result = self.client.post("/v1/parent/login", json={
            "username": "parent", "password": PASSWORD, "otp": code_at(self.secret, self.now)})
        self.assertEqual(result.status_code, 200)
        self.csrf = result.json()["csrf_token"]

    def proof(self, *, password=PASSWORD):
        """A fresh step-up proof: the next TOTP step, since login consumed this one."""
        self.now += 30
        return {"password": password, "otp": code_at(self.secret, self.now)}

    def parent(self, method, url, **kw):
        return self.client.request(method, url, headers={"X-CSRF-Token": self.csrf}, **kw)

    # -- device ----------------------------------------------------------

    def snapshot(self, settings=None, *, rev=0, awaiting=(), receipts=(), inventory=(), generation=None):
        if generation is None:
            self.generation += 1
            generation = self.generation
        memory = Memory(1, Day(20761, 1, 0, 0, 0, 0, 0, 0), Day(), Growth(), (), ())
        return {
            "schema_version": "2.0", "generation": generation, "ledger_schema": 3,
            "fw_version": "pet-2026.11.04", "rtc_day": 20761,
            "pet": {"mode": "IDLE", "mood": "HAPPY", "interaction_count": 5, "active_page": 1, "quiet": False},
            "memory_b64": base64.b64encode(memory.encode()).decode(),
            "parent_settings_b64": base64.b64encode((settings or ParentSettings()).encode()).decode(),
            "parent_settings_rev": rev, "battery": {"pct": 64, "charging": False},
            "inventory": list(inventory), "sd_free_kib": 1000, "awaiting_confirm": list(awaiting),
            "applied_commands": [{"command_id": c, "result": r} for c, r in receipts]}

    def put_state(self, body, *, advance=11):
        self.now += advance
        result = self.client.put(f"/v1/devices/{DEVICE}/state", json=body,
                                 headers={"Authorization": f"Bearer {TOKEN}"})
        self.assertEqual(result.status_code, 200, result.text)
        return result.json()

    def table(self, sql, *args):
        with connect(self.path) as db:
            return [tuple(r) for r in db.execute(sql, args)]
