"""S1 pairing: screen codes, device credentials, rotation, revocation, limits."""
from contextlib import closing
from pathlib import Path
import sqlite3
import tempfile
import unittest
from unittest.mock import patch

from cryptography.fernet import Fernet
from fastapi.testclient import TestClient

from services.pet_learning import pairing
from services.pet_learning.api import create_app
from services.pet_learning.auth import ParentAuth
from services.pet_learning.config import Settings
from services.pet_learning.db import connect, migrate
from services.pet_learning.totp import code_at, new_secret

PASSWORD = "correct-test-password-1234"
T0 = 1800000000


class PairingBase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name) / "pair.sqlite3"
        self.now = T0
        patcher = patch.object(pairing, "now", lambda: self.now)
        patcher.start()
        self.addCleanup(patcher.stop)
        settings = Settings(totp_key=Fernet.generate_key().decode())
        auth = ParentAuth(self.path, settings, clock=lambda: self.now)
        self.secret = new_secret()
        auth.create_account("parent", PASSWORD, self.secret, code_at(self.secret, self.now))
        self.now += 30
        self.app = create_app(self.path, settings=settings)
        self.app.state.parent_auth = auth
        self.client = TestClient(self.app, base_url="https://testserver")
        self.addCleanup(self.client.close)
        self.csrf = None

    def login(self):
        r = self.client.post("/v1/parent/login", json={
            "username": "parent", "password": PASSWORD, "otp": code_at(self.secret, self.now)})
        self.assertEqual(r.status_code, 200)
        self.csrf = r.json()["csrf_token"]

    def parent_post(self, url, **kw):
        return self.client.post(url, headers={"X-CSRF-Token": self.csrf}, **kw)

    def auth_header(self, token):
        return {"Authorization": f"Bearer {token}"}

    def start_pairing(self, device="pet-aaa"):
        if self.csrf is None:
            self.login()
        pid = self.parent_post("/v1/parent/pairings").json()["pairing_id"]
        started = self.client.post("/v1/pairing/start", json={"device_id": device})
        return pid, started

    def pair(self, device="pet-aaa"):
        pid, started = self.start_pairing(device)
        self.assertEqual(started.status_code, 200)
        info = started.json()
        self.assertEqual(self.parent_post(f"/v1/parent/pairings/{pid}/redeem",
                                          json={"code": info["code"]}).status_code, 200)
        claimed = self.client.post("/v1/pairing/claim", json={
            "device_id": device, "claim_secret": info["claim_secret"]}).json()
        self.assertEqual(self.client.post("/v1/pairing/confirm", json={"device_id": device},
                                          headers=self.auth_header(claimed["token"])).status_code, 200)
        return claimed["token"]

    def device_status(self, device, token):
        return self.client.get(f"/v1/devices/{device}/snapshot", headers=self.auth_header(token)).status_code

    def audit_rows(self):
        with connect(self.path) as db:
            return [dict(r) for r in db.execute("SELECT * FROM audit_log ORDER BY id")]


