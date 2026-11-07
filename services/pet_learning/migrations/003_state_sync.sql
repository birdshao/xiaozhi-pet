-- Server S2 (V5-05): device state snapshots, memory projection, commands.
-- Memory tables are kept until the parent clears memory or deletes the
-- device (D6); only commands/audit are purged on a schedule.
CREATE TABLE device_state (
    device_id TEXT PRIMARY KEY REFERENCES devices(device_id) ON DELETE CASCADE,
    generation INTEGER NOT NULL,
    state_sha TEXT NOT NULL,
    received_at INTEGER NOT NULL,
    ledger_schema INTEGER NOT NULL,
    fw_version TEXT NOT NULL,
    rtc_day INTEGER NOT NULL,
    pet_json TEXT NOT NULL,
    battery_pct INTEGER NOT NULL,
    charging INTEGER NOT NULL,
    inventory_json TEXT NOT NULL,
    sd_free_kib INTEGER,
    parent_settings_json TEXT NOT NULL,
    parent_settings_rev INTEGER NOT NULL,
    awaiting_json TEXT NOT NULL,
    temperament TEXT NOT NULL,
    -- hash of the whole upload: identical retries are answered read-only
    upload_sha TEXT NOT NULL,
    -- Consecutive snapshots older than the stored generation (§5.2).
    stale_streak INTEGER NOT NULL DEFAULT 0,
    stale_at INTEGER,
    conflict_at INTEGER
);
CREATE TABLE memory_current (
    device_id TEXT PRIMARY KEY REFERENCES devices(device_id) ON DELETE CASCADE,
    generation INTEGER NOT NULL,
    memory_json TEXT NOT NULL,
    clear_epoch INTEGER NOT NULL
);
CREATE TABLE daily_summaries (
    device_id TEXT NOT NULL REFERENCES devices(device_id) ON DELETE CASCADE,
    day INTEGER NOT NULL,
    feeds INTEGER NOT NULL,
    greetings INTEGER NOT NULL,
    books INTEGER NOT NULL,
    games INTEGER NOT NULL,
    choices INTEGER NOT NULL,
    last_book INTEGER NOT NULL,
    last_game INTEGER NOT NULL,
    PRIMARY KEY(device_id, day)
);
-- One row per device: §5.4 tombstone + epoch for parent memory clears.
-- clear_state: none | pending | applied | rejected | expired
CREATE TABLE memory_clears (
    device_id TEXT PRIMARY KEY REFERENCES devices(device_id) ON DELETE CASCADE,
    clear_epoch INTEGER NOT NULL,
    command_id INTEGER,
    clear_state TEXT NOT NULL,
    requested_at INTEGER NOT NULL,
    -- generation of the snapshot that carried the applied receipt; memory is
    -- accepted again only from a later generation.
    applied_generation INTEGER
);
CREATE TABLE commands (
    command_id INTEGER PRIMARY KEY CHECK(command_id > 0 AND command_id < 9007199254740992),
    device_id TEXT NOT NULL REFERENCES devices(device_id) ON DELETE CASCADE,
    -- per-device delivery order (device_sync.server_rev at enqueue)
    seq INTEGER NOT NULL,
    kind TEXT NOT NULL,
    payload_json TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    expires_at INTEGER NOT NULL,
    state TEXT NOT NULL CHECK(state IN ('pending','applied','rejected','expired')),
    result_code TEXT,
    delivered_at INTEGER,
    completed_at INTEGER
);
CREATE INDEX commands_device_state ON commands(device_id, state);
CREATE TABLE device_sync (
    device_id TEXT PRIMARY KEY REFERENCES devices(device_id) ON DELETE CASCADE,
    server_rev INTEGER NOT NULL
);
CREATE TABLE maintenance_runs (
    job TEXT PRIMARY KEY,
    last_run_at INTEGER NOT NULL
);
