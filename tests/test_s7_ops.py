from pathlib import Path
from contextlib import contextmanager
import os
import sqlite3
import time
import subprocess
import sys
import tempfile
import unittest

from cryptography.fernet import Fernet

from services.pet_learning import ops
from services.pet_learning.db import migrate

ROOT = Path(__file__).resolve().parents[1]
MIGRATIONS = ROOT / "services" / "pet_learning" / "migrations"

@contextmanager
def opened(path):
    db = sqlite3.connect(str(path))
    try:
        with db:
            yield db
    finally:
        db.close()


GOOD_XZ = {
    "server": {"auth": {"enabled": True, "allowed_devices": []}, "auth_key": "k" * 40,
               "websocket": "wss://pet.family-home.net:8443/xiaozhi/v1/", "mqtt_gateway": None},
    "delete_audio": True,
    "selected_module": {"Memory": "nomem", "Intent": "nointent"},
    # S6 TTS egress audit: config-driven upstream speech is off or fixed.
    "enable_greeting": False, "end_prompt": {"enable": False}, "enable_stop_tts_notify": False,
    "system_error_response": "现在还不能语音聊天，我们可以先用按键和小宠物玩。",
}


def good_env():
    return {
        "KID_PET_ENV": "production", "KID_PET_PUBLIC_ORIGIN": "https://pet.family-home.net:8443",
        "KID_PET_TRUSTED_PROXY": "192.168.1.1", "KID_PET_TOTP_KEY": Fernet.generate_key().decode(),
        "KID_PET_BACKUP_KEY": Fernet.generate_key().decode(), "XZ_AUTH_KEY": "a" * 40,
        "KID_PET_NOTES_KEY": Fernet.generate_key().decode(),
        "KID_PET_VOICE_TOKEN": "t" * 40, "KID_PET_CHILD_MODE": "1",
        "KID_PET_UPSTREAM_DEVICE_ID": "aa:bb", "KID_PET_BUSINESS_DEVICE_ID": "pet-1",
    }


class BackupRestoreTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.db = self.root / "data" / "pet.sqlite3"
        migrate(self.db)
        with opened(self.db) as db:
            db.execute("INSERT INTO audit_log(at,actor,action,target,result) VALUES (1,'a','b','c','ok')")
        self.content = self.root / "data" / "packages"
        (self.content / "abc").mkdir(parents=True)
        (self.content / "abc" / "pkg.bin").write_bytes(b"payload" * 1000)
        self.key = Fernet.generate_key().decode()
        self.addCleanup(self.tmp.cleanup)

    def test_round_trip_into_fresh_location(self):
        out = ops.backup(self.db, self.content, self.root / "b" / "x.enc", self.key)
        self.assertNotIn(b"payload", out.read_bytes())
        db2, content2 = self.root / "new" / "pet.sqlite3", self.root / "new" / "packages"
        ops.restore(out, db2, content2, self.key)
        with opened(db2) as db:
            self.assertEqual(db.execute("SELECT count(*) FROM audit_log").fetchone()[0], 1)
            self.assertEqual(db.execute("PRAGMA user_version").fetchone()[0], len(list(MIGRATIONS.glob("[0-9][0-9][0-9]_*.sql"))))
        self.assertEqual((content2 / "abc" / "pkg.bin").read_bytes(), b"payload" * 1000)

    def test_restore_guards(self):
        out = ops.backup(self.db, self.content, self.root / "x.enc", self.key)
        with self.assertRaises(ops.OpsError):
            ops.restore(out, self.db, self.content, self.key)  # destination exists
        with self.assertRaises(ops.OpsError):
            ops.restore(out, self.root / "n.db", self.root / "n", Fernet.generate_key().decode())
        with self.assertRaises(ops.OpsError):
            ops.backup(self.db, self.content, out, self.key)  # no overwrite
        ops.restore(out, self.db, self.content, self.key, force=True)
        self.assertTrue(list(self.db.parent.glob("pet.sqlite3.pre-restore-*")))

    def test_corrupt_archive_rejected(self):
        out = ops.backup(self.db, self.content, self.root / "x.enc", self.key)
        data = bytearray(out.read_bytes())
        data[-5] ^= 0xFF
        out.write_bytes(bytes(data))
        with self.assertRaises(ops.OpsError):
            ops.restore(out, self.root / "n.db", self.root / "n", self.key)


