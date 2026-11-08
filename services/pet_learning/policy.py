"""Server S3 (V5-06): parent policy and the conservative settings merge.

V5_V6 plan §5.4 / §7.1 / D13. Two writers change the parent settings (the
device's parent menu and the panel); last-write-wins is forbidden. The server
keeps three versions per device: `settings_json` (what the parent wants),
`base_json` (what the device was last known to hold) and the device's latest
snapshot. Whenever device and panel have both moved, the result is merged
conservatively: lower volumes, the union of quiet hours, motion off.

Revisions: `rev` is a server counter the panel passes back as `base_rev`
(a stale panel gets 409). The command also carries the device's own
`parent_settings_rev` as `base_rev`. A device that cannot store a revision yet
(codec v1) reports 0; change detection then relies on content, never on rev.

Nothing here logs; audit rows carry operation type and device id only.
"""
from __future__ import annotations

from dataclasses import fields, replace
import base64
import json
import sqlite3
from typing import Any

from . import commands
from .memory_codec import MINUTES_PER_DAY, ParentSettings

# Code-level gate (§6.5): open chat cannot be turned on, and no turn reaches the model,
# while this is False. S6 set it after the TTS egress audit of the pinned XiaoZhi
# server (docs/SERVER_S6.md §3) and its overlay contract tests. ADR-016 replaced
# "a real moderation provider" with the parent's decision of no content review; a
# configured GLM key is checked separately (profile.py / open_chat.py).
OPEN_CHAT_SELECTABLE = True
# Actions the model may request, panel order (mirrors open_chat.LLM_ACTIONS; a test pins it).
LLM_ACTIONS = ("OPEN_READER", "PAGE_PREV", "PAGE_NEXT", "OPEN_GAME", "GO_HOME", "PET_FEED", "PET_GROOM",
               "PLAY_SONG", "START_BRUSH", "START_WASH", "SLEEP_REQUEST", "ASK_NEEDS", "TELL_JOKE")
DEFAULT_LLM_ACTIONS = LLM_ACTIONS[:10]


def parse_actions(raw: str | None) -> list[str]:
    """Stored subset in panel order; NULL/corrupt -> the default subset (no SLEEP_REQUEST)."""
    if raw is None:
        return list(DEFAULT_LLM_ACTIONS)
    try:
        chosen = set(json.loads(raw))
    except (ValueError, TypeError):
        return list(DEFAULT_LLM_ACTIONS)
    return [a for a in LLM_ACTIONS if a in chosen]
ISSUED_KEPT = 8
SETTING_FIELDS = tuple(f.name for f in fields(ParentSettings))
NOTICE_MERGED = "MERGED_CONSERVATIVELY"
NOTICE_DEVICE_CHANGED = "DEVICE_CHANGED"
NOTICE_MERGE_FAILED = "MERGE_FAILED_DEVICE_KEPT"


class PolicyError(Exception):
    def __init__(self, status: int, code: str) -> None:
        self.status = status
        self.code = code
        super().__init__(code)


class MergeError(Exception):
    """No valid merged settings exist; the caller keeps the device's version."""


def _length(start: int, end: int) -> int:
    return (end - start) % MINUTES_PER_DAY or MINUTES_PER_DAY


def quiet_hull(a: tuple[int, int], b: tuple[int, int]) -> tuple[int, int] | None:
    """Smallest single circular window covering both quiet windows (their union
    when they overlap). None when that would be the whole day."""
    best: tuple[int, int, int] | None = None
    for (start, end), (o_start, o_end) in ((a, b), (b, a)):
        reach = max(_length(start, end), (o_start - start) % MINUTES_PER_DAY + _length(o_start, o_end))
        if reach < MINUTES_PER_DAY and (best is None or reach < best[2]):
            best = (start, (start + reach) % MINUTES_PER_DAY, reach)
    return None if best is None else (best[0], best[1])


def _three_way(proposed, device, base):
    """Fields with no safe direction: the panel's change wins, else the device's."""
    return device if device != base and proposed == base else proposed


def merge_settings(proposed: ParentSettings, device: ParentSettings,
                   base: ParentSettings) -> ParentSettings:
    """§5.4 conservative merge of the panel's version and the device's version."""
    window = quiet_hull((proposed.quiet_start_min, proposed.quiet_end_min),
                        (device.quiet_start_min, device.quiet_end_min))
    if window is None:
        raise MergeError("QUIET_WINDOW")
    safe = {"volume_cap_pct": min(proposed.volume_cap_pct, device.volume_cap_pct),
            "quiet_volume_pct": min(proposed.quiet_volume_pct, device.quiet_volume_pct),
            "motion_enabled": proposed.motion_enabled and device.motion_enabled,
            "quiet_start_min": window[0], "quiet_end_min": window[1]}
    schedule = ("wake_min", "evening_min", "nap_enabled", "nap_start_min", "nap_end_min")
    # The hull can break a schedule rule (e.g. evening inside quiet hours), so
    # fall back through the device's and the old schedule before giving up.
    for source in (None, device, base):
        values = {name: (_three_way(getattr(proposed, name), getattr(device, name), getattr(base, name))
                         if source is None else getattr(source, name)) for name in schedule}
        merged = replace(proposed, **safe, **values)
        if merged.valid():
            return merged
    raise MergeError("INVALID")


