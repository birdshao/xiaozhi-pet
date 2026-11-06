"""Ordered, transactional SQLite migrations, including unversioned P3/P4 DBs."""
from contextlib import contextmanager
from pathlib import Path
import sqlite3

MIGRATIONS = Path(__file__).with_name("migrations")


@contextmanager
def connect(filename):
    db = sqlite3.connect(str(filename), timeout=5, isolation_level=None)
    try:
        db.row_factory = sqlite3.Row
        db.execute("PRAGMA foreign_keys=ON")
        db.execute("PRAGMA busy_timeout=5000")
        with db:
            yield db
    finally:
        db.close()


def migrate(filename, *, directory=MIGRATIONS):
    if str(filename) != ":memory:":
        Path(filename).parent.mkdir(parents=True, exist_ok=True)
    files = sorted(directory.glob("[0-9][0-9][0-9]_*.sql"))
    if [int(p.name[:3]) for p in files] != list(range(1, len(files) + 1)):
        raise ValueError("Migration versions must be contiguous")
    with connect(filename) as db:
        # Recheck under the write lock: concurrent initializers cannot race.
        for version in range(1, len(files) + 2):
            db.execute("BEGIN IMMEDIATE")
            current = db.execute("PRAGMA user_version").fetchone()[0]
            if current > len(files):
                raise ValueError("Database schema is newer than this service")
            if version <= current or version > len(files):
                db.commit()
                continue
            # executescript implicitly commits; execute complete statements instead.
            statement = ""
            for line in files[version - 1].read_text(encoding="utf-8").splitlines(True):
                statement += line
                if sqlite3.complete_statement(statement):
                    db.execute(statement)
                    statement = ""
            if statement.strip():
                raise ValueError("Incomplete migration statement")
            db.execute(f"PRAGMA user_version={version}")
            db.commit()
        db.execute("PRAGMA journal_mode=WAL")