class PairingFlowTests(PairingBase):
    def test_happy_path_and_token_is_usable_everywhere(self):
        token = self.pair()
        self.assertEqual(self.device_status("pet-aaa", token), 200)
        self.assertEqual(self.client.post("/v1/devices/pet-aaa/story/stop",
                                          headers=self.auth_header(token)).status_code, 200)
        listed = self.client.get("/v1/parent/devices").json()["devices"]
        self.assertEqual([d["device_id"] for d in listed], ["pet-aaa"])
        self.assertEqual(listed[0]["status"], "active")
        self.assertEqual(listed[0]["last_seen_at"], self.now)
        # Hashes only: neither the token nor the code appears in the database.
        with connect(self.path) as db:
            dump = "\n".join(db.iterdump())
        self.assertNotIn(token, dump)
        actions = [(r["action"], r["result"]) for r in self.audit_rows() if r["action"].startswith("pairing")]
        self.assertEqual(actions, [("pairing.create", "success"), ("pairing.start", "success"),
                                   ("pairing.redeem", "success"), ("pairing.claim", "success"),
                                   ("pairing.confirm", "success")])
        self.assertNotIn(token, str(self.audit_rows()))

    def test_token_returned_once_and_unconfirmed_token_is_unusable(self):
        pid, started = self.start_pairing()
        info = started.json()
        self.assertEqual(self.client.post("/v1/pairing/claim", json={
            "device_id": "pet-aaa", "claim_secret": info["claim_secret"]}).json(), {"status": "pending"})
        self.parent_post(f"/v1/parent/pairings/{pid}/redeem", json={"code": info["code"]})
        body = {"device_id": "pet-aaa", "claim_secret": info["claim_secret"]}
        token = self.client.post("/v1/pairing/claim", json=body).json()["token"]
        self.assertEqual(self.client.post("/v1/pairing/claim", json=body).status_code, 401)
        self.assertEqual(self.device_status("pet-aaa", token), 401)  # not confirmed yet
        self.now += pairing.CONFIRM_TTL + 1
        self.assertEqual(self.client.post("/v1/pairing/confirm", json={"device_id": "pet-aaa"},
                                          headers=self.auth_header(token)).status_code, 401)
        self.client.post("/v1/pairing/start", json={"device_id": "pet-zzz"})  # triggers purge
        with connect(self.path) as db:
            self.assertIsNone(db.execute("SELECT 1 FROM devices WHERE device_id='pet-aaa'").fetchone())

    def test_start_requires_parent_created_session(self):
        self.assertEqual(self.client.post("/v1/pairing/start", json={"device_id": "pet-aaa"}).status_code, 404)
        self.assertEqual(self.client.post("/v1/parent/pairings").status_code, 401)
        self.login()
        self.assertEqual(self.client.post("/v1/parent/pairings").status_code, 403)  # no CSRF

    def test_known_device_id_cannot_be_repaired_and_second_device_blocked(self):
        self.pair()
        self.parent_post("/v1/parent/pairings")
        self.assertEqual(self.client.post("/v1/pairing/start", json={"device_id": "pet-aaa"}).status_code, 404)
        self.assertEqual(self.client.post("/v1/pairing/start", json={"device_id": "pet-bbb"}).status_code, 200)
        self.assertEqual(self.client.post("/v1/pairing/start", json={"device_id": "pet-ccc"}).status_code, 404)

    def test_expired_code_rejected(self):
        pid, started = self.start_pairing()
        self.now += pairing.PAIRING_TTL + 1
        r = self.parent_post(f"/v1/parent/pairings/{pid}/redeem", json={"code": started.json()["code"]})
        self.assertEqual(r.status_code, 404)

    def test_code_reuse_rejected(self):
        pid, started = self.start_pairing()
        code = started.json()["code"]
        self.assertEqual(self.parent_post(f"/v1/parent/pairings/{pid}/redeem", json={"code": code}).status_code, 200)
        self.assertEqual(self.parent_post(f"/v1/parent/pairings/{pid}/redeem", json={"code": code}).status_code, 404)

    def test_five_wrong_codes_void_pairing_even_for_correct_code(self):
        pid, started = self.start_pairing()
        code = started.json()["code"]
        wrong = "000000" if code != "000000" else "111111"
        statuses = [self.parent_post(f"/v1/parent/pairings/{pid}/redeem", json={"code": wrong}).status_code
                    for _ in range(5)]
        self.assertEqual(statuses, [400, 400, 400, 400, 410])
        self.assertEqual(self.parent_post(f"/v1/parent/pairings/{pid}/redeem", json={"code": code}).status_code, 404)
        claim = self.client.post("/v1/pairing/claim", json={
            "device_id": "pet-aaa", "claim_secret": started.json()["claim_secret"]})
        self.assertEqual(claim.status_code, 401)

    def test_bruteforce_across_pairings_locks_account(self):
        self.login()
        for _ in range(2):
            pid = self.parent_post("/v1/parent/pairings").json()["pairing_id"]
            self.client.post("/v1/pairing/start", json={"device_id": "pet-aaa"})
            for _ in range(4):
                self.parent_post(f"/v1/parent/pairings/{pid}/redeem", json={"code": "123456"})
        # 8 wrong so far; two more reach the account limit, then the correct code is refused too.
        pid = self.parent_post("/v1/parent/pairings").json()["pairing_id"]
        info = self.client.post("/v1/pairing/start", json={"device_id": "pet-aaa"}).json()
        for _ in range(2):
            self.parent_post(f"/v1/parent/pairings/{pid}/redeem", json={"code": "123456"})
        r = self.parent_post(f"/v1/parent/pairings/{pid}/redeem", json={"code": info["code"]})
        self.assertEqual(r.status_code, 429)

    def test_device_start_rate_limit_and_restart_cap(self):
        self.login()
        self.parent_post("/v1/parent/pairings")
        codes = [self.client.post("/v1/pairing/start", json={"device_id": "pet-aaa"}).status_code
                 for _ in range(4)]
        self.assertEqual(codes, [200, 200, 200, 404])
        for _ in range(8):
            self.client.post("/v1/pairing/start", json={"device_id": "pet-aaa"})
        self.assertEqual(self.client.post("/v1/pairing/start", json={"device_id": "pet-aaa"}).status_code, 429)

    def test_invalid_bodies_do_not_echo_input(self):
        r = self.client.post("/v1/pairing/claim", json={"device_id": "pet-aaa", "claim_secret": "short-secret"})
        self.assertEqual(r.status_code, 422)
        self.assertNotIn("short-secret", r.text)

    def test_other_account_session_cannot_redeem_foreign_pairing_id(self):
        self.login()
        r = self.parent_post("/v1/parent/pairings/" + "A" * 24 + "/redeem", json={"code": "123456"})
        self.assertEqual(r.status_code, 404)


