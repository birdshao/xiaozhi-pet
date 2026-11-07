"""S7 operations: encrypted backup/restore, retention cleanup, deployment checks.

Standard library plus Fernet (already a dependency). No HTTP surface; driven by
tools/pet_learning_admin.py. Nothing here logs or prints secrets or child data.
"""
from __future__ import annotations

import ipaddress
import os
import shutil
import sqlite3
import tarfile
import tempfile
import time
from pathlib import Path

from cryptography.fernet import Fernet, InvalidToken

from .config import Settings, read_env
from .db import MIGRATIONS, connect
from .xz_checklist import _weak, check_xiaozhi_config  # noqa: F401  (re-exported)

MAGIC = b"PETBK1\n"
CHUNK = 1 << 20
DB_MEMBER = "pet_learning.sqlite3"
CONTENT_MEMBER = "packages"
DAY = 86400

# Explicit allow-list (D6). Memory tables (memory_current, daily_summaries, likes,
# growth, recent books, parent notes) are never listed, so cleanup cannot touch them.
# (table, time column, max age in days, extra WHERE). Tables that do not exist yet
# (later stages) are skipped.
RETENTION = (
    ("audit_log", "at", 365, "1=1"),
    ("commands", "completed_at", 30, "completed_at IS NOT NULL"),
    ("moderation_stats", "day_start", 90, "1=1"),
)
EPHEMERAL = (
    ("parent_sessions", "expires_at", "expires_at < :now"),
    ("parent_login_attempts", "at", "at < :now - 86400"),
)


class OpsError(ValueError):
    pass


def fernet_from(key: str, name: str) -> Fernet:
    try:
        return Fernet(key.encode("ascii"))
    except (ValueError, UnicodeError):
        raise OpsError(f"{name} must be a Fernet key") from None


def _encrypt_stream(src, dst, fernet):
    dst.write(MAGIC)
    while chunk := src.read(CHUNK):
        token = fernet.encrypt(chunk)
        dst.write(len(token).to_bytes(4, "big"))
        dst.write(token)


def _decrypt_stream(src, dst, fernet):
    if src.read(len(MAGIC)) != MAGIC:
        raise OpsError("Not a PetLearning backup archive")
    while header := src.read(4):
        size = int.from_bytes(header, "big")
        token = src.read(size)
        if len(token) != size or size > 2 * CHUNK:
            raise OpsError("Backup archive is truncated or corrupt")
        try:
            dst.write(fernet.decrypt(token))
        except InvalidToken:
            raise OpsError("Wrong backup key or corrupt archive") from None


def backup(db_path, content_dir, out_path, key: str) -> Path:
    """Online SQLite snapshot (sqlite3.backup) + content dir -> encrypted archive."""
    fernet = fernet_from(key, "KID_PET_BACKUP_KEY")
    db_path, out_path = Path(db_path), Path(out_path)
    if not db_path.is_file():
        raise OpsError("Database file not found")
    if out_path.exists():
        raise OpsError("Refusing to overwrite an existing backup file")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=out_path.parent) as tmp:
        snapshot = Path(tmp) / DB_MEMBER
        source = sqlite3.connect(str(db_path), timeout=5)
        target = sqlite3.connect(str(snapshot))
        try:
            source.backup(target)
        finally:
            target.close()
            source.close()
        archive = Path(tmp) / "backup.tar"
        with tarfile.open(archive, "w") as tar:
            tar.add(snapshot, arcname=DB_MEMBER)
            content = Path(content_dir)
            if content.is_dir():
                for path in sorted(content.rglob("*")):
                    if path.is_symlink():
                        continue
                    tar.add(path, arcname=f"{CONTENT_MEMBER}/{path.relative_to(content).as_posix()}",
                            recursive=False)
        partial = Path(tmp) / "backup.enc"
        with archive.open("rb") as src, partial.open("wb") as dst:
            _encrypt_stream(src, dst, fernet)
        try:
            os.chmod(partial, 0o600)
        except OSError:
            pass
        os.replace(partial, out_path)
    return out_path


def _safe_extract(archive: Path, dest: Path):
    with tarfile.open(archive, "r") as tar:
        for member in tar:
            name = member.name
            parts = Path(name).parts
            allowed = name == DB_MEMBER or (parts and parts[0] == CONTENT_MEMBER)
            if (not allowed or name.startswith("/") or ".." in parts
                    or not (member.isreg() or member.isdir())):
                raise OpsError("Backup archive contains an unexpected entry")
            target = dest / name
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            with tar.extractfile(member) as src, target.open("wb") as dst:
                shutil.copyfileobj(src, dst)


