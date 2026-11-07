"""Server -> device command queue (V5_V6 plan §5.3).

Every function runs inside the caller's SQLite write transaction. Command ids
are random, non-zero and below 2**53: the top bit is 0 (device "external" id
space, IsExternalEventId) and the value survives cJSON's double parsing. The
device uses the id as its ledger event id, so a re-delivered command is a
replay-window duplicate and never runs twice.
"""
from __future__ import annotations

import json
import secrets
import sqlite3
from typing import Any, Iterable

DAY_SECONDS = 24 * 3600
# kind -> needs a parent confirmation on the device (D5) before it applies.
KINDS = {
    "MEMORY_CLEAR": True,
    "SET_TEMPERAMENT": True,
    "SET_PARENT_SETTINGS": False,
    "SELECT_VOICE_PACK": False,
    "SYNC_CONTENT": False,
}
CONFIRM_TTL = 7 * DAY_SECONDS
DEFAULT_TTL = DAY_SECONDS
COMPLETED_RETENTION = 30 * DAY_SECONDS
ID_BITS = 53
RESULTS = frozenset({"applied", "rejected", "expired"})


def audit(db: sqlite3.Connection, now: int, actor: str, action: str,
          device_id: str, result: str, ip: str | None = None) -> None:
    """Operation type only; never memory content, payloads or tokens (§8)."""
    db.execute("INSERT INTO audit_log(at,actor,action,target,result,ip) VALUES(?,?,?,?,?,?)",
               (now, actor, action, device_id, result, ip))


def server_rev(db: sqlite3.Connection, device_id: str) -> int:
    row = db.execute("SELECT server_rev FROM device_sync WHERE device_id=?", (device_id,)).fetchone()
    return row[0] if row else 0


def _bump(db: sqlite3.Connection, device_id: str) -> None:
    db.execute("INSERT INTO device_sync(device_id,server_rev) VALUES(?,1) "
               "ON CONFLICT(device_id) DO UPDATE SET server_rev=server_rev+1", (device_id,))


def enqueue(db: sqlite3.Connection, device_id: str, kind: str, payload: dict[str, Any],
            now: int, *, actor: str = "parent", ip: str | None = None) -> dict[str, Any]:
    if kind not in KINDS:
        raise ValueError("Unknown command kind")
    ttl = CONFIRM_TTL if KINDS[kind] else DEFAULT_TTL
    while True:
        command_id = secrets.randbits(ID_BITS)
        if command_id and not db.execute("SELECT 1 FROM commands WHERE command_id=?",
                                         (command_id,)).fetchone():
            break
    _bump(db, device_id)
    db.execute("INSERT INTO commands(command_id,device_id,seq,kind,payload_json,created_at,expires_at,state) "
               "VALUES(?,?,?,?,?,?,?,'pending')",
               (command_id, device_id, server_rev(db, device_id), kind,
                json.dumps(payload, sort_keys=True), now, now + ttl))
    audit(db, now, actor, f"command.{kind.lower()}", device_id, "queued", ip)
    return get(db, command_id)


def get(db: sqlite3.Connection, command_id: int) -> dict[str, Any] | None:
    row = db.execute("SELECT * FROM commands WHERE command_id=?", (command_id,)).fetchone()
    return dict(row) if row else None


def _finish(db: sqlite3.Connection, row: sqlite3.Row | dict, state: str, code: str,
            now: int, actor: str) -> dict[str, Any]:
    db.execute("UPDATE commands SET state=?,result_code=?,completed_at=? WHERE command_id=?",
               (state, code, now, row["command_id"]))
    _bump(db, row["device_id"])
    audit(db, now, actor, f"command.{row['kind'].lower()}", row["device_id"], state)
    return {**dict(row), "state": state, "result_code": code, "completed_at": now}


def expire_overdue(db: sqlite3.Connection, now: int, device_id: str | None = None) -> list[dict]:
    """Pending commands past expires_at become expired; returns the changed rows."""
    query = "SELECT * FROM commands WHERE state='pending' AND expires_at<=?"
    args: tuple = (now,)
    if device_id is not None:
        query += " AND device_id=?"
        args += (device_id,)
    return [_finish(db, row, "expired", "TIMEOUT", now, "server")
            for row in db.execute(query, args).fetchall()]


def apply_receipts(db: sqlite3.Connection, device_id: str,
                   receipts: Iterable[tuple[int, str]], now: int) -> list[dict]:
    """Device receipts from a snapshot. Only this device's commands change.

    pending -> applied/rejected/expired. A command the server already timed out
    may still be reported applied/rejected: the device is the authority on
    what it did (an unknown-clock device may apply an idempotent clear late).
    Final device results are never overwritten.
    """
    changed = []
    for command_id, result in receipts:
        if result not in RESULTS:
            continue
        row = db.execute("SELECT * FROM commands WHERE command_id=? AND device_id=?",
                         (command_id, device_id)).fetchone()
        if row is None:
            continue
        late = row["state"] == "expired" and row["result_code"] == "TIMEOUT" and result != "expired"
        if row["state"] == "pending" or late:
            changed.append(_finish(db, row, result, f"DEVICE_{result.upper()}", now, "device"))
    return changed


def deliverable(db: sqlite3.Connection, device_id: str, now: int) -> list[dict[str, Any]]:
    """Pending, unexpired commands in creation order; marks first delivery."""
    rows = db.execute("SELECT * FROM commands WHERE device_id=? AND state='pending' AND expires_at>? "
                      "ORDER BY seq", (device_id, now)).fetchall()
    db.execute("UPDATE commands SET delivered_at=? WHERE device_id=? AND state='pending' "
               "AND expires_at>? AND delivered_at IS NULL", (now, device_id, now))
    return [{"command_id": r["command_id"], "kind": r["kind"], "expires_at": r["expires_at"],
             "requires_confirm": KINDS[r["kind"]], "payload": json.loads(r["payload_json"])}
            for r in rows]


def status(row: dict[str, Any] | None, awaiting: Iterable[int] = ()) -> dict[str, Any] | None:
    """Parent-facing view: queued / delivered / awaiting_device_confirm / final."""
    if row is None:
        return None
    shown = row["state"]
    if shown == "pending":
        if row["command_id"] in set(awaiting):
            shown = "awaiting_device_confirm"
        elif row["delivered_at"] is not None:
            shown = "delivered"
        else:
            shown = "queued"
    return {"command_id": row["command_id"], "kind": row["kind"], "status": shown,
            "created_at": row["created_at"], "expires_at": row["expires_at"],
            "completed_at": row["completed_at"]}


def purge_completed(db: sqlite3.Connection, now: int) -> int:
    """D6: finished commands are kept 30 days after completion."""
    return db.execute("DELETE FROM commands WHERE state!='pending' AND completed_at<=?",
                      (now - COMPLETED_RETENTION,)).rowcount
