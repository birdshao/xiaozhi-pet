CREATE TABLE content_packages (
    package_id INTEGER PRIMARY KEY AUTOINCREMENT,
    kind TEXT NOT NULL CHECK(kind IN ('book','soundpack','game')),
    content_id INTEGER,
    revision INTEGER NOT NULL,
    sha256 TEXT NOT NULL UNIQUE,
    header_crc INTEGER NOT NULL,
    bytes INTEGER NOT NULL,
    path TEXT NOT NULL,
    title TEXT NOT NULL,
    status TEXT NOT NULL DEFAULT 'draft' CHECK(status IN ('draft','approved','retired')),
    created_at INTEGER NOT NULL,
    reviewed_by TEXT,
    reviewed_at INTEGER,
    checklist_json TEXT
);
CREATE INDEX content_packages_fingerprint ON content_packages(kind, content_id, revision);
CREATE INDEX content_packages_path ON content_packages(kind, path, status);
CREATE TABLE device_content (
    device_id TEXT NOT NULL REFERENCES devices(device_id) ON DELETE CASCADE,
    package_id INTEGER NOT NULL REFERENCES content_packages(package_id),
    wanted TEXT NOT NULL CHECK(wanted IN ('install','remove')),
    state TEXT NOT NULL DEFAULT 'pending' CHECK(state IN ('pending','downloading','installed','failed')),
    reason TEXT,
    attempts INTEGER NOT NULL DEFAULT 0,
    updated_at INTEGER NOT NULL,
    PRIMARY KEY(device_id, package_id)
);
CREATE TABLE device_content_inventory (
    device_id TEXT PRIMARY KEY REFERENCES devices(device_id) ON DELETE CASCADE,
    sd_free_kib INTEGER,
    inventory_json TEXT NOT NULL,
    reported_at INTEGER NOT NULL
);
CREATE TABLE content_pages (
    package_id INTEGER NOT NULL REFERENCES content_packages(package_id),
    page INTEGER NOT NULL,
    text TEXT NOT NULL,
    PRIMARY KEY(package_id, page)
);
CREATE TABLE like_topics (
    package_id INTEGER NOT NULL REFERENCES content_packages(package_id),
    slot INTEGER NOT NULL,
    key TEXT NOT NULL,
    label TEXT NOT NULL,
    PRIMARY KEY(package_id, slot)
);
CREATE TABLE like_options (
    package_id INTEGER NOT NULL REFERENCES content_packages(package_id),
    slot INTEGER NOT NULL,
    option INTEGER NOT NULL,
    key TEXT NOT NULL,
    label TEXT NOT NULL,
    PRIMARY KEY(package_id, slot, option)
);