def settings_from_dict(raw: dict[str, Any]) -> ParentSettings:
    return ParentSettings(**{name: raw[name] for name in SETTING_FIELDS})


def _b64(settings: ParentSettings) -> str:
    return base64.b64encode(settings.encode()).decode("ascii")


def _device_settings(db: sqlite3.Connection, device_id: str) -> tuple[ParentSettings, int] | None:
    row = db.execute("SELECT parent_settings_json, parent_settings_rev FROM device_state WHERE device_id=?",
                     (device_id,)).fetchone()
    return (settings_from_dict(json.loads(row[0])), row[1]) if row else None


def _issue(db, device_id: str, settings: ParentSettings, device_rev: int, new_rev: int,
           now: int, ip: str | None, actor: str) -> dict:
    """One SET_PARENT_SETTINGS in flight per device: a newer one supersedes."""
    commands.supersede(db, device_id, "SET_PARENT_SETTINGS", now)
    issued = json.loads(_row(db, device_id)["issued_json"])
    issued = [v for v in issued if v != settings.as_dict()] + [settings.as_dict()]
    db.execute("UPDATE policies SET issued_json=? WHERE device_id=?",
               (json.dumps(issued[-ISSUED_KEPT:], sort_keys=True), device_id))
    return commands.enqueue(db, device_id, "SET_PARENT_SETTINGS",
                            {"settings_b64": _b64(settings), "base_rev": device_rev, "new_rev": new_rev},
                            now, actor=actor, ip=ip)


def _pending(db, device_id: str, now: int) -> bool:
    return db.execute("SELECT 1 FROM commands WHERE device_id=? AND kind='SET_PARENT_SETTINGS' "
                      "AND state='pending' AND expires_at>?", (device_id, now)).fetchone() is not None


def _row(db, device_id: str):
    return db.execute("SELECT * FROM policies WHERE device_id=?", (device_id,)).fetchone()


def view(db: sqlite3.Connection, device_id: str, now: int) -> dict[str, Any]:
    """Parent-facing policy: desired settings, command status, notice."""
    row = _row(db, device_id)
    device = _device_settings(db, device_id)
    state = db.execute("SELECT awaiting_json FROM device_state WHERE device_id=?", (device_id,)).fetchone()
    awaiting = json.loads(state[0]) if state else []
    if row is None:
        settings = (device[0] if device else ParentSettings()).as_dict()
        return {"rev": 0, "settings": settings, "device_settings": device[0].as_dict() if device else None,
                "open_chat": False, "open_chat_disabled_reason": None, "open_chat_disabled_at": None,
                "open_chat_selectable": OPEN_CHAT_SELECTABLE, "llm_actions": list(DEFAULT_LLM_ACTIONS),
                "notice": None, "command": None}
    last = db.execute("SELECT * FROM commands WHERE device_id=? AND kind='SET_PARENT_SETTINGS' "
                      "ORDER BY seq DESC LIMIT 1", (device_id,)).fetchone()
    return {"rev": row["rev"], "settings": json.loads(row["settings_json"]),
            "device_settings": device[0].as_dict() if device else None,
            "open_chat": bool(row["open_chat"]),
            "open_chat_disabled_reason": row["open_chat_disabled_reason"],
            "open_chat_disabled_at": row["open_chat_disabled_at"],
            "open_chat_selectable": OPEN_CHAT_SELECTABLE,
            "llm_actions": parse_actions(row["llm_actions_json"]),
            "notice": row["notice"], "notice_at": row["notice_at"],
            "command": commands.status(dict(last), awaiting) if last else None}


def _issued(row) -> list[ParentSettings]:
    return [settings_from_dict(v) for v in json.loads(row["issued_json"])] if row else []


