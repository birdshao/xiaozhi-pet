-- Server S6 (V6-04/05, ADR-015/016): open chat with GLM.
-- llm_actions_json: the device actions the parent lets the model request
-- (NULL = the default subset, which excludes SLEEP_REQUEST).
ALTER TABLE policies ADD COLUMN llm_actions_json TEXT;
ALTER TABLE policies ADD COLUMN open_chat_disabled_at INTEGER;
-- Breaker input: one row per model call outcome. No text, no reply, no reason detail.
CREATE TABLE open_chat_outcomes (
    device_id TEXT NOT NULL REFERENCES devices(device_id) ON DELETE CASCADE,
    at INTEGER NOT NULL,
    outcome TEXT NOT NULL CHECK(outcome IN ('ok','error','block'))
);
CREATE INDEX open_chat_outcomes_device_at ON open_chat_outcomes(device_id, at);
