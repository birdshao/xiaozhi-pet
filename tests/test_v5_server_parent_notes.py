"""S3 / D9: parent notes are encrypted at rest and isolated from voice, models, devices and logs."""
import logging
from pathlib import Path
import re
import sys
import unittest

from cryptography.fernet import Fernet
from fastapi.testclient import TestClient

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from s3_support import DEVICE, PASSWORD, S3Base, TOKEN, VOICE_TOKEN  # noqa: E402
from services.pet_learning import ops  # noqa: E402
from services.pet_learning.api import create_app  # noqa: E402
from services.pet_learning.auth import ParentAuth  # noqa: E402
from services.pet_learning.config import Settings  # noqa: E402
from services.pet_learning.db import connect  # noqa: E402
from services.pet_learning.totp import code_at  # noqa: E402

URL = f"/v1/parent/devices/{DEVICE}/notes"
NICK = "小满XYZZYnick"
NOTE = "对花生过敏XYZZYnote，睡前要听故事"
SECRETS = ("XYZZYnick", "XYZZYnote", "对花生过敏", "05-17")
SERVICE = ROOT / "services" / "pet_learning"


class Capture(logging.Handler):
    def __init__(self):
        super().__init__(logging.DEBUG)
        self.lines = []

    def emit(self, record):
        self.lines.append(record.getMessage())


class NotesBase(S3Base):
    def setUp(self):
        super().setUp()
        self.login()

    def write(self, **body):
        body = {"nickname": NICK, "birthday": "05-17", "note": NOTE, **body}
        return self.parent("PUT", URL, json={**body, "reauth": self.proof()})

    def everything_on_disk(self):
        data = b""
        for path in self.path.parent.glob(self.path.name + "*"):
            data += path.read_bytes()
        return data


class EncryptionTests(NotesBase):
    def test_round_trip_and_ciphertext_only_on_disk(self):
        self.assertEqual(self.write().status_code, 200)
        got = self.client.get(URL).json()
        self.assertEqual((got["exists"], got["nickname"], got["birthday"], got["note"]), (True, NICK, "05-17", NOTE))
        row = self.table("SELECT payload_enc FROM parent_notes")[0][0]
        for secret in SECRETS:
            self.assertNotIn(secret, row)
            self.assertNotIn(secret.encode(), self.everything_on_disk())
        # the stored token really is the application key's Fernet token, bound to this device
        plain = Fernet(self.settings.notes_key.encode()).decrypt(row.encode()).decode()
        self.assertIn(DEVICE, plain)

    def test_unset_notes_read_as_empty(self):
        self.assertEqual(self.client.get(URL).json(), {"exists": False, "nickname": None, "birthday": None,
                                                       "note": None, "updated_at": None})

    def test_no_key_fails_closed_without_a_plaintext_fallback(self):
        settings = Settings(totp_key=self.settings.totp_key, voice_token=VOICE_TOKEN)
        app = create_app(self.path, settings=settings)
        app.state.profile_service.clock = lambda: self.now
        app.state.parent_auth = ParentAuth(self.path, settings, clock=lambda: self.now)
        with TestClient(app, base_url="https://testserver") as client:
            self.now += 30  # the fixture login already used this TOTP step
            login = client.post("/v1/parent/login", json={
                "username": "parent", "password": PASSWORD, "otp": code_at(self.secret, self.now)})
            self.assertEqual(login.status_code, 200, login.text)
            csrf = login.json()["csrf_token"]
            self.assertEqual(client.get(URL).json()["detail"], "NOTES_KEY_MISSING")
            self.now += 30
            result = client.put(URL, headers={"X-CSRF-Token": csrf}, json={
                "note": NOTE, "reauth": {"password": PASSWORD, "otp": code_at(self.secret, self.now)}})
            self.assertEqual((result.status_code, result.json()["detail"]), (503, "NOTES_KEY_MISSING"))
        self.assertEqual(self.table("SELECT COUNT(*) FROM parent_notes"), [(0,)])
        self.assertNotIn(b"XYZZYnote", self.everything_on_disk())

    def test_production_settings_refuse_to_start_without_the_key(self):
        with self.assertRaises(ValueError):
            Settings(production=True, public_origin="https://pet.example", trusted_proxy="192.168.1.1",
                     totp_key=self.settings.totp_key).validate()

    def test_wrong_key_or_moved_row_is_unreadable_never_garbled(self):
        self.write()
        other = Settings(totp_key=self.settings.totp_key, notes_key=Fernet.generate_key().decode())
        self.app.state.profile_service.notes.settings = other
        self.assertEqual(self.client.get(URL).json()["detail"], "NOTES_UNREADABLE")
        self.app.state.profile_service.notes.settings = self.settings
        with connect(self.path) as db:  # the same ciphertext under another device id must not decrypt
            db.execute("INSERT INTO parent_notes SELECT 'pet-002', payload_enc, updated_at FROM parent_notes")
        self.assertEqual(self.client.get("/v1/parent/devices/pet-002/notes").json()["detail"], "NOTES_UNREADABLE")


