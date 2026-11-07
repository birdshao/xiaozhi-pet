-- Server S3 (V5-06): parent initialisation, policy and parent notes.
-- Profiles and policies are written only from the parent panel (and the
-- conservative merge); parent_notes is encrypted and read by no other module.
CREATE TABLE profiles (
    device_id TEXT PRIMARY KEY REFERENCES devices(device_id) ON DELETE CASCADE,
    rev INTEGER NOT NULL,
    pet_name TEXT NOT NULL,
    -- temperament: what the parent requested. temperament_confirmed: what the device confirmed
    -- (set only from an applied receipt; NULL until then). S6 persona reads the confirmed value.
    -- temperament_seq: monotonic number carried in every SET_TEMPERAMENT payload.
    temperament TEXT NOT NULL CHECK(temperament IN ('curious','gentle','lively')),
    temperament_confirmed TEXT CHECK(temperament_confirmed IN ('curious','gentle','lively')),
    temperament_seq INTEGER NOT NULL DEFAULT 0,
    voice_pack_id INTEGER,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
);
-- settings_json: what the parent wants the device to have (desired).
-- base_json/base_rev: the settings the server last knew the device to hold,
-- the third side of the 5.4 conservative merge. A device that cannot send a
-- revision yet (codec v1) reports rev 0; the merge then compares content only.
CREATE TABLE policies (
    device_id TEXT PRIMARY KEY REFERENCES devices(device_id) ON DELETE CASCADE,
    rev INTEGER NOT NULL DEFAULT 0,
    settings_json TEXT NOT NULL,
    base_json TEXT NOT NULL,
    base_rev INTEGER NOT NULL DEFAULT 0,
    -- recent settings versions the server issued: a snapshot equal to one is "applied", not a local edit
    issued_json TEXT NOT NULL DEFAULT '[]',
    open_chat INTEGER NOT NULL DEFAULT 0 CHECK(open_chat IN (0,1)),
    open_chat_disabled_reason TEXT,
    notice TEXT,
    notice_at INTEGER,
    updated_at INTEGER NOT NULL
);
-- One encrypted blob per device (nickname, birthday month-day, note, device id).
CREATE TABLE parent_notes (
    device_id TEXT PRIMARY KEY REFERENCES devices(device_id) ON DELETE CASCADE,
    payload_enc TEXT NOT NULL,
    updated_at INTEGER NOT NULL
);
