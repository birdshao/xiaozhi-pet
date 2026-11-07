ALTER TABLE devices ADD COLUMN status TEXT NOT NULL DEFAULT 'active' CHECK(status IN ('pending','active','revoked'));
ALTER TABLE devices ADD COLUMN enrolled_at INTEGER;
ALTER TABLE devices ADD COLUMN token_rotated_at INTEGER;
ALTER TABLE devices ADD COLUMN fw_version TEXT;
ALTER TABLE devices ADD COLUMN last_seen_at INTEGER;
ALTER TABLE devices ADD COLUMN last_seen_ip TEXT;
ALTER TABLE devices ADD COLUMN prev_token_sha TEXT;
ALTER TABLE devices ADD COLUMN prev_token_expires_at INTEGER NOT NULL DEFAULT 0;
CREATE TABLE pairing_codes (
    pairing_id TEXT PRIMARY KEY,
    account_id INTEGER NOT NULL REFERENCES parent_accounts(id),
    created_at INTEGER NOT NULL,
    expires_at INTEGER NOT NULL,
    state TEXT NOT NULL CHECK(state IN ('open','code_issued','approved','claimed','done','void','expired')),
    device_id TEXT,
    code_salt TEXT,
    code_sha TEXT,
    claim_sha TEXT,
    starts INTEGER NOT NULL DEFAULT 0,
    attempts INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX pairing_codes_device ON pairing_codes(device_id, state);
CREATE TABLE pairing_attempts (
    scope TEXT NOT NULL,
    key TEXT NOT NULL,
    at INTEGER NOT NULL
);
CREATE INDEX pairing_attempts_scope ON pairing_attempts(scope, key, at);