def update(db: sqlite3.Connection, device_id: str, *, base_rev: int, changes: dict[str, Any],
           open_chat: bool | None, now: int, ip: str | None, actor: str = "parent",
           llm_actions: list[str] | None = None) -> dict[str, Any]:
    """Apply a panel edit inside the caller's write transaction."""
    row = _row(db, device_id)
    current_rev = row["rev"] if row else 0
    if base_rev != current_rev:
        raise PolicyError(409, "POLICY_REV_CONFLICT")
    device = _device_settings(db, device_id)
    device_settings, device_rev = device if device else (None, 0)
    desired = settings_from_dict(json.loads(row["settings_json"])) if row else (device_settings or ParentSettings())
    base = settings_from_dict(json.loads(row["base_json"])) if row else desired
    if device_settings is not None and device_settings in _issued(row):
        base = device_settings  # the device applied a version we sent; that is not a local edit
    notice = None
    issued = False
    settings = desired
    if changes:
        proposed = replace(desired, **changes)
        if not proposed.valid():
            raise PolicyError(422, "SETTINGS_INVALID")
        settings = proposed
        if device_settings is not None and device_settings != base and device_settings != proposed:
            try:
                settings = merge_settings(proposed, device_settings, base)
            except MergeError:
                raise PolicyError(409, "SETTINGS_MERGE_REVIEW") from None
            notice = NOTICE_MERGED
        base_for_row = device_settings if device_settings is not None else base
        if device_settings is None or settings != device_settings:
            issued = True
        else:
            commands.supersede(db, device_id, "SET_PARENT_SETTINGS", now)
    else:
        base_for_row = base
    new_rev = max(current_rev, device_rev) + 1
    reason = row["open_chat_disabled_reason"] if row else None
    chat = bool(row["open_chat"]) if row else False
    disabled_at = row["open_chat_disabled_at"] if row else None
    if open_chat is not None and open_chat != chat:
        chat = open_chat
        reason = None if chat else "parent"
        disabled_at = None if chat else now
        if chat:  # the parent reopened: the breaker counts afresh
            db.execute("DELETE FROM open_chat_outcomes WHERE device_id=?", (device_id,))
    actions_json = row["llm_actions_json"] if row else None
    if llm_actions is not None:
        if len(set(llm_actions)) != len(llm_actions) or not set(llm_actions) <= set(LLM_ACTIONS):
            raise PolicyError(422, "LLM_ACTIONS_INVALID")
        actions_json = json.dumps([a for a in LLM_ACTIONS if a in llm_actions])
    db.execute("INSERT INTO policies(device_id,rev,settings_json,base_json,base_rev,open_chat,"
               "open_chat_disabled_reason,open_chat_disabled_at,llm_actions_json,notice,notice_at,updated_at) "
               "VALUES(?,?,?,?,?,?,?,?,?,?,?,?) "
               "ON CONFLICT(device_id) DO UPDATE SET rev=excluded.rev,settings_json=excluded.settings_json,"
               "base_json=excluded.base_json,base_rev=excluded.base_rev,open_chat=excluded.open_chat,"
               "open_chat_disabled_reason=excluded.open_chat_disabled_reason,"
               "open_chat_disabled_at=excluded.open_chat_disabled_at,llm_actions_json=excluded.llm_actions_json,"
               "notice=excluded.notice,notice_at=excluded.notice_at,updated_at=excluded.updated_at",
               (device_id, new_rev, json.dumps(settings.as_dict(), sort_keys=True),
                json.dumps(base_for_row.as_dict(), sort_keys=True), device_rev, int(chat), reason,
                disabled_at, actions_json, notice, now if notice else None, now))
    if issued:
        _issue(db, device_id, settings, device_rev, new_rev, now, ip, actor)
    commands.audit(db, now, actor, "policy.update", device_id, "merged" if notice else "success", ip)
    return view(db, device_id, now)


def on_snapshot(db: sqlite3.Connection, device_id: str, device: ParentSettings,
                device_rev: int, now: int) -> None:
    """Called by S2 with every accepted snapshot, inside its transaction.

    - device holds the desired settings: it is in sync, move the base up;
    - device unchanged from base: a command is simply in flight;
    - device changed on its own menu while a panel edit is still pending:
      merge conservatively and issue the merged version (never last-write-wins);
    - device changed with nothing pending: the device is a legitimate writer,
      adopt its settings (the panel's rev moves so a stale form gets 409).
    """
    row = _row(db, device_id)
    if row is None:
        return
    desired = settings_from_dict(json.loads(row["settings_json"]))
    base = settings_from_dict(json.loads(row["base_json"]))
    encoded = json.dumps(device.as_dict(), sort_keys=True)
    if device == desired:
        if device != base or device_rev != row["base_rev"]:
            db.execute("UPDATE policies SET base_json=?,base_rev=? WHERE device_id=?",
                       (encoded, device_rev, device_id))
        return
    if device == base:
        return
    if device in _issued(row):
        # The device applied an earlier version we issued while a newer edit is still in flight:
        # not a local edit, so no merge (a merge here would wrongly pull the newer edit down).
        db.execute("UPDATE policies SET base_json=?,base_rev=? WHERE device_id=?", (encoded, device_rev, device_id))
        return
    new_rev = max(row["rev"], device_rev) + 1
    if _pending(db, device_id, now):
        try:
            merged = merge_settings(desired, device, base)
            notice = NOTICE_MERGED
        except MergeError:
            merged, notice = device, NOTICE_MERGE_FAILED
    else:
        merged, notice = device, NOTICE_DEVICE_CHANGED
    db.execute("UPDATE policies SET rev=?,settings_json=?,base_json=?,base_rev=?,notice=?,notice_at=?,"
               "updated_at=? WHERE device_id=?",
               (new_rev, json.dumps(merged.as_dict(), sort_keys=True), encoded, device_rev, notice, now,
                now, device_id))
    if merged != device:
        _issue(db, device_id, merged, device_rev, new_rev, now, None, "server")
    else:
        # The panel's pending edit is dropped, not applied after the device's own change.
        commands.supersede(db, device_id, "SET_PARENT_SETTINGS", now)
    commands.audit(db, now, "server", "policy.merge", device_id, notice.lower())


__all__ = ["PolicyError", "MergeError", "merge_settings", "quiet_hull", "view", "update", "on_snapshot"]
