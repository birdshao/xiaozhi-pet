"""Schema upgrade and rollback contracts, without hardware or network."""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import sqlite3
import tempfile
import unittest

from services.pet_learning.db import MIGRATIONS, connect, migrate
from services.pet_learning.store import PetStore
from services.pet_learning.quiz import QuizEngine
from services.pet_learning.story_reader import StoryReader


class MigrationTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name) / "pet.sqlite3"

    def test_empty_database_and_repeated_concurrent_initializers(self):
        with ThreadPoolExecutor(max_workers=4) as pool:
            list(pool.map(lambda _: migrate(self.path), range(8)))
        for constructor in (PetStore, QuizEngine, StoryReader):
            constructor(self.path)
        with connect(self.path) as db:
            self.assertEqual(db.execute("PRAGMA user_version").fetchone()[0], len(list(MIGRATIONS.glob("[0-9][0-9][0-9]_*.sql"))))
            self.assertEqual(db.execute("PRAGMA journal_mode").fetchone()[0], "wal")
            self.assertEqual(db.execute("PRAGMA foreign_key_check").fetchall(), [])

    def test_unversioned_legacy_rows_and_receipts_survive(self):
        with connect(self.path) as db:
            db.executescript((MIGRATIONS / "001_baseline.sql").read_text(encoding="utf-8"))
            db.execute("INSERT INTO devices(device_id,token_sha,seq,version) VALUES('pet-001','sha',1,1)")
            db.execute("INSERT INTO event_log VALUES('pet-001','msg-001',1,'payload',1)")
            db.execute("INSERT INTO books VALUES('book-001','title')")
            db.execute("INSERT INTO pages VALUES('book-001',1,'reviewed',1)")
            db.execute("INSERT INTO quiz_sessions VALUES('pet-001','card',1,2,3)")
            db.execute("INSERT INTO story_reader_sessions VALUES('pet-001','book-001',1,2)")
            tables = ['devices', 'event_log', 'books', 'pages', 'quiz_sessions', 'story_reader_sessions']
            before = {t: [tuple(r) for r in db.execute(f"SELECT * FROM {t}")] for t in tables}
        migrate(self.path)
        migrate(self.path)
        with connect(self.path) as db:
            after = {t: [tuple(r) for r in db.execute(f"SELECT * FROM {t}")] for t in tables}
        # Later migrations append columns to devices; the S0 columns must be untouched.
        after["devices"] = [row[:len(before["devices"][0])] for row in after["devices"]]
        self.assertEqual(before, after)

    def test_version_one_upgrade(self):
        with connect(self.path) as db:
            db.executescript((MIGRATIONS / "001_baseline.sql").read_text(encoding="utf-8"))
            db.execute("PRAGMA user_version=1")
        migrate(self.path)
        with connect(self.path) as db:
            self.assertEqual(db.execute("SELECT COUNT(*) FROM parent_accounts").fetchone()[0], 0)

    def test_future_schema_refused_without_changes(self):
        with connect(self.path) as db:
            db.execute("PRAGMA user_version=99")
        for constructor in (migrate, PetStore, QuizEngine, StoryReader):
            with self.assertRaisesRegex(ValueError, "newer"):
                constructor(self.path)
        with connect(self.path) as db:
            self.assertEqual(db.execute("PRAGMA user_version").fetchone()[0], 99)
            self.assertEqual(db.execute("SELECT name FROM sqlite_master").fetchall(), [])

    def test_failed_migration_rolls_back_schema_data_and_version(self):
        directory = Path(self.tmp.name) / "migrations"
        directory.mkdir()
        (directory / "001_ok.sql").write_text("CREATE TABLE stable(value TEXT);\n", encoding="utf-8")
        (directory / "002_fail.sql").write_text(
            "CREATE TABLE transient(value TEXT);\nINSERT INTO stable VALUES('bad');\nINVALID SQL;\n", encoding="utf-8")
        with self.assertRaises(sqlite3.OperationalError):
            migrate(self.path, directory=directory)
        with connect(self.path) as db:
            self.assertEqual(db.execute("PRAGMA user_version").fetchone()[0], 1)
            self.assertEqual(db.execute("SELECT * FROM stable").fetchall(), [])
            self.assertIsNone(db.execute("SELECT name FROM sqlite_master WHERE name='transient'").fetchone())
        (directory / "002_fail.sql").write_text("INSERT INTO stable VALUES('good');\n", encoding="utf-8")
        migrate(self.path, directory=directory)
        with connect(self.path) as db:
            self.assertEqual(db.execute("SELECT value FROM stable").fetchone()[0], "good")
