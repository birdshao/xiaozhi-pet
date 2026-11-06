"""Persistent auth, API isolation, CSRF, throttling and privacy contracts."""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import io

from cryptography.fernet import Fernet
from fastapi.testclient import TestClient

from services.pet_learning.api import create_app
from services.pet_learning.auth import (AuthError, CSRF_COOKIE, SESSION_COOKIE,
                                       ParentAuth, password_hash, password_matches)
from services.pet_learning.config import Settings
from services.pet_learning.db import connect
from services.pet_learning.totp import code_at, new_secret
from tools import pet_learning_admin

PASSWORD = "correct-test-password-1234"


class ParentAuthTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name) / "auth.sqlite3"
        self.now = 1800000000
        self.settings = Settings(totp_key=Fernet.generate_key().decode())
        self.auth = ParentAuth(self.path, self.settings, clock=lambda: self.now)
        self.secret = new_secret()
        self.recovery = self.auth.create_account("parent", PASSWORD, self.secret, code_at(self.secret, self.now))
        self.now += 30
        self.app = create_app(self.path, settings=self.settings)
        self.app.state.parent_auth = self.auth
        self.client = TestClient(self.app, base_url="https://testserver")
        self.addCleanup(self.client.close)

    def login(self, **changes):
        body = dict(username="parent", password=PASSWORD, otp=code_at(self.secret, self.now))
        body.update(changes)
        return self.client.post("/v1/parent/login", json=body)

    def test_scrypt_salted_and_secrets_encrypted(self):
        a, b = password_hash(PASSWORD), password_hash(PASSWORD)
        self.assertNotEqual(a, b)
        self.assertTrue(password_matches(PASSWORD, a))
        self.assertFalse(password_matches("wrong", a))
        with connect(self.path) as db:
            dump = "\n".join(db.iterdump())
        for secret in [PASSWORD, self.secret, *self.recovery]:
            self.assertNotIn(secret, dump)
        with self.assertRaisesRegex(ValueError, "already exists"):
            self.auth.create_account("another", PASSWORD, self.secret, code_at(self.secret, self.now))
        with self.assertRaises(ValueError):
            ParentAuth(self.path, Settings(totp_key=Fernet.generate_key().decode()))
        with self.assertRaises(ValueError):
            ParentAuth(self.path, Settings())

    def test_cookie_csrf_logout_and_revocation_survive_restart(self):
        result = self.login()
        self.assertEqual(result.status_code, 200)
        cookie = result.headers.get_list("set-cookie")[0]
        for flag in ("HttpOnly", "Secure", "SameSite=strict", "Path=/"):
            self.assertIn(flag, cookie)
        self.assertNotIn("Domain=", cookie)
        token = self.client.cookies.get(SESSION_COOKIE)
        csrf = result.json()["csrf_token"]
        restarted = ParentAuth(self.path, self.settings, clock=lambda: self.now)
        self.assertEqual(restarted.session(token), 1)
        with connect(self.path) as db:
            dump = "\n".join(db.iterdump())
        self.assertNotIn(token, dump)
        self.assertNotIn(csrf, dump)
        self.assertEqual(self.client.post("/v1/parent/logout").status_code, 403)
        self.assertEqual(self.client.post("/v1/parent/logout", headers={"X-CSRF-Token": "wrong"}).status_code, 403)
        self.assertEqual(self.client.post("/v1/parent/logout", headers={"X-CSRF-Token": csrf}).status_code, 200)
        with self.assertRaises(AuthError):
            restarted.session(token)
        self.assertEqual(self.client.post("/v1/parent/logout").status_code, 401)

    def test_csrf_cookie_must_match_and_be_session_bound(self):
        first = self.login().json()["csrf_token"]
        self.now += 30
        second = self.login().json()["csrf_token"]
        self.assertNotEqual(first, second)
        self.client.cookies.set(CSRF_COOKIE, first, domain="testserver.local", path="/")
        self.assertEqual(self.client.post("/v1/parent/logout", headers={"X-CSRF-Token": first}).status_code, 403)

    def test_idle_and_absolute_expiration(self):
        token, _ = self.auth.login("parent", PASSWORD, code_at(self.secret, self.now), "ip-a")
        self.now += 3599
        self.assertEqual(self.auth.session(token), 1)
        self.now += 3600
        with self.assertRaises(AuthError):
            self.auth.session(token)
        token, _ = self.auth.login("parent", PASSWORD, code_at(self.secret, self.now), "ip-a")
        start = self.now
        for _ in range(24):
            self.now += 1799
            self.assertEqual(self.auth.session(token), 1)
        self.now = start + 12 * 3600
        with self.assertRaises(AuthError):
            self.auth.session(token)

    def test_totp_required_and_cannot_be_replayed_after_restart(self):
        self.assertEqual(self.login(otp="badbad").status_code, 422)
        self.assertEqual(self.login(otp=code_at(self.secret, self.now + 90)).status_code, 401)
        self.assertEqual(self.login().status_code, 200)
        restarted = ParentAuth(self.path, self.settings, clock=lambda: self.now)
        with self.assertRaises(AuthError):
            restarted.login("parent", PASSWORD, code_at(self.secret, self.now), "new-ip")

    def test_recovery_code_requires_password_and_is_one_time_even_concurrently(self):
        self.assertEqual(self.login(password="wrong", otp=self.recovery[0]).status_code, 401)
        def attempt(ip):
            try:
                self.auth.login("parent", PASSWORD, self.recovery[0], ip)
                return True
            except AuthError:
                return False
        with ThreadPoolExecutor(max_workers=2) as pool:
            results = list(pool.map(attempt, ("ip-1", "ip-2")))
        self.assertEqual(sorted(results), [False, True])

    def test_ip_limit_counts_unknown_users_and_ignores_untrusted_forwarding(self):
        for index in range(5):
            response = self.client.post("/v1/parent/login", headers={"X-Forwarded-For": f"192.0.2.{index}"},
                                        json={"username": "unknown", "password": PASSWORD, "otp": "123456"})
            self.assertEqual(response.status_code, 401)
        self.app.state.parent_auth = ParentAuth(self.path, self.settings, clock=lambda: self.now)
        self.assertEqual(self.login().status_code, 429)
        self.now += 900
        self.assertEqual(self.login().status_code, 200)

    def test_account_lock_across_ips_and_restart_then_unlock(self):
        for index in range(5):
            with self.assertRaises(AuthError) as error:
                self.auth.login("parent", "wrong", "123456", f"ip-{index}")
            self.assertEqual(error.exception.status, 401)
        restarted = ParentAuth(self.path, self.settings, clock=lambda: self.now)
        with self.assertRaises(AuthError) as error:
            restarted.login("parent", PASSWORD, code_at(self.secret, self.now), "fresh-ip")
        self.assertEqual(error.exception.status, 429)
        self.now += 900
        token, _ = restarted.login("parent", PASSWORD, code_at(self.secret, self.now), "fresh-ip")
        self.assertEqual(restarted.session(token), 1)
        with connect(self.path) as db:
            self.assertEqual(db.execute("SELECT failed_attempts,locked_until FROM parent_accounts").fetchone()[:], (0, 0))
            self.assertEqual(db.execute("SELECT COUNT(*) FROM audit_log WHERE action='parent.lock'").fetchone()[0], 1)

    def test_audit_actions_new_ip_once_and_no_credentials(self):
        self.assertEqual(self.login(password="wrong").status_code, 401)
        csrf = self.login().json()["csrf_token"]
        token = self.client.cookies.get(SESSION_COOKIE)
        self.client.post("/v1/parent/logout", headers={"X-CSRF-Token": csrf})
        self.now += 30
        self.login()
        with connect(self.path) as db:
            rows = [dict(r) for r in db.execute("SELECT * FROM audit_log")]
        self.assertEqual([r["action"] for r in rows].count("parent.login_new_ip"), 1)
        self.assertTrue({"parent.create", "parent.login", "parent.logout"} <= {r["action"] for r in rows})
        self.assertIn("failure", {r["result"] for r in rows})
        for secret in (PASSWORD, self.secret, token, csrf, self.recovery[0]):
            self.assertNotIn(secret, str(rows))

    def test_identity_isolation_and_origin_validation(self):
        self.app.state.pet_store.enroll_device("pet-001", "d" * 40)
        self.assertEqual(self.client.post("/v1/parent/logout", headers={"Authorization": "Bearer " + "d" * 40}).status_code, 401)
        self.login()
        for path in ("snapshot", "intent"):
            response = (self.client.get("/v1/devices/pet-001/" + path) if path == "snapshot"
                        else self.client.post("/v1/devices/pet-001/intent", json={"text": "讲故事"}))
            self.assertEqual(response.status_code, 401)
        self.assertEqual(self.client.post("/v1/parent/login", headers={"Origin": "https://evil.example"},
                                         json={"username": "parent", "password": PASSWORD, "otp": "123456"}).status_code, 403)

    def test_validation_does_not_echo_secrets(self):
        response = self.login(password=PASSWORD * 30, otp=self.secret)
        self.assertEqual(response.status_code, 422)
        self.assertNotIn(PASSWORD, response.text)
        self.assertNotIn(self.secret, response.text)

    def test_trusted_proxy_single_ip_only(self):
        self.app.state.settings = Settings(trusted_proxy="testclient")  # Test transport peer name.
        self.client.post("/v1/parent/login", headers={"X-Forwarded-For": "192.0.2.7"},
                         json={"username": "parent", "password": PASSWORD, "otp": code_at(self.secret, self.now)})
        with connect(self.path) as db:
            self.assertEqual(db.execute("SELECT ip FROM parent_login_ips").fetchone()[0], "192.0.2.7")

    def test_cli_creates_confirmed_account_without_secret_arguments(self):
        other = Path(self.tmp.name) / "cli.sqlite3"
        secret = new_secret()
        # Use actual wall clock for CLI, while deterministic tests above inject a clock.
        import time
        output = io.StringIO()
        with patch.object(Settings, "load", return_value=self.settings), \
                patch.object(pet_learning_admin, "new_secret", return_value=secret), \
                patch.dict("os.environ", {"KID_PET_PARENT_PASSWORD": PASSWORD}), \
                patch.object(pet_learning_admin.getpass, "getpass", return_value=code_at(secret, time.time())), \
                patch("sys.argv", ["admin", "--db", str(other), "create-parent", "--username", "parent"]), \
                patch("sys.stdout", output):
            pet_learning_admin.main()
        self.assertIn("PARENT_CREATED", output.getvalue())
        self.assertNotIn(PASSWORD, output.getvalue())
        with connect(other) as db:
            self.assertEqual(db.execute("SELECT COUNT(*) FROM parent_recovery_codes").fetchone()[0], 8)

    def test_bad_binding_does_not_create_account(self):
        other = ParentAuth(Path(self.tmp.name) / "unbound.db", self.settings, clock=lambda: self.now)
        with self.assertRaises(ValueError):
            other.create_account("parent", PASSWORD, self.secret, "invalid")
        with connect(other.filename) as db:
            self.assertEqual(db.execute("SELECT COUNT(*) FROM parent_accounts").fetchone()[0], 0)
