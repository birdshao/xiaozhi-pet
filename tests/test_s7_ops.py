from pathlib import Path
from contextlib import contextmanager
import sqlite3
import subprocess
import sys
import tempfile
import unittest

from cryptography.fernet import Fernet

from services.pet_learning import ops
from services.pet_learning.db import migrate

ROOT = Path(__file__).resolve().parents[1]
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
               "websocket": "wss://pet.example:8443/xiaozhi/v1/", "mqtt_gateway": None},
    "delete_audio": True,
    "selected_module": {"Memory": "nomem", "Intent": "nointent"},
}


def good_env():
    return {
        "KID_PET_ENV": "production", "KID_PET_PUBLIC_ORIGIN": "https://pet.example:8443",
        "KID_PET_TRUSTED_PROXY": "192.168.1.1", "KID_PET_TOTP_KEY": Fernet.generate_key().decode(),
        "KID_PET_BACKUP_KEY": Fernet.generate_key().decode(), "XZ_AUTH_KEY": "a" * 40,
        "KID_PET_DEVICE_TOKEN": "t" * 40, "KID_PET_CHILD_MODE": "1",
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
            self.assertEqual(db.execute("PRAGMA user_version").fetchone()[0], 2)
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
    def test_retention_and_memory_tables_untouched(self):
        with tempfile.TemporaryDirectory() as tmp:
            db_path = Path(tmp) / "p.sqlite3"
            migrate(db_path)
            now = 1_800_000_000
            with opened(db_path) as db:
                db.execute("CREATE TABLE memory_current(device_id TEXT, at INTEGER)")
                db.execute("CREATE TABLE daily_summaries(device_id TEXT, at INTEGER)")
                db.executemany("INSERT INTO memory_current VALUES ('d',?)", [(1,), (now,)])
                db.executemany("INSERT INTO daily_summaries VALUES ('d',?)", [(1,), (now,)])
                db.executemany("INSERT INTO audit_log(at,actor,action,target,result) VALUES (?,'a','b','c','ok')",
                               [(now - 366 * 86400,), (now - 364 * 86400,)])
            result = ops.cleanup(db_path, now=now)
            self.assertEqual(result["audit_log"], 1)
            with opened(db_path) as db:
                self.assertEqual(db.execute("SELECT count(*) FROM audit_log").fetchone()[0], 1)
                for table in ("memory_current", "daily_summaries"):
                    self.assertEqual(db.execute(f"SELECT count(*) FROM {table}").fetchone()[0], 2)


class ConfigCheckTests(unittest.TestCase):
    def test_good_env_and_each_failure(self):
        self.assertEqual(ops.check_deployment(good_env()), [])
        for key, value in (("KID_PET_ENV", "development"), ("KID_PET_PUBLIC_ORIGIN", "http://pet.example"),
                           ("KID_PET_PUBLIC_ORIGIN", "https://localhost"), ("KID_PET_TRUSTED_PROXY", ""),
                           ("KID_PET_BACKUP_KEY", "weak"), ("XZ_AUTH_KEY", "short"),
                           ("XZ_AUTH_KEY", "change-me" * 6), ("KID_PET_CHILD_MODE", "0"),
                           ("KID_PET_ENABLE_PLAYGROUND", "1"), ("PET_GATEWAY_BIND", "0.0.0.0")):
            env = {**good_env(), key: value}
            self.assertTrue(ops.check_deployment(env), key)
        env = good_env()
        env["KID_PET_BACKUP_KEY"] = env["KID_PET_TOTP_KEY"]
        self.assertTrue(ops.check_deployment(env))

    def test_xiaozhi_checklist(self):
        self.assertEqual(ops.check_xiaozhi_config(GOOD_XZ, "https://pet.example:8443"), [])
        bad = [("server", {**GOOD_XZ["server"], "auth": {"enabled": False}}),
               ("server", {**GOOD_XZ["server"], "auth": {"enabled": True, "allowed_devices": ["x"]}}),
               ("server", {**GOOD_XZ["server"], "websocket": "ws://pet.example/x"}),
               ("selected_module", {"Memory": "mem0ai", "Intent": "nointent"}),
               ("delete_audio", False), ("read_config_from_api", True)]
        for key, value in bad:
            self.assertTrue(ops.check_xiaozhi_config({**GOOD_XZ, key: value}), key)
        self.assertTrue(ops.check_xiaozhi_config(GOOD_XZ, "https://other.example"))

    def test_cli_backup_and_check_config(self):
        with tempfile.TemporaryDirectory() as tmp:
            db = Path(tmp) / "p.sqlite3"
            migrate(db)
            env = {"KID_PET_BACKUP_KEY": Fernet.generate_key().decode(), "PATH": "", "SYSTEMROOT": ""}
            run = subprocess.run([sys.executable, "-m", "tools.pet_learning_admin", "--db", str(db), "backup",
                                  "--out", str(Path(tmp) / "o.enc")], cwd=ROOT, env=env,
                                 capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertIn("BACKUP_WRITTEN", run.stdout)
            self.assertNotIn(env["KID_PET_BACKUP_KEY"], run.stdout + run.stderr)


if __name__ == "__main__":
    unittest.main()
