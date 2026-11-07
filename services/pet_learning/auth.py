"""Single-parent authentication. Credentials never enter audit records."""
import hashlib
import hmac
import re
import secrets
import time

from cryptography.fernet import InvalidToken

from .db import connect, migrate
from .totp import match_step

SESSION_SECONDS = 12 * 3600
IDLE_SECONDS = 3600
LOGIN_WINDOW = 15 * 60
LOGIN_LIMIT = 5
SESSION_COOKIE = "__Host-pet_parent"
CSRF_COOKIE = "__Host-pet_csrf"


def digest(value):
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def password_hash(password, *, salt=None):
    salt = salt or secrets.token_hex(16)
    key = hashlib.scrypt(password.encode("utf-8"), salt=bytes.fromhex(salt),
                         n=32768, r=8, p=1, dklen=32, maxmem=64 * 1024 * 1024)
    return f"scrypt$32768$8$1${salt}${key.hex()}"


def password_matches(password, encoded):
    return hmac.compare_digest(password_hash(password, salt=encoded.split("$")[4]), encoded)


class AuthError(Exception):
    def __init__(self, status, code):
        self.status = status
        self.code = code
        super().__init__(code)


def audit(db, now, action, result, *, account_id=None, ip=None, actor="parent"):
    db.execute("INSERT INTO audit_log(at,actor,action,target,result,ip) VALUES(?,?,?,?,?,?)",
               (now, actor, action, str(account_id) if account_id else "unknown", result, ip))


