CREATE TABLE IF NOT EXISTS devices (
    device_id TEXT PRIMARY KEY, token_sha TEXT NOT NULL,
    seq INTEGER NOT NULL DEFAULT 0,
    version INTEGER NOT NULL DEFAULT 0,
    interaction_count INTEGER NOT NULL DEFAULT 0,
    active_book TEXT, active_page INTEGER NOT NULL DEFAULT 1,
    mode TEXT NOT NULL DEFAULT 'HOME', quiet INTEGER NOT NULL DEFAULT 0,
    open_chat INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS event_log (
    device_id TEXT NOT NULL,
    message_id TEXT NOT NULL,
    seq INTEGER NOT NULL,
    payload_sha TEXT NOT NULL,
    accepted_version INTEGER NOT NULL,
    PRIMARY KEY(device_id, message_id),
    UNIQUE(device_id, seq),
    FOREIGN KEY(device_id) REFERENCES devices(device_id)
);
CREATE TABLE IF NOT EXISTS books (
    book_id TEXT PRIMARY KEY, title TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS pages (
    book_id TEXT NOT NULL, page INTEGER NOT NULL,
    text TEXT NOT NULL, approved INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY(book_id, page),
    FOREIGN KEY(book_id) REFERENCES books(book_id)
);

CREATE TABLE IF NOT EXISTS quiz_cards (
    card_id TEXT PRIMARY KEY, prompt TEXT NOT NULL,
    first_text TEXT NOT NULL, second_text TEXT NOT NULL,
    correct INTEGER NOT NULL CHECK(correct IN (1,2)),
    success_text TEXT NOT NULL, retry_text TEXT NOT NULL,
    approved INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS quiz_sessions (
    device_id TEXT PRIMARY KEY,
    active_card TEXT,
    attempts INTEGER NOT NULL DEFAULT 0,
    next_cursor INTEGER NOT NULL DEFAULT 0,
    completed INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS story_reader_sessions(
    device_id TEXT PRIMARY KEY,
    current_book TEXT,
    current_page INTEGER NOT NULL DEFAULT 1,
    next_cursor INTEGER NOT NULL DEFAULT 0
);