class ValidationAndAuthTests(NotesBase):
    def test_limits_and_formats(self):
        bad = ({"note": "x" * 501}, {"nickname": "n" * 21}, {"birthday": "2020-05-17"}, {"birthday": "02-30"},
               {"birthday": "13-01"}, {"birthday": "05-17\n"}, {"note": "bad\x00char"}, {"nickname": "a\x1bb"})
        for change in bad:
            with self.subTest(change=change):
                self.assertEqual(self.write(**change).status_code, 422)
        self.assertEqual(self.write(note="x" * 500, birthday="02-29").status_code, 200)
        self.assertEqual(self.write(nickname=None, birthday=None, note=None).status_code, 422)
        self.assertEqual(self.write(note="第一行\n第二行").status_code, 200)

    def test_writes_need_password_and_a_fresh_totp(self):
        base = {"note": NOTE}
        self.assertEqual(self.parent("PUT", URL, json=base).status_code, 422)  # reauth is mandatory
        self.assertEqual(self.parent("PUT", URL, json={**base, "reauth": self.proof(password="nope-nope-nope")}).status_code, 401)
        proof = self.proof()
        self.assertEqual(self.parent("PUT", URL, json={**base, "reauth": proof}).status_code, 200)
        self.assertEqual(self.parent("PUT", URL, json={**base, "reauth": proof}).status_code, 401)  # replay
        self.assertEqual(self.parent("DELETE", URL, json={"reauth": proof}).status_code, 401)
        self.assertEqual(self.parent("DELETE", URL, json={"reauth": self.proof()}).status_code, 200)
        self.assertFalse(self.client.get(URL).json()["exists"])

    def test_audit_says_modified_never_what(self):
        self.write()
        self.parent("DELETE", URL, json={"reauth": self.proof()})
        rows = self.table("SELECT actor, action, target, result FROM audit_log WHERE action LIKE 'notes.%'")
        self.assertEqual(rows, [("parent", "notes.update", DEVICE, "success"), ("parent", "notes.delete", DEVICE, "success")])
        for row in self.table("SELECT * FROM audit_log"):
            self.assertFalse(any(secret in " ".join(map(str, row)) for secret in SECRETS))


class LifecycleTests(NotesBase):
    def test_clearing_memory_keeps_notes_and_deleting_the_device_removes_them(self):
        self.write()
        self.put_state(self.snapshot())
        self.assertEqual(self.parent("DELETE", f"/v1/parent/devices/{DEVICE}/memories").status_code, 200)
        self.assertTrue(self.client.get(URL).json()["exists"])
        with connect(self.path) as db:
            db.execute("DELETE FROM devices WHERE device_id=?", (DEVICE,))
        self.assertEqual(self.table("SELECT COUNT(*) FROM parent_notes"), [(0,)])

    def test_retention_jobs_never_touch_notes(self):
        self.write()
        self.now += 5 * 365 * 86400
        self.app.state.state_sync.run_retention(self.now)
        ops.cleanup(self.path, now=self.now)
        self.assertEqual(self.table("SELECT COUNT(*) FROM parent_notes"), [(1,)])


class IsolationTests(NotesBase):
    def test_only_the_parent_panel_module_can_reach_notes(self):
        importers = []
        for path in SERVICE.rglob("*.py"):
            text = path.read_text(encoding="utf-8")
            if re.search(r"(from\s+\.+\s*notes\b|from\s+\.+notes\s+import|import\s+notes\b|"
                         r"from\s+services\.pet_learning\.notes|parent_notes)", text):
                importers.append(path.name)
        self.assertEqual(sorted(importers), ["notes.py", "profile.py"], importers)
        profile = (SERVICE / "profile.py").read_text(encoding="utf-8")
        self.assertEqual(len(re.findall(r"self\.notes\.", profile)), 3)  # get / put / delete only

    def test_notes_never_reach_voice_summary_device_api_parent_views_or_logs(self):
        handler = Capture()
        logging.getLogger().addHandler(handler)
        previous = logging.getLogger().level
        logging.getLogger().setLevel(logging.DEBUG)
        self.addCleanup(lambda: (logging.getLogger().removeHandler(handler), logging.getLogger().setLevel(previous)))
        self.write()
        self.parent("PUT", f"/v1/parent/devices/{DEVICE}/profile", json={"base_rev": 0, "pet_name": "x"})
        self.assertEqual(self.client.post(f"/v1/parent/devices/{DEVICE}/init", headers={"X-CSRF-Token": self.csrf},
                                          json={"pet_name": "团团", "temperament": "gentle"}).status_code, 200)
        self.parent("PUT", f"/v1/parent/devices/{DEVICE}/policy", json={"base_rev": 0, "settings": {"volume_cap_pct": 70}})
        seen = [self.put_state(self.snapshot()), self.put_state(self.snapshot(generation=1), advance=1)]
        responses = [r for r in (
            self.client.get(f"/v1/voice/{DEVICE}/summary", headers={"Authorization": f"Bearer {VOICE_TOKEN}"}),
            self.client.get(f"/v1/devices/{DEVICE}/commands", headers={"Authorization": f"Bearer {TOKEN}"}),
            self.client.post(f"/v1/voice/{DEVICE}/turn", headers={"Authorization": f"Bearer {VOICE_TOKEN}"},
                             json={"text": "我叫什么名字", "turn_id": "c0ffee01"}),
            self.client.get(f"/v1/parent/devices/{DEVICE}/memories"),
            self.client.get(f"/v1/parent/devices/{DEVICE}/profile"),
            self.client.get(f"/v1/parent/devices/{DEVICE}/policy"),
            self.client.get("/v1/parent/devices"),
        )]
        blob = " ".join([r.text for r in responses] + [str(s) for s in seen] + handler.lines
                        + [" ".join(map(str, row)) for row in self.table("SELECT * FROM commands")]
                        + [" ".join(map(str, row)) for row in self.table("SELECT * FROM profiles")]
                        + [" ".join(map(str, row)) for row in self.table("SELECT * FROM policies")]
                        + [" ".join(map(str, row)) for row in self.table("SELECT * FROM memory_current")])
        for secret in SECRETS:
            self.assertNotIn(secret, blob)
        self.assertTrue(any(r.status_code == 200 for r in responses[:2]))


if __name__ == "__main__":
    unittest.main()