class ParentAuth:
    def __init__(self, filename, settings, *, clock=time.time):
        self.filename = filename
        self.settings = settings
        self.clock = clock
        migrate(filename)
        # A fixed dummy salt avoids an account-existence timing shortcut.
        self._dummy_hash = password_hash("dummy-account-password", salt="00" * 16)
        with connect(filename) as db:
            row = db.execute("SELECT totp_secret_enc FROM parent_accounts").fetchone()
        if row:
            try:
                settings.cipher().decrypt(row[0].encode("ascii"))
            except InvalidToken:
                raise ValueError("Parent TOTP key does not match database") from None

    def create_account(self, username, password, secret, confirmation):
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{2,63}", username):
            raise ValueError("Username must be 3-64 ASCII identifier characters")
        if not 12 <= len(password) <= 256:
            raise ValueError("Password must contain 12-256 characters")
        now = int(self.clock())
        step = match_step(secret, confirmation, now)
        if step is None:
            raise ValueError("TOTP binding confirmation failed")
        encoded = password_hash(password)
        encrypted = self.settings.cipher().encrypt(secret.encode("ascii")).decode("ascii")
        recovery = [secrets.token_hex(16) for _ in range(8)]
        with connect(self.filename) as db:
            db.execute("BEGIN IMMEDIATE")
            if db.execute("SELECT 1 FROM parent_accounts").fetchone():
                raise ValueError("A parent administrator already exists")
            db.execute("INSERT INTO parent_accounts(id,username,password_hash,totp_secret_enc,"
                       "created_at,last_totp_step) VALUES(1,?,?,?,?,?)",
                       (username, encoded, encrypted, now, step))
            db.executemany("INSERT INTO parent_recovery_codes VALUES(1,?)",
                           [(digest(code),) for code in recovery])
            audit(db, now, "parent.create", "success", account_id=1, actor="cli")
        return recovery

    def login(self, username, password, otp, ip):
        now = int(self.clock())
        error = None
        with connect(self.filename) as db:
            db.execute("BEGIN IMMEDIATE")
            db.execute("DELETE FROM parent_login_attempts WHERE at<=?", (now - LOGIN_WINDOW,))
            attempts = db.execute("SELECT COUNT(*) FROM parent_login_attempts WHERE ip=?",
                                  (ip,)).fetchone()[0]
            row = db.execute("SELECT * FROM parent_accounts WHERE username=?", (username,)).fetchone()
            account_id = row["id"] if row else None
            if attempts >= LOGIN_LIMIT:
                audit(db, now, "parent.login", "rate_limited", account_id=account_id, ip=ip)
                error = AuthError(429, "PARENT_LOGIN_THROTTLED")
            else:
                db.execute("INSERT INTO parent_login_attempts VALUES(?,?)", (ip, now))
                if row and row["locked_until"] > now:
                    audit(db, now, "parent.login", "locked", account_id=account_id, ip=ip)
                    error = AuthError(429, "PARENT_LOGIN_THROTTLED")
                else:
                    valid_password = password_matches(password, row["password_hash"] if row else self._dummy_hash)
                    step, recovery = None, None
                    if row and valid_password:
                        secret = self.settings.cipher().decrypt(row["totp_secret_enc"].encode("ascii")).decode("ascii")
                        step = match_step(secret, otp, now, last_step=row["last_totp_step"])
                        if step is None:
                            recovery = db.execute("SELECT code_sha FROM parent_recovery_codes "
                                                  "WHERE account_id=? AND code_sha=?",
                                                  (account_id, digest(otp))).fetchone()
                    if not row or not valid_password or (step is None and recovery is None):
                        audit(db, now, "parent.login", "failure", account_id=account_id, ip=ip)
                        error = AuthError(401, "PARENT_AUTH_REQUIRED")
                        if row:
                            failures = (0 if row["locked_until"] else row["failed_attempts"]) + 1
                            locked_until = now + LOGIN_WINDOW if failures >= LOGIN_LIMIT else 0
                            db.execute("UPDATE parent_accounts SET failed_attempts=?,locked_until=? WHERE id=?",
                                       (failures, locked_until, account_id))
                            if locked_until:
                                audit(db, now, "parent.lock", "locked", account_id=account_id, ip=ip)
                    else:
                        if recovery:
                            db.execute("DELETE FROM parent_recovery_codes WHERE code_sha=?", (recovery[0],))
                            audit(db, now, "parent.recovery", "success", account_id=account_id, ip=ip)
                        else:
                            db.execute("UPDATE parent_accounts SET last_totp_step=? WHERE id=?", (step, account_id))
                        db.execute("UPDATE parent_accounts SET failed_attempts=0,locked_until=0 WHERE id=?", (account_id,))
                        db.execute("DELETE FROM parent_sessions WHERE expires_at<=? OR last_seen_at<=?",
                                   (now, now - IDLE_SECONDS))
                        session, csrf = secrets.token_urlsafe(32), secrets.token_urlsafe(32)
                        db.execute("INSERT INTO parent_sessions VALUES(?,?,?,?,?)",
                                   (digest(session), account_id, now + SESSION_SECONDS, now, digest(csrf)))
                        if not db.execute("SELECT 1 FROM parent_login_ips WHERE account_id=? AND ip=?",
                                          (account_id, ip)).fetchone():
                            db.execute("INSERT INTO parent_login_ips VALUES(?,?)", (account_id, ip))
                            audit(db, now, "parent.login_new_ip", "success", account_id=account_id, ip=ip)
                        audit(db, now, "parent.login", "success", account_id=account_id, ip=ip)
            # Failures must commit counters and audit before raising outside this context.
            db.commit()
        if error:
            raise error
        return session, csrf

    def reauth(self, account_id, password, otp, ip):
        """Step-up for sensitive actions (§9): password + a fresh TOTP code, no recovery codes.

        Failures count toward the same account lockout as login; a code already
        used (by login or an earlier step-up) is a replay and is refused.
        """
        now = int(self.clock())
        error = None
        with connect(self.filename) as db:
            db.execute("BEGIN IMMEDIATE")
            row = db.execute("SELECT * FROM parent_accounts WHERE id=?", (account_id,)).fetchone()
            if not row:
                error = AuthError(401, "PARENT_AUTH_REQUIRED")
            elif row["locked_until"] > now:
                audit(db, now, "parent.reauth", "locked", account_id=account_id, ip=ip)
                error = AuthError(429, "PARENT_LOGIN_THROTTLED")
            else:
                step = None
                if password_matches(password, row["password_hash"]):
                    secret = self.settings.cipher().decrypt(row["totp_secret_enc"].encode("ascii")).decode("ascii")
                    step = match_step(secret, otp, now, last_step=row["last_totp_step"])
                if step is None:
                    failures = (0 if row["locked_until"] else row["failed_attempts"]) + 1
                    locked_until = now + LOGIN_WINDOW if failures >= LOGIN_LIMIT else 0
                    db.execute("UPDATE parent_accounts SET failed_attempts=?,locked_until=? WHERE id=?",
                               (failures, locked_until, account_id))
                    audit(db, now, "parent.reauth", "failure", account_id=account_id, ip=ip)
                    error = AuthError(401, "PARENT_REAUTH_FAILED")
                else:
                    db.execute("UPDATE parent_accounts SET last_totp_step=?,failed_attempts=0,locked_until=0 "
                               "WHERE id=?", (step, account_id))
                    audit(db, now, "parent.reauth", "success", account_id=account_id, ip=ip)
            db.commit()
        if error:
            raise error

    def session(self, token, *, csrf=None, csrf_cookie=None, logout=False, ip=None):
        now = int(self.clock())
        if not token or len(token) > 256:
            raise AuthError(401, "PARENT_AUTH_REQUIRED")
        with connect(self.filename) as db:
            db.execute("BEGIN IMMEDIATE")
            row = db.execute("SELECT * FROM parent_sessions WHERE session_sha=?", (digest(token),)).fetchone()
            if not row or row["expires_at"] <= now or row["last_seen_at"] <= now - IDLE_SECONDS:
                raise AuthError(401, "PARENT_AUTH_REQUIRED")
            if logout or csrf is not None:
                if (not csrf or not csrf_cookie or len(csrf) > 256 or len(csrf_cookie) > 256
                        or not hmac.compare_digest(csrf.encode(), csrf_cookie.encode())
                        or not hmac.compare_digest(digest(csrf), row["csrf_sha"])):
                    raise AuthError(403, "PARENT_CSRF_REQUIRED")
            if logout:
                db.execute("DELETE FROM parent_sessions WHERE session_sha=?", (digest(token),))
                audit(db, now, "parent.logout", "success", account_id=row["account_id"], ip=ip)
            else:
                db.execute("UPDATE parent_sessions SET last_seen_at=? WHERE session_sha=?", (now, digest(token)))
            return row["account_id"]