class CredentialTests(PairingBase):
    def test_rotation_overlap_and_confirm(self):
        old = self.pair()
        rotated = self.client.post("/v1/devices/pet-aaa/token/rotate", headers=self.auth_header(old))
        self.assertEqual(rotated.status_code, 200)
        new = rotated.json()["token"]
        self.assertEqual(self.device_status("pet-aaa", old), 200)
        self.assertEqual(self.device_status("pet-aaa", new), 200)
        self.now += pairing.ROTATION_OVERLAP - 1
        self.assertEqual(self.device_status("pet-aaa", old), 200)
        self.now += 2
        self.assertEqual(self.device_status("pet-aaa", old), 401)
        self.assertEqual(self.device_status("pet-aaa", new), 200)

    def test_confirm_rotation_revokes_old_token_immediately(self):
        old = self.pair()
        new = self.client.post("/v1/devices/pet-aaa/token/rotate", headers=self.auth_header(old)).json()["token"]
        self.assertEqual(self.client.post("/v1/devices/pet-aaa/token/confirm",
                                          headers=self.auth_header(old)).status_code, 401)
        self.assertEqual(self.client.post("/v1/devices/pet-aaa/token/confirm",
                                          headers=self.auth_header(new)).status_code, 200)
        self.assertEqual(self.device_status("pet-aaa", old), 401)
        self.assertEqual(self.device_status("pet-aaa", new), 200)

    def test_previous_token_cannot_rotate_and_current_survives(self):
        old = self.pair()
        new = self.client.post("/v1/devices/pet-aaa/token/rotate", headers=self.auth_header(old)).json()["token"]
        r = self.client.post("/v1/devices/pet-aaa/token/rotate", headers=self.auth_header(old))
        self.assertEqual(r.status_code, 409)
        self.assertEqual(self.device_status("pet-aaa", new), 200)
        self.assertIn(("token.rotate", "via_previous"),
                      [(x["action"], x["result"]) for x in self.audit_rows()])

    def test_lost_rotation_response_cannot_be_recovered_with_the_previous_token(self):
        """Rotation is deliberately not idempotent: tokens are stored hashed, and handing the new
        token to whoever holds the previous one would let a leaked old token take the device over.
        The device gets an explicit code and the previous token keeps working for the overlap."""
        old = self.pair()
        self.client.post("/v1/devices/pet-aaa/token/rotate", headers=self.auth_header(old))  # reply lost
        retry = self.client.post("/v1/devices/pet-aaa/token/rotate", headers=self.auth_header(old))
        self.assertEqual((retry.status_code, retry.json()["detail"]), (409, "ROTATE_WITH_CURRENT_TOKEN"))
        self.assertNotIn("token", retry.json())
        self.assertEqual(self.device_status("pet-aaa", old), 200)  # still usable until the overlap ends
        self.now += pairing.ROTATION_OVERLAP + 1
        self.assertEqual(self.device_status("pet-aaa", old), 401)

    def test_revoked_token_rejected_everywhere(self):
        token = self.pair()
        new = self.client.post("/v1/devices/pet-aaa/token/rotate", headers=self.auth_header(token)).json()["token"]
        self.assertEqual(self.parent_post("/v1/parent/devices/pet-aaa/revoke").status_code, 200)
        for t in (token, new):
            h = self.auth_header(t)
            self.assertEqual(self.client.get("/v1/devices/pet-aaa/snapshot", headers=h).status_code, 401)
            self.assertEqual(self.client.post("/v1/devices/pet-aaa/quiz/start", headers=h).status_code, 401)
            self.assertEqual(self.client.get("/v1/devices/pet-aaa/story/status", headers=h).status_code, 401)
            self.assertEqual(self.client.post("/v1/devices/pet-aaa/token/rotate", headers=h).status_code, 401)
            self.assertEqual(self.client.get("/v1/books/b-1/pages/1",
                                             headers={**h, "X-Device-Id": "pet-aaa"}).status_code, 401)
        self.assertEqual(self.client.get("/v1/parent/devices").json()["devices"][0]["status"], "revoked")
        self.assertIn(("device.revoke", "pet-aaa"),
                      [(r["action"], r["target"]) for r in self.audit_rows()])

    def test_token_for_device_a_cannot_act_as_device_b(self):
        a = self.pair("pet-aaa")
        b = self.pair("pet-bbb")
        self.assertEqual(self.device_status("pet-bbb", a), 401)
        self.assertEqual(self.device_status("pet-aaa", b), 401)
        self.assertEqual(self.client.post("/v1/devices/pet-bbb/token/rotate", headers=self.auth_header(a)).status_code, 401)
        self.assertEqual(self.client.get("/v1/books/b-1/pages/1", headers={
            **self.auth_header(a), "X-Device-Id": "pet-bbb"}).status_code, 401)

    def test_parent_cookie_and_other_routes_need_parent_session(self):
        self.assertEqual(self.client.get("/v1/parent/devices").status_code, 401)
        self.assertEqual(self.client.post("/v1/parent/devices/pet-aaa/revoke").status_code, 401)
        token = self.pair()
        self.assertEqual(self.client.get("/v1/parent/devices", headers=self.auth_header(token)).status_code, 200)
        fresh = TestClient(self.app, base_url="https://testserver")
        self.addCleanup(fresh.close)
        self.assertEqual(fresh.get("/v1/parent/devices", headers=self.auth_header(token)).status_code, 401)

    def test_revoke_requires_csrf_and_known_device(self):
        self.pair()
        self.assertEqual(self.client.post("/v1/parent/devices/pet-aaa/revoke").status_code, 403)
        self.assertEqual(self.parent_post("/v1/parent/devices/pet-nope/revoke").status_code, 404)

    def test_last_used_ip_recorded(self):
        token = self.pair()
        self.now += 120
        self.device_status("pet-aaa", token)
        dev = self.client.get("/v1/parent/devices").json()["devices"][0]
        self.assertEqual(dev["last_seen_at"], self.now)
        self.assertEqual(dev["last_seen_ip"], "testclient")

    def test_bad_tokens_throttled_but_correct_token_never_refused(self):
        token = self.pair()
        codes = [self.client.post("/v1/devices/pet-aaa/token/confirm",
                                  headers=self.auth_header("x" * 40)).status_code for _ in range(22)]
        self.assertEqual(codes[:20], [401] * 20)
        self.assertEqual(codes[20:], [429, 429])
        self.assertEqual(self.client.post("/v1/devices/pet-aaa/token/confirm",
                                          headers=self.auth_header(token)).status_code, 200)
        self.assertEqual(self.client.post("/v1/devices/pet-aaa/token/rotate",
                                          headers=self.auth_header(token)).status_code, 200)
        with connect(self.path) as db:  # capped: no unbounded growth once limited
            n = db.execute("SELECT COUNT(*) FROM pairing_attempts WHERE scope='devauth_ip'").fetchone()[0]
        self.assertEqual(n, 20)

    def test_bad_tokens_counted_per_device_across_ips(self):
        self.pair()
        for i in range(20):
            with self.assertRaises(pairing.PairingError) as e:
                self.app.state.pairing.confirm_rotation("pet-aaa", "y" * 40, f"10.0.0.{i}")
            self.assertEqual(e.exception.status, 401)
        with self.assertRaises(pairing.PairingError) as e:
            self.app.state.pairing.confirm_rotation("pet-aaa", "y" * 40, "10.0.1.1")
        self.assertEqual(e.exception.status, 429)

    def test_all_device_and_voice_routes_reject_revoked_token(self):
        token = self.pair()
        self.parent_post("/v1/parent/devices/pet-aaa/revoke")
        h = self.auth_header(token)
        checked = 0
        for path, methods in self.app.openapi()["paths"].items():
            if not (path.startswith("/v1/devices/") or path.startswith("/v1/voice/")
                    or path.startswith("/v1/books/")):
                continue
            url = path.replace("{device_id}", "pet-aaa").replace("{book_id}", "bk-001").replace("{page}", "1")
            for method in methods:
                r = self.client.request(method.upper(), url, headers={**h, "X-Device-Id": "pet-aaa"}, json={})
                self.assertEqual(r.status_code, 401, f"{method} {path}")
                checked += 1
        self.assertGreaterEqual(checked, 12)

    def test_code_hash_is_salted_and_peppered(self):
        pid, started = self.start_pairing()
        code = started.json()["code"]
        with connect(self.path) as db:
            row = db.execute("SELECT code_salt, code_sha FROM pairing_codes").fetchone()
        self.assertTrue(row["code_salt"])
        self.assertNotEqual(row["code_sha"], pairing.digest(f"{pid}:{code}"))
        self.assertNotEqual(row["code_sha"], pairing.digest(f"{row['code_salt']}:{pid}:{code}"))

    def test_unparsable_forwarded_for_from_trusted_proxy_logs_warning(self):
        settings = Settings(totp_key=self.app.state.settings.totp_key, trusted_proxy="testclient")
        self.app.state.settings = settings
        with self.assertLogs("services.pet_learning.pairing", "WARNING"):
            self.client.post("/v1/pairing/start", json={"device_id": "pet-aaa"},
                             headers={"X-Forwarded-For": "not-an-ip"})


