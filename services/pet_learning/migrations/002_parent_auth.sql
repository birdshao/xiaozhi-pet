CREATE TABLE parent_accounts (
    id INTEGER PRIMARY KEY CHECK(id = 1),
    username TEXT NOT NULL UNIQUE,
    password_hash TEXT NOT NULL,
    totp_secret_enc TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    last_totp_step INTEGER NOT NULL DEFAULT -1,
    failed_attempts INTEGER NOT NULL DEFAULT 0,
    locked_until INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE parent_recovery_codes (
    account_id INTEGER NOT NULL REFERENCES parent_accounts(id),
    code_sha TEXT PRIMARY KEY
);
CREATE TABLE parent_sessions (
    session_sha TEXT PRIMARY KEY,
    account_id INTEGER NOT NULL REFERENCES parent_accounts(id),
    expires_at INTEGER NOT NULL,
    last_seen_at INTEGER NOT NULL,
    csrf_sha TEXT NOT NULL
);
CREATE TABLE parent_login_attempts (
    ip TEXT NOT NULL,
    at INTEGER NOT NULL
);
CREATE INDEX parent_login_attempts_ip_at ON parent_login_attempts(ip, at);
CREATE TABLE parent_login_ips (
    account_id INTEGER NOT NULL REFERENCES parent_accounts(id),
    ip TEXT NOT NULL,
    PRIMARY KEY(account_id, ip)
);
CREATE TABLE audit_log (
    id INTEGER PRIMARY KEY,
    at INTEGER NOT NULL,
    actor TEXT NOT NULL,
    action TEXT NOT NULL,
    target TEXT NOT NULL,
    result TEXT NOT NULL,
    ip TEXT
);