class CleanupTests(unittest.TestCase):
    def test_prunes_real_commands_table_from_state_sync_migration(self):
        with tempfile.TemporaryDirectory() as tmp:
            db_path = Path(tmp) / "p.sqlite3"
            migrate(db_path)
            now = 1_800_000_000
            with opened(db_path) as db:
                db.execute("INSERT INTO devices(device_id, token_sha) VALUES ('d', 'x')")
                rows = [(1, "applied", now - 31 * 86400), (2, "rejected", now - 40 * 86400),
                        (3, "applied", now - 29 * 86400), (4, "pending", None)]
                for command_id, state, done in rows:
                    db.execute("INSERT INTO commands(command_id, device_id, seq, kind, payload_json, created_at, "
                               "expires_at, state, completed_at) VALUES (?, 'd', ?, 'k', '{}', 1, 2, ?, ?)",
                               (command_id, command_id, state, done))
            result = ops.cleanup(db_path, now=now)
            self.assertEqual(result["commands"], 2)
            with opened(db_path) as db:
                kept = [r[0] for r in db.execute("SELECT command_id FROM commands ORDER BY command_id")]
                self.assertEqual(kept, [3, 4])
                self.assertEqual(db.execute("SELECT count(*) FROM devices").fetchone()[0], 1)

    def test_retention_and_memory_tables_untouched(self):
        with tempfile.TemporaryDirectory() as tmp:
            db_path = Path(tmp) / "p.sqlite3"
            migrate(db_path)
            now = 1_800_000_000
            with opened(db_path) as db:
                db.executemany("INSERT INTO devices(device_id, token_sha) VALUES (?, 'x')", [("d1",), ("d2",)])
                db.executemany("INSERT INTO memory_current VALUES (?, 1, '{}', 0)", [("d1",), ("d2",)])
                db.executemany("INSERT INTO daily_summaries VALUES (?, ?, 0, 0, 0, 0, 0, 0, 0)",
                               [("d1", 1), ("d1", 2)])
                db.executemany("INSERT INTO audit_log(at,actor,action,target,result) VALUES (?,'a','b','c','ok')",
                               [(now - 366 * 86400,), (now - 364 * 86400,)])
            result = ops.cleanup(db_path, now=now)
            self.assertEqual(result["audit_log"], 1)
            with opened(db_path) as db:
                self.assertEqual(db.execute("SELECT count(*) FROM audit_log").fetchone()[0], 1)
                for table in ("memory_current", "daily_summaries"):
                    self.assertEqual(db.execute(f"SELECT count(*) FROM {table}").fetchone()[0], 2)