class MigrationTests(unittest.TestCase):
    def test_s0_database_upgrades_and_cli_token_keeps_working(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "s0.sqlite3"
            from services.pet_learning.db import MIGRATIONS
            import shutil
            old = Path(tmp) / "m"
            old.mkdir()
            for name in ("001_baseline.sql", "002_parent_auth.sql"):
                shutil.copy(MIGRATIONS / name, old / name)
            migrate(path, directory=old)
            token = "t" * 40
            with closing(sqlite3.connect(path)) as db:
                db.execute("INSERT INTO devices(device_id,token_sha,seq) VALUES('pet-old',?,7)",
                           (pairing.digest(token),))
                db.commit()
                self.assertEqual(db.execute("PRAGMA user_version").fetchone()[0], 2)
            migrate(path)
            with closing(sqlite3.connect(path)) as db:
                row = db.execute("SELECT status,seq,enrolled_at,prev_token_sha FROM devices").fetchone()
                self.assertGreaterEqual(db.execute("PRAGMA user_version").fetchone()[0], 3)
            self.assertEqual(row, ("active", 7, None, None))
            self.assertTrue(pairing.verify_token(path, "pet-old", token))
            self.assertFalse(pairing.verify_token(path, "pet-old", "u" * 40))


if __name__ == "__main__":
    unittest.main()