def restore(archive_path, db_path, content_dir, key: str, *, force=False):
    fernet = fernet_from(key, "KID_PET_BACKUP_KEY")
    db_path, content_dir = Path(db_path), Path(content_dir)
    if (db_path.exists() or content_dir.exists()) and not force:
        raise OpsError("Destination exists; pass --force to move it aside and restore")
    db_path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=db_path.parent) as tmp:
        tar_path = Path(tmp) / "backup.tar"
        with Path(archive_path).open("rb") as src, tar_path.open("wb") as dst:
            _decrypt_stream(src, dst, fernet)
        staged = Path(tmp) / "staged"
        staged.mkdir()
        _safe_extract(tar_path, staged)
        staged_db = staged / DB_MEMBER
        if not staged_db.is_file():
            raise OpsError("Backup archive has no database")
        check = sqlite3.connect(str(staged_db))
        try:
            if check.execute("PRAGMA integrity_check").fetchone()[0] != "ok":
                raise OpsError("Restored database failed integrity_check")
            version = check.execute("PRAGMA user_version").fetchone()[0]
        finally:
            check.close()
        if version > len(list(MIGRATIONS.glob("[0-9][0-9][0-9]_*.sql"))):
            raise OpsError("Backup schema is newer than this service")
        stamp = time.strftime("%Y%m%d%H%M%S")
        for suffix in ("", "-wal", "-shm"):
            old = Path(str(db_path) + suffix)
            if old.exists():
                os.replace(old, Path(str(db_path) + f".pre-restore-{stamp}{suffix}"))
        if content_dir.exists():
            os.replace(content_dir, content_dir.with_name(content_dir.name + f".pre-restore-{stamp}"))
        os.replace(staged_db, db_path)
        staged_content = staged / CONTENT_MEMBER
        if staged_content.is_dir():
            shutil.move(str(staged_content), str(content_dir))
        else:
            content_dir.mkdir(parents=True, exist_ok=True)


def _has_columns(db, table, *columns):
    found = {row["name"] for row in db.execute(f"PRAGMA table_info({table})")}
    return bool(found) and set(columns) <= found


def cleanup(db_path, *, now=None) -> dict:
    """Delete rows past their retention period. Returns {table: rows_deleted}."""
    now = int(time.time() if now is None else now)
    result = {}
    with connect(db_path) as db:
        for table, column, days, where in RETENTION:
            if not _has_columns(db, table, column):
                continue
            cursor = db.execute(f"DELETE FROM {table} WHERE {column} < ? AND ({where})",
                                (now - days * DAY,))
            result[table] = cursor.rowcount
        for table, column, where in EPHEMERAL:
            if not _has_columns(db, table, column):
                continue
            result[table] = db.execute(f"DELETE FROM {table} WHERE {where}", {"now": now}).rowcount
    return result


def check_deployment(env: dict) -> list[str]:
    """Return human-readable problems with a production .env (empty = OK)."""
    problems = []
    if env.get("KID_PET_ENV") != "production":
        problems.append("KID_PET_ENV must be production")
    try:
        Settings(
            production=True,
            public_origin=env.get("KID_PET_PUBLIC_ORIGIN", ""),
            trusted_proxy=env.get("KID_PET_TRUSTED_PROXY", ""),
            totp_key=env.get("KID_PET_TOTP_KEY", ""),
        ).validate()
    except ValueError as error:
        problems.append(str(error))
    for name in ("KID_PET_TOTP_KEY", "KID_PET_BACKUP_KEY"):
        try:
            fernet_from(env.get(name, ""), name)
        except OpsError as error:
            problems.append(str(error))
    if env.get("KID_PET_BACKUP_KEY") and env.get("KID_PET_BACKUP_KEY") == env.get("KID_PET_TOTP_KEY"):
        problems.append("KID_PET_BACKUP_KEY must differ from KID_PET_TOTP_KEY")
    for name in ("XZ_AUTH_KEY", "KID_PET_DEVICE_TOKEN"):
        value = env.get(name, "")
        if len(value) < 32 or _weak(value):
            problems.append(f"{name} must be a private random value of at least 32 characters")
    if env.get("KID_PET_CHILD_MODE") != "1":
        problems.append("KID_PET_CHILD_MODE must be 1")
    for name in ("KID_PET_UPSTREAM_DEVICE_ID", "KID_PET_BUSINESS_DEVICE_ID"):
        if not env.get(name):
            problems.append(f"{name} is required")
    if env.get("KID_PET_ENABLE_PLAYGROUND") == "1":
        problems.append("KID_PET_ENABLE_PLAYGROUND must not be 1 in production")
    try:
        port = int(env.get("PET_GATEWAY_PORT", "8780"))
        if not 1024 <= port <= 65535:
            raise ValueError
    except ValueError:
        problems.append("PET_GATEWAY_PORT must be 1024-65535")
    bind = env.get("PET_GATEWAY_BIND", "127.0.0.1")
    try:
        if ipaddress.ip_address(bind).is_unspecified:
            problems.append("PET_GATEWAY_BIND must be 127.0.0.1 or the VM LAN address, not 0.0.0.0")
    except ValueError:
        problems.append("PET_GATEWAY_BIND must be an IP address")
    return problems


def check_env_file(path) -> list[str]:
    return check_deployment({**read_env(path), **os.environ})