class MaintenanceTests(unittest.TestCase):
    def test_prune_restore_copies_only_old_ones(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            db, content = root / "pet.sqlite3", root / "packages"
            old_db, old_dir = root / "pet.sqlite3.pre-restore-1", root / "packages.pre-restore-1"
            new = root / "pet.sqlite3.pre-restore-2"
            for f in (old_db, new):
                f.write_text("x")
            old_dir.mkdir()
            (old_dir / "f").write_text("x")
            past = time.time() - 30 * 86400
            os.utime(old_db, (past, past))
            os.utime(old_dir, (past, past))
            self.assertEqual(ops.prune_restore_copies(db, content, days=14), 2)
            self.assertFalse(old_db.exists() or old_dir.exists())
            self.assertTrue(new.exists())

    def test_rotate_backups_keeps_newest(self):
        with tempfile.TemporaryDirectory() as tmp:
            names = [f"pet-2026010{i}-000000.enc" for i in range(1, 6)]
            for name in names:
                (Path(tmp) / name).write_text("x")
            (Path(tmp) / "other.txt").write_text("x")
            self.assertEqual(ops.rotate_backups(tmp, 2), 3)
            self.assertEqual(sorted(p.name for p in Path(tmp).glob("pet-*.enc")), names[-2:])
            self.assertTrue((Path(tmp) / "other.txt").exists())
            with self.assertRaises(ops.OpsError):
                ops.rotate_backups(tmp, 0)


class ConfigCheckTests(unittest.TestCase):
    def test_good_env_and_each_failure(self):
        self.assertEqual(ops.check_deployment(good_env()), [])
        for key, value in (("KID_PET_ENV", "development"), ("KID_PET_PUBLIC_ORIGIN", "http://pet.example"),
                           ("KID_PET_PUBLIC_ORIGIN", "https://localhost"), ("KID_PET_TRUSTED_PROXY", ""),
                           ("KID_PET_BACKUP_KEY", "weak"), ("KID_PET_NOTES_KEY", ""),
                           ("KID_PET_NOTES_KEY", "weak"), ("XZ_AUTH_KEY", "short"),
                           ("XZ_AUTH_KEY", "change-me" * 6), ("KID_PET_CHILD_MODE", "0"),
                           ("KID_PET_ENABLE_PLAYGROUND", "1"), ("PET_GATEWAY_BIND", "0.0.0.0"),
                           ("KID_PET_TRUSTED_PROXY", "127.0.0.1"), ("KID_PET_TRUSTED_PROXY", "192.168.1.0/24"),
                           ("KID_PET_TRUSTED_PROXY", "10.0.0.1,10.0.0.2")):
            env = {**good_env(), key: value}
            self.assertTrue(ops.check_deployment(env), key)
        env = good_env()
        env["KID_PET_BACKUP_KEY"] = env["KID_PET_TOTP_KEY"]
        self.assertTrue(ops.check_deployment(env))
        for other in ("KID_PET_TOTP_KEY", "KID_PET_BACKUP_KEY"):
            env = good_env()
            env["KID_PET_NOTES_KEY"] = env[other]
            self.assertTrue(any("NOTES_KEY" in p for p in ops.check_deployment(env)), other)

    def test_placeholder_websocket_host_fails_and_lan_override_passes(self):
        env = {**good_env(), "KID_PET_PUBLIC_ORIGIN": "https://pet.example.com:8443"}
        self.assertTrue(any("placeholder" in p for p in ops.check_deployment(env)))
        self.assertTrue(ops.deployment_warnings(env))
        lan = {**env, "XZ_WEBSOCKET_URL": "ws://192.168.1.110:8780/xiaozhi/v1/"}
        self.assertEqual(ops.check_deployment(lan), [])
        self.assertTrue(ops.deployment_warnings(lan))  # origin is still a placeholder: warn only
        for bad in ("ws://pet.family-home.net/x", "ws://pet.example.com/x", "ws://1.2.3.4/x", "https://192.168.1.110/x"):
            self.assertTrue(ops.check_deployment({**lan, "XZ_WEBSOCKET_URL": bad}), bad)
        self.assertEqual(ops.deployment_warnings(good_env()), [])

    def test_env_without_kid_pet_env_is_checked_as_production(self):
        env = good_env()
        del env["KID_PET_ENV"]
        self.assertEqual(ops.check_deployment(env), [])
        self.assertEqual(ops.check_deployment({**env, "KID_PET_TRUSTED_PROXY": "172.28.0.1"}), [])

    @unittest.skipUnless(os.name == "posix", "POSIX permissions")
    def test_permission_warnings(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / ".env"
            path.write_text("A=1\n")
            path.chmod(0o644)
            self.assertTrue(ops.permission_warnings(path, Path(tmp) / "missing"))
            path.chmod(0o600)
            self.assertEqual(ops.permission_warnings(path), [])

    def test_xiaozhi_checklist(self):
        self.assertEqual(ops.check_xiaozhi_config(GOOD_XZ, "https://pet.family-home.net:8443"), [])
        bad = [("server", {**GOOD_XZ["server"], "auth": {"enabled": False}}),
               ("server", {**GOOD_XZ["server"], "auth": {"enabled": True, "allowed_devices": ["x"]}}),
               ("server", {**GOOD_XZ["server"], "websocket": "ws://pet.example/x"}),
               ("selected_module", {"Memory": "mem0ai", "Intent": "nointent"}),
               ("delete_audio", False), ("read_config_from_api", True),
               ("enable_greeting", True), ("end_prompt", {"enable": True}), ("system_error_response", "主人")]
        for key, value in bad:
            self.assertTrue(ops.check_xiaozhi_config({**GOOD_XZ, key: value}), key)
        self.assertTrue(ops.check_xiaozhi_config(GOOD_XZ, "https://other.example"))

    def test_cli_backup_and_check_config(self):
        with tempfile.TemporaryDirectory() as tmp:
            db = Path(tmp) / "p.sqlite3"
            migrate(db)
            env = {"KID_PET_BACKUP_KEY": Fernet.generate_key().decode(), **{k: v for k, v in os.environ.items() if not k.startswith("KID_PET_")}}
            run = subprocess.run([sys.executable, "-m", "tools.pet_learning_admin", "--db", str(db), "backup",
                                  "--out", str(Path(tmp) / "o.enc")], cwd=ROOT, env=env,
                                 capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertIn("BACKUP_WRITTEN", run.stdout)
            rotated = subprocess.run([sys.executable, "-m", "tools.pet_learning_admin", "--db", str(db), "backup",
                                      "--out-dir", str(Path(tmp) / "bk"), "--keep", "1"], cwd=ROOT, env=env,
                                     capture_output=True, text=True)
            self.assertEqual(rotated.returncode, 0, rotated.stderr)
            self.assertEqual(len(list((Path(tmp) / "bk").glob("pet-*.enc"))), 1)
            self.assertNotIn(env["KID_PET_BACKUP_KEY"], run.stdout + run.stderr)


if __name__ == "__main__":
    unittest.main()
