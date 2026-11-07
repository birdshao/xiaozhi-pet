"""Server S2: snapshot sync, command channel, tombstones, memory APIs (§5.2-§8)."""
import base64
from dataclasses import dataclass
import json
from pathlib import Path
import sys
import tempfile
import unittest

from cryptography.fernet import Fernet
from fastapi.testclient import TestClient

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from services.pet_learning import commands  # noqa: E402
from services.pet_learning.api import create_app  # noqa: E402
from services.pet_learning.auth import ParentAuth  # noqa: E402
from services.pet_learning.config import Settings  # noqa: E402
from services.pet_learning.db import connect  # noqa: E402
from services.pet_learning.memory_codec import (  # noqa: E402
    Day, Growth, Memory, ParentSettings, Preference)
from services.pet_learning.sync import CatalogNames, civil_day  # noqa: E402
from services.pet_learning.totp import code_at, new_secret  # noqa: E402

PASSWORD = "correct-test-password-1234"
TOKEN = "device-token-" + "a" * 32
OTHER_TOKEN = "device-token-" + "b" * 32
VOICE_TOKEN = "voice-token-" + "c" * 32
START = 1_793_600_000  # 2026-11-03 local
FIXTURES = ROOT / "tests" / "fixtures" / "memory"


@dataclass(frozen=True)
class VoiceSettings(Settings):
    # The voice identity field arrives with S5; declared here so S2 runs alone.
    voice_token: str = ""


class Names(CatalogNames):
    def like(self, db, slot, option):
        return {(7, 2): ("最喜欢的动物", "大象")}.get((slot, option))

    def book(self, db, content_id):
        return {9: "森林里的朋友"}.get(content_id)


def memory_with(day, *, feeds=1, likes=((7, 2), (3, 4)), books=(9, 17), yesterday=None):
    prefs = tuple(Preference(s, o, 100 + i) for i, (s, o) in enumerate(likes))
    return Memory(1, Day(day, feeds, 0, 1 if books else 0, 0, len(likes), books[0] if books else 0, 0),
                  yesterday or Day(), Growth(len(books), 0, len(likes), 1, 1 | 32), prefs, tuple(books))


class StateSyncTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name) / "pet.sqlite3"
        self.now = START
        self.settings = VoiceSettings(totp_key=Fernet.generate_key().decode(), voice_token=VOICE_TOKEN)
        self.app = create_app(self.path, settings=self.settings)
        self.sync = self.app.state.state_sync
        self.sync.clock = lambda: self.now
        self.sync.names = Names()
        store = self.app.state.pet_store
        store.enroll_device("pet-001", TOKEN)
        store.enroll_device("pet-002", OTHER_TOKEN)
        self.client = TestClient(self.app, base_url="https://testserver")
        self.addCleanup(self.client.close)
        self.generation = 0

    # -- helpers ----------------------------------------------------------

    def snapshot(self, memory=None, *, generation=None, receipts=(), awaiting=(), **changes):
        if generation is None:
            self.generation += 1
            generation = self.generation
        body = {
            "schema_version": "2.0", "generation": generation, "ledger_schema": 3,
            "fw_version": "pet-2026.11.04", "rtc_day": 20761,
            "pet": {"mode": "IDLE", "mood": "HAPPY", "interaction_count": 57, "active_page": 1, "quiet": False},
            "memory_b64": base64.b64encode((memory or memory_with(20761)).encode()).decode(),
            "parent_settings_b64": base64.b64encode(ParentSettings().encode()).decode(),
            "parent_settings_rev": 7,
            "battery": {"pct": 64, "charging": False},
            "inventory": [{"kind": "book", "content_id": 17, "revision": 2, "header_crc": "9a0c11e2"}],
            "sd_free_kib": 1843200,
            "awaiting_confirm": list(awaiting),
            "applied_commands": [{"command_id": c, "result": r} for c, r in receipts],
        }
        body.update(changes)
        return body

    def put(self, body, *, device="pet-001", token=TOKEN, advance=11):
        self.now += advance
        return self.client.put(f"/v1/devices/{device}/state", json=body,
                               headers={"Authorization": f"Bearer {token}"})

    def login(self):
        secret = new_secret()
        auth = ParentAuth(self.path, self.settings, clock=lambda: self.now)
        auth.create_account("parent", PASSWORD, secret, code_at(secret, self.now))
        self.app.state.parent_auth = auth
        self.now += 30
        result = self.client.post("/v1/parent/login", json={
            "username": "parent", "password": PASSWORD, "otp": code_at(secret, self.now)})
        self.assertEqual(result.status_code, 200)
        self.csrf = result.json()["csrf_token"]

    def memories(self, device="pet-001"):
        result = self.client.get(f"/v1/parent/devices/{device}/memories")
        self.assertEqual(result.status_code, 200, result.text)
        return result.json()

    def clear(self):
        result = self.client.delete("/v1/parent/devices/pet-001/memories", headers={"X-CSRF-Token": self.csrf})
        self.assertEqual(result.status_code, 200, result.text)
        return result.json()

    def summary(self, **headers):
        return self.client.get("/v1/voice/pet-001/summary",
                               headers={"Authorization": f"Bearer {VOICE_TOKEN}", **headers})

    def table(self, sql, *args):
        with connect(self.path) as db:
            return [tuple(r) for r in db.execute(sql, args)]

    # -- snapshot protocol --------------------------------------------------

    def test_accept_duplicate_conflict_and_stale(self):
        first = self.snapshot()
        result = self.put(first)
        self.assertEqual(result.status_code, 200)
        self.assertEqual(result.json()["result"], "accepted")
        self.assertEqual(self.put(first).json()["result"], "duplicate")
        self.assertEqual(self.put(first, advance=0).json()["result"], "duplicate")  # retry inside 10 s
        # Volatile fields (no ledger commit) refresh the row under the same generation.
        refreshed = dict(first, battery={"pct": 10, "charging": True}, rtc_day=20762)
        self.assertEqual(self.put(refreshed, advance=1).json()["result"], "duplicate")
        self.assertEqual(self.table("SELECT battery_pct,rtc_day FROM device_state"), [(10, 20762)])
        # Same generation, different ledger content: conflict, nothing stored.
        changed = dict(first, memory_b64=base64.b64encode(memory_with(20761, feeds=9).encode()).decode())
        self.assertEqual(self.put(changed).json()["result"], "generation_conflict")
        self.assertEqual(self.table("SELECT battery_pct FROM device_state"), [(10,)])
        self.assertEqual(self.sync.memories("pet-001")["memory"]["today"]["feeds"], 1)
        self.assertIn("GENERATION_CONFLICT", self.sync.memories("pet-001")["warnings"])
        self.now += 8 * 86400
        self.assertNotIn("GENERATION_CONFLICT", self.sync.memories("pet-001")["warnings"])
        self.assertEqual(self.put(self.snapshot()).json()["result"], "accepted")
        stale = self.put(self.snapshot(generation=1))
        self.assertEqual(stale.json()["result"], "stale")
        self.assertEqual(stale.json()["generation"], 2)
        self.assertEqual(self.table("SELECT action,result FROM audit_log WHERE action LIKE 'state.%'"),
                         [("state.conflict", "rejected")])

    def test_persistent_regression_warns_parent(self):
        self.login()
        self.put(self.snapshot(generation=50))
        for _ in range(3):
            self.assertEqual(self.put(self.snapshot(generation=5)).json()["result"], "stale")
        self.assertIn("GENERATION_REGRESSED", self.memories()["warnings"])
        self.put(self.snapshot(generation=51))
        self.assertNotIn("GENERATION_REGRESSED", self.memories()["warnings"])

    def test_schema_size_rate_and_decode_rejections(self):
        self.assertEqual(self.put(self.snapshot(schema_version="1.0")).json()["detail"], "UNSUPPORTED_SCHEMA_VERSION")
        self.assertEqual(self.put(self.snapshot(ledger_schema=2)).json()["detail"], "UNSUPPORTED_LEDGER_SCHEMA")
        self.assertEqual(self.put(self.snapshot(extra=1)).status_code, 422)
        bad = (FIXTURES / "memory_bad_format_v2.bin").read_bytes()
        result = self.put(self.snapshot(memory_b64=base64.b64encode(bad).decode()))
        self.assertEqual((result.status_code, result.json()["detail"]), (422, "MEMORY_FORMAT_UNSUPPORTED"))
        bad = (FIXTURES / "memory_bad_duplicate_slot.bin").read_bytes()
        self.assertEqual(self.put(self.snapshot(memory_b64=base64.b64encode(bad).decode())).json()["detail"],
                         "MEMORY_CORRUPT")
        bad = (FIXTURES / "settings_bad_version_2.bin").read_bytes()
        self.assertEqual(self.put(self.snapshot(parent_settings_b64=base64.b64encode(bad).decode()))
                         .json()["detail"], "SETTINGS_VERSION_UNSUPPORTED")
        self.assertEqual(self.put(self.snapshot(memory_b64="!" * 384)).json()["detail"], "BASE64_INVALID")
        huge = self.snapshot(fw_version="x" * 6000)
        self.assertEqual(self.put(huge).status_code, 413)
        for bad in ({"generation": True}, {"generation": 5.0}, {"rtc_day": "20761"},
                    {"battery": {"pct": 64.0, "charging": False}}, {"awaiting_confirm": [2 ** 53]},
                    {"awaiting_confirm": [0]}, {"awaiting_confirm": [1] * 9},
                    {"pet": {"mode": "HOME", "mood": "HAPPY", "interaction_count": 1, "active_page": 1,
                             "quiet": False}},
                    {"pet": {"mode": "IDLE", "mood": "HAPPY", "interaction_count": 1, "active_page": 1,
                             "quiet": 0}}):
            with self.subTest(bad):
                self.assertEqual(self.put(self.snapshot(**bad)).json()["detail"], "INVALID_STATE")
        self.assertEqual(self.table("SELECT COUNT(*) FROM device_state"), [(0,)])
        self.assertEqual(self.put(self.snapshot()).status_code, 200)
        early = self.put(self.snapshot(), advance=3)
        self.assertEqual((early.status_code, early.headers["retry-after"]), (429, "10"))

    def test_worst_case_snapshot_fits_the_limit(self):
        top = 2 ** 53 - 1
        worst = self.snapshot(
            generation=2 ** 63 - 1, fw_version="x" * 40, rtc_day=2 ** 32 - 1, parent_settings_rev=2 ** 32 - 1,
            pet={"mode": "LISTENING", "mood": "THINKING", "interaction_count": 2 ** 32 - 1,
                 "active_page": 65535, "quiet": False},
            battery={"pct": 100, "charging": False}, sd_free_kib=2 ** 32 - 1,
            inventory=[{"kind": "soundpack", "content_id": 65535, "revision": 65535,
                        "header_crc": "ffffffff"}] * 32,
            awaiting_confirm=[top] * 8, applied_commands=[{"command_id": top, "result": "rejected"}] * 16)
        compact = json.dumps(worst, separators=(",", ":")).encode()
        self.assertEqual(len(compact), 4440)
        self.now += 11
        result = self.client.put("/v1/devices/pet-001/state", content=compact, headers={
            "Authorization": f"Bearer {TOKEN}", "Content-Type": "application/json"})
        self.assertEqual(result.json()["result"], "accepted")
        padded = compact[:-1] + b" " * (5121 - len(compact)) + b"}"
        self.now += 11
        self.assertEqual(self.client.put("/v1/devices/pet-001/state", content=padded, headers={
            "Authorization": f"Bearer {TOKEN}"}).status_code, 413)

    def test_retries_are_answered_without_a_write(self):
        first = self.snapshot()
        self.put(first)
        writes = []
        original = self.sync._accept

        def counting(*args, **kwargs):
            writes.append(1)
            return original(*args, **kwargs)
        self.sync._accept = counting
        self.assertEqual(self.put(first, advance=1).json()["result"], "duplicate")
        self.assertEqual(writes, [])
        with connect(self.path) as db:  # a new command must be delivered: write path
            commands.enqueue(db, "pet-001", "SYNC_CONTENT", {}, self.now)
        self.assertEqual(len(self.put(first, advance=1).json()["commands"]), 1)
        self.assertEqual(writes, [1])
        self.assertEqual(self.put(first, advance=1).json()["result"], "duplicate")
        self.assertEqual(writes, [1])
        self.put(self.snapshot(generation=5))
        self.put(self.snapshot(generation=1))
        self.assertEqual(self.put(self.snapshot(generation=1), advance=1).json()["result"], "stale")
        self.assertEqual(self.table("SELECT stale_streak FROM device_state"), [(1,)])
        # Repeated decode failures produce one audit row per hour.
        bad = base64.b64encode((FIXTURES / "memory_bad_zero_book.bin").read_bytes()).decode()
        for _ in range(5):
            self.assertEqual(self.put(self.snapshot(memory_b64=bad)).status_code, 422)
        self.assertEqual(self.table("SELECT COUNT(*) FROM audit_log WHERE action='state.reject'"), [(1,)])

    def test_confirm_kinds_always_require_confirmation(self):
        original = dict(commands.KINDS)
        self.addCleanup(lambda: commands.KINDS.update(original))
        commands.KINDS.update({"MEMORY_CLEAR": False, "SET_TEMPERAMENT": False})  # misconfiguration
        with connect(self.path) as db:
            clear = commands.enqueue(db, "pet-001", "MEMORY_CLEAR", {"clear_epoch": 1}, self.now)
            commands.enqueue(db, "pet-001", "SET_TEMPERAMENT", {"temperament": "lively"}, self.now)
            with self.assertRaises(ValueError):
                commands.enqueue(db, "pet-001", "SYNC_CONTENT", {"requires_confirm": False}, self.now)
        self.assertEqual(clear["expires_at"] - self.now, commands.CONFIRM_TTL)
        delivered = self.put(self.snapshot()).json()["commands"]
        self.assertEqual([(c["kind"], c["requires_confirm"]) for c in delivered],
                         [("MEMORY_CLEAR", True), ("SET_TEMPERAMENT", True)])

    def test_late_refusal_after_resume_does_not_refreeze(self):
        self.login()
        self.put(self.snapshot())
        cid = self.clear()["memory_clear"]["command"]["command_id"]
        self.now += commands.CONFIRM_TTL + 1
        self.sync.resume("pet-001", None)
        self.put(self.snapshot(receipts=[(cid, "rejected")]))
        self.put(self.snapshot())
        view = self.sync.memories("pet-001")
        self.assertEqual(view["memory_clear"]["state"], "none")
        self.assertEqual(len(view["memory"]["likes"]), 2)
        self.assertEqual(self.table("SELECT state FROM commands"), [("rejected",)])

    def test_token_is_bound_to_its_device(self):
        body = self.snapshot()
        self.assertEqual(self.put(body, token=OTHER_TOKEN).status_code, 401)
        self.assertEqual(self.put(body, device="pet-002", token=TOKEN).status_code, 401)
        self.assertEqual(self.put(body, token="").status_code, 401)
        self.assertEqual(self.client.get("/v1/devices/pet-001/commands",
                                         headers={"Authorization": f"Bearer {OTHER_TOKEN}"}).status_code, 401)
        self.assertEqual(self.put(body, device="pet-002", token=OTHER_TOKEN).status_code, 200)
        self.assertEqual(self.table("SELECT device_id FROM device_state"), [("pet-002",)])
        # Device and voice identities do not open parent routes, and vice versa.
        for auth in (TOKEN, VOICE_TOKEN):
            self.assertEqual(self.client.get("/v1/parent/devices/pet-001/memories",
                                             headers={"Authorization": f"Bearer {auth}"}).status_code, 401)
        self.assertEqual(self.client.get("/v1/voice/pet-001/summary",
                                         headers={"Authorization": f"Bearer {TOKEN}"}).status_code, 401)

    def test_daily_archive_and_offline_gap(self):
        self.login()
        self.put(self.snapshot(memory_with(20761, feeds=2)))
        self.put(self.snapshot(memory_with(20761, feeds=3)))
        self.assertEqual(self.memories()["days"], [])
        # Next day: the device's own "yesterday" carries the finished day.
        yesterday = Day(20761, 4, 0, 1, 0, 2, 9, 0)
        self.put(self.snapshot(memory_with(20762, feeds=1, yesterday=yesterday)))
        self.assertEqual([(d["day"], d["feeds"]) for d in self.memories()["days"]], [(20761, 4)])
        # Offline three days: only the last active day survives on the device.
        self.put(self.snapshot(memory_with(20766, feeds=1, yesterday=Day(20763, 2, 0, 1, 0, 0, 9, 0))))
        days = [(d["day"], d["feeds"]) for d in self.memories()["days"]]
        self.assertEqual(days, [(20763, 2), (20762, 1), (20761, 4)])
        self.assertEqual(self.memories()["memory"]["growth"]["milestones"], ["first_book", "first_preference"])

    # -- commands and tombstones -------------------------------------------

    def test_clear_tombstone_beats_stale_memory_until_applied(self):
        self.login()
        self.put(self.snapshot())
        self.assertIsNotNone(self.memories()["memory"])
        cleared = self.clear()
        command = cleared["memory_clear"]["command"]
        cid = command["command_id"]
        self.assertTrue(0 < cid < 2 ** 53)
        self.assertEqual(command["status"], "queued")
        self.assertEqual(self.memories()["memory"], None)
        self.assertEqual(self.table("SELECT COUNT(*) FROM daily_summaries"), [(0,)])
        # A later-generation snapshot still carrying the old memory is ignored.
        result = self.put(self.snapshot()).json()
        self.assertEqual(result["commands"][0]["kind"], "MEMORY_CLEAR")
        self.assertEqual(result["commands"][0]["payload"], {"clear_epoch": 1})
        self.assertTrue(result["commands"][0]["requires_confirm"])
        self.assertIsNone(self.memories()["memory"])
        self.assertEqual(self.memories()["memory_clear"]["command"]["status"], "delivered")
        # Waiting for the parent on the device is not a ledger commit: same generation.
        self.put(self.snapshot(generation=self.generation, awaiting=[cid]), advance=1)
        self.assertEqual(self.memories()["memory_clear"]["command"]["status"], "awaiting_device_confirm")
        self.assertEqual(self.summary().json(), {"trait": "gentle"})
        # Receipt snapshot: memory still dropped; the next one is accepted.
        empty = Memory(1)
        receipt = self.put(self.snapshot(empty, receipts=[(cid, "applied")])).json()
        self.assertEqual(receipt["acked_commands"], [cid])
        self.assertEqual(receipt["commands"], [])
        self.assertIsNone(self.memories()["memory"])
        self.assertEqual(self.memories()["memory_clear"]["state"], "applied")
        self.put(self.snapshot(empty))
        self.assertEqual(self.memories()["memory"]["likes"], [])
        # An old (lower generation) snapshot cannot write memory back.
        self.assertEqual(self.put(self.snapshot(generation=1)).json()["result"], "stale")
        self.assertEqual(self.memories()["memory"]["likes"], [])

    def test_device_rejection_keeps_server_copy_deleted_until_resume(self):
        self.login()
        self.put(self.snapshot())
        cid = self.clear()["memory_clear"]["command"]["command_id"]
        resume = self.client.post("/v1/parent/devices/pet-001/memories/resume", headers={"X-CSRF-Token": self.csrf})
        self.assertEqual((resume.status_code, resume.json()["detail"]), (409, "CLEAR_PENDING"))
        self.put(self.snapshot(receipts=[(cid, "rejected")]))
        self.put(self.snapshot())
        view = self.memories()
        self.assertIsNone(view["memory"])
        self.assertEqual(view["memory_clear"]["state"], "rejected")
        self.assertTrue(view["memory_clear"]["device_memory_not_cleared"])
        self.assertEqual(view["memory_clear"]["command"]["status"], "rejected")
        self.assertEqual(self.client.post("/v1/parent/devices/pet-001/memories/resume").status_code, 403)
        resume = self.client.post("/v1/parent/devices/pet-001/memories/resume", headers={"X-CSRF-Token": self.csrf})
        self.assertEqual(resume.status_code, 200)
        self.put(self.snapshot())
        self.assertEqual(len(self.memories()["memory"]["likes"]), 2)
        # A second clear uses a new epoch and a new command id.
        again = self.clear()["memory_clear"]
        self.assertEqual(again["clear_epoch"], 2)
        self.assertNotEqual(again["command"]["command_id"], cid)

    def test_late_confirmation_after_resume_drops_resumed_memory(self):
        self.login()
        self.put(self.snapshot(memory_with(20762, yesterday=Day(20761, 1, 0, 0, 0, 0, 0, 0))))
        cid = self.clear()["memory_clear"]["command"]["command_id"]
        self.now += commands.CONFIRM_TTL + 1
        self.assertEqual(self.sync.memories("pet-001")["memory_clear"]["state"], "expired")
        self.sync.resume("pet-001", None)
        self.put(self.snapshot(memory_with(20762, yesterday=Day(20761, 1, 0, 0, 0, 0, 0, 0))))
        self.assertIsNotNone(self.sync.memories("pet-001")["memory"])
        # The parent finally confirms on a clock-less device.
        self.put(self.snapshot(Memory(1), receipts=[(cid, "applied")]))
        view = self.sync.memories("pet-001")
        self.assertIsNone(view["memory"])
        self.assertEqual(view["days"], [])
        self.assertEqual(self.sync.summary("pet-001"), {"trait": "gentle"})

    def test_repeated_clear_while_pending_is_idempotent(self):
        self.login()
        first = self.clear()["memory_clear"]["command"]["command_id"]
        second = self.clear()["memory_clear"]["command"]["command_id"]
        self.assertEqual(first, second)
        self.assertEqual(self.table("SELECT COUNT(*) FROM commands"), [(1,)])

    def test_command_expiry_and_late_device_receipt(self):
        self.login()
        self.put(self.snapshot())
        cid = self.clear()["memory_clear"]["command"]["command_id"]
        self.now += commands.CONFIRM_TTL + 1
        fetched = self.client.get("/v1/devices/pet-001/commands", headers={"Authorization": f"Bearer {TOKEN}"})
        self.assertEqual(fetched.json()["commands"], [])
        view = self.sync.memories("pet-001")  # the parent session idled out meanwhile
        self.assertEqual(view["memory_clear"]["state"], "expired")
        self.assertIsNone(view["memory"])
        # The device later reports it did apply the clear: tombstone lifts after that generation.
        self.put(self.snapshot(Memory(1), receipts=[(cid, "applied")]))
        self.put(self.snapshot(Memory(1)))
        view = self.sync.memories("pet-001")
        self.assertEqual(view["memory_clear"]["state"], "applied")
        self.assertEqual(view["memory"]["likes"], [])
        # Final device results are never overwritten.
        self.put(self.snapshot(Memory(1), receipts=[(cid, "rejected")]))
        self.assertEqual(self.table("SELECT state FROM commands"), [("applied",)])

    def test_command_lifecycle_kinds_ttl_and_device_scope(self):
        with connect(self.path) as db:
            temperament = commands.enqueue(db, "pet-001", "SET_TEMPERAMENT", {"temperament": "lively"}, self.now)
            sync = commands.enqueue(db, "pet-001", "SYNC_CONTENT", {}, self.now)
            other = commands.enqueue(db, "pet-002", "SYNC_CONTENT", {}, self.now)
            with self.assertRaises(ValueError):
                commands.enqueue(db, "pet-001", "RUN_SHELL", {}, self.now)
        self.assertEqual(temperament["expires_at"] - self.now, 7 * 86400)
        self.assertEqual(sync["expires_at"] - self.now, 86400)
        response = self.put(self.snapshot()).json()
        self.assertEqual([c["kind"] for c in response["commands"]], ["SET_TEMPERAMENT", "SYNC_CONTENT"])
        self.assertEqual([c["requires_confirm"] for c in response["commands"]], [True, False])
        rev = response["server_rev"]
        # Receipts for another device's command are ignored but acknowledged.
        result = self.put(self.snapshot(receipts=[(temperament["command_id"], "rejected"),
                                                  (other["command_id"], "applied"),
                                                  (sync["command_id"], "applied")])).json()
        self.assertEqual(result["commands"], [])
        self.assertGreater(result["server_rev"], rev)
        states = dict(self.table("SELECT kind || ':' || device_id, state FROM commands"))
        self.assertEqual(states, {"SET_TEMPERAMENT:pet-001": "rejected", "SYNC_CONTENT:pet-001": "applied",
                                  "SYNC_CONTENT:pet-002": "pending"})
        self.now += 86400 + 1
        with connect(self.path) as db:
            self.assertEqual([r["kind"] for r in commands.expire_overdue(db, self.now)], ["SYNC_CONTENT"])

    # -- voice summary and parent view -------------------------------------

    def test_minimal_summary_contents(self):
        self.now = START + 86400 * 2 - 3600  # local 2026-11-04 evening
        self.assertEqual(civil_day(self.now, "Asia/Shanghai"), 20761)
        self.put(self.snapshot(memory_with(20761, feeds=2)))
        body = self.summary().json()
        self.assertEqual(body, {"trait": "gentle", "likes": [{"topic": "最喜欢的动物", "option": "大象"}],
                                "today": {"feed": 2, "books": 1, "games": 0}, "last_book": "森林里的朋友"})
        text = json.dumps(body, ensure_ascii=False)
        for forbidden in ("pet-001", "20761", "slot", "100", "generation"):
            self.assertNotIn(forbidden, text)
        self.now += 86400  # next day, no new snapshot: today's counts are stale
        self.assertEqual(self.summary().json()["today"], {"feed": 0, "books": 0, "games": 0})
        self.assertEqual(self.summary(**{"X-Forwarded-For": "1.2.3.4"}).status_code, 403)
        self.assertEqual(self.client.get("/v1/voice/pet-001/summary", headers={
            "Authorization": "Bearer wrong", "Forwarded": "for=1.2.3.4"}).status_code, 403)
        self.assertEqual(self.client.get("/v1/voice/pet-001/summary").status_code, 401)
        self.assertEqual(self.client.get("/v1/voice/nope-999/summary",
                                         headers={"Authorization": f"Bearer {VOICE_TOKEN}"}).status_code, 404)

    def test_voice_summary_disabled_without_voice_token(self):
        self.app.state.settings = Settings(totp_key=self.settings.totp_key)
        self.assertEqual(self.summary().status_code, 401)

    def test_parent_view_requires_session_and_csrf(self):
        self.assertEqual(self.client.get("/v1/parent/devices/pet-001/memories").status_code, 401)
        self.login()
        self.assertEqual(self.client.delete("/v1/parent/devices/pet-001/memories").status_code, 403)
        self.assertEqual(self.client.delete("/v1/parent/devices/pet-001/memories",
                                            headers={"X-CSRF-Token": "wrong"}).status_code, 403)
        self.assertEqual(self.client.get("/v1/parent/devices/nope-999/memories").status_code, 404)
        self.put(self.snapshot())
        view = self.memories()
        self.assertEqual(view["memory"]["likes"][0], {"slot": 7, "option": 2, "topic": "最喜欢的动物", "label": "大象"})
        self.assertEqual(view["memory"]["recent_books"][0], {"content_id": 9, "title": "森林里的朋友"})
        self.assertEqual(self.client.get("/v1/parent/devices/pet-001/memories").headers["cache-control"], "no-store")
        # Preference source event ids are never stored server side.
        self.assertNotIn("source_event", self.table("SELECT memory_json FROM memory_current")[0][0])

    def test_audit_has_no_memory_content(self):
        self.login()
        self.put(self.snapshot())
        self.clear()
        rows = self.table("SELECT actor,action,target,result FROM audit_log WHERE actor!='parent' "
                          "OR action LIKE 'memory.%' OR action LIKE 'command.%'")
        self.assertIn(("parent", "memory.clear", "pet-001", "requested"), rows)
        self.assertIn(("parent", "command.memory_clear", "pet-001", "queued"), rows)
        dump = json.dumps(self.table("SELECT * FROM audit_log"), ensure_ascii=False)
        for forbidden in (TOKEN, "大象", "clear_epoch", "memory_b64"):
            self.assertNotIn(forbidden, dump)

    def test_v2_database_upgrade_and_device_delete_cascade(self):
        old = Path(self.tmp.name) / "old.sqlite3"
        with connect(old) as db:
            for name in ("001_baseline.sql", "002_parent_auth.sql"):
                db.executescript((ROOT / "services/pet_learning/migrations" / name).read_text(encoding="utf-8"))
            db.execute("PRAGMA user_version=2")
            db.execute("INSERT INTO devices(device_id,token_sha) VALUES('pet-009','sha')")
        sync = type(self.sync)(old)
        with connect(old) as db:
            self.assertEqual(db.execute("SELECT device_id FROM devices").fetchall()[0][0], "pet-009")
            self.assertEqual(db.execute("SELECT COUNT(*) FROM device_state").fetchone()[0], 0)
        self.assertEqual(sync.memories("pet-009")["memory_clear"]["state"], "none")
        # S1's "delete device" removes every S2 row with it (§8).
        self.login()
        self.put(self.snapshot())
        self.clear()
        with connect(self.path) as db:
            for table in ("event_log", "quiz_sessions", "story_reader_sessions"):
                db.execute(f"DELETE FROM {table} WHERE device_id='pet-001'")
            db.execute("DELETE FROM devices WHERE device_id='pet-001'")
        for table in ("device_state", "memory_current", "daily_summaries", "memory_clears",
                      "commands", "device_sync"):
            self.assertEqual(self.table(f"SELECT COUNT(*) FROM {table} WHERE device_id='pet-001'"), [(0,)])

    # -- retention (D6) -----------------------------------------------------

    def test_retention_purges_operational_data_only(self):
        self.login()
        self.put(self.snapshot(memory_with(20762, yesterday=Day(20761, 1, 0, 0, 0, 0, 0, 0))))
        with connect(self.path) as db:
            done = commands.enqueue(db, "pet-001", "SYNC_CONTENT", {}, self.now)
            commands.apply_receipts(db, "pet-001", [(done["command_id"], "applied")], self.now)
            commands.enqueue(db, "pet-001", "SET_TEMPERAMENT", {"temperament": "lively"}, self.now)
        memory_tables = ("memory_current", "daily_summaries", "memory_clears", "device_state")
        before = {t: self.table(f"SELECT * FROM {t}") for t in memory_tables}
        counts = self.sync.run_retention(self.now + 400 * 86400)
        self.assertEqual(counts["commands"], 1)  # the finished command; the other just expired
        self.assertGreater(counts["audit_log"], 0)
        self.assertEqual(self.table("SELECT kind,state FROM commands"), [("SET_TEMPERAMENT", "expired")])
        self.assertEqual({t: self.table(f"SELECT * FROM {t}") for t in memory_tables}, before)
        self.assertEqual(self.sync.run_retention(self.now + 431 * 86400)["commands"], 1)

    def test_retention_runs_daily_from_device_traffic(self):
        self.put(self.snapshot())
        (first,) = self.table("SELECT last_run_at FROM maintenance_runs")
        self.put(self.snapshot())
        self.assertEqual(self.table("SELECT last_run_at FROM maintenance_runs"), [first])
        self.now += 86400
        self.put(self.snapshot())
        self.assertGreater(self.table("SELECT last_run_at FROM maintenance_runs")[0][0], first[0])


if __name__ == "__main__":
    unittest.main()
