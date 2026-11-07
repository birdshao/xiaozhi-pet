"""S3: initialisation, profile and policy APIs, device confirmation, merge flows, migration 006."""
import base64
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from s3_support import DEVICE, PASSWORD, S3Base  # noqa: E402
from services.pet_learning import commands, policy  # noqa: E402
from services.pet_learning.db import MIGRATIONS, connect, migrate  # noqa: E402
from services.pet_learning.memory_codec import ParentSettings, decode_parent_settings  # noqa: E402
from services.pet_learning.totp import code_at  # noqa: E402

PACK = {"kind": "soundpack", "content_id": 3, "revision": 1, "header_crc": "0000abcd"}
BASE_URL = f"/v1/parent/devices/{DEVICE}"


def settings_of(command):
    return decode_parent_settings(base64.b64decode(command["payload"]["settings_b64"]))


class ProfileBase(S3Base):
    def setUp(self):
        super().setUp()
        self.login()

    def approve_pack(self, *, crc=0xABCD, status="approved", content_id=3):
        with connect(self.path) as db:
            db.execute("INSERT INTO content_packages(kind,content_id,revision,sha256,header_crc,bytes,path,"
                       "title,status,created_at) VALUES('soundpack',?,1,?,?,10,'p','温柔声音包',?,1)",
                       (content_id, f"{crc:064x}{status}"[:64], crc, status))

    def init(self, **body):
        body = {"pet_name": "团团", "temperament": "lively", **body}
        return self.parent("POST", f"{BASE_URL}/init", json=body)

    def pending(self, kind=None):
        rows = self.table("SELECT command_id,kind,state,result_code FROM commands ORDER BY seq")
        return [r for r in rows if kind is None or r[1] == kind]


class MigrationTests(unittest.TestCase):
    def test_006_adds_tables_to_a_005_database_without_touching_data(self):
        with tempfile.TemporaryDirectory() as tmp:
            old = Path(tmp) / "old"
            old.mkdir()
            for path in sorted(MIGRATIONS.glob("00[1-5]_*.sql")):
                shutil.copy(path, old / path.name)
            db_path = Path(tmp) / "pet.sqlite3"
            migrate(db_path, directory=old)
            with connect(db_path) as db:
                db.execute("INSERT INTO devices(device_id,token_sha) VALUES('pet-001','x')")
                self.assertEqual(db.execute("PRAGMA user_version").fetchone()[0], 5)
            migrate(db_path)
            with connect(db_path) as db:
                self.assertEqual(db.execute("PRAGMA user_version").fetchone()[0], len(list(MIGRATIONS.glob("*.sql"))))
                self.assertEqual(db.execute("SELECT COUNT(*) FROM devices").fetchone()[0], 1)
                for table in ("profiles", "policies", "parent_notes"):
                    self.assertEqual(db.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0], 0)
                # open chat defaults off and temperaments are constrained by the schema
                db.execute("INSERT INTO policies(device_id,settings_json,base_json,updated_at) "
                           "VALUES('pet-001','{}','{}',1)")
                self.assertEqual(db.execute("SELECT open_chat FROM policies").fetchone()[0], 0)
                with self.assertRaises(Exception):
                    db.execute("INSERT INTO profiles VALUES('pet-001',1,'a','angry',NULL,1,1)")
                # deleting the device cascades to every S3 table
                db.execute("DELETE FROM devices WHERE device_id='pet-001'")
                self.assertEqual(db.execute("SELECT COUNT(*) FROM policies").fetchone()[0], 0)


class AuthTests(ProfileBase):
    ROUTES = (("GET", "profile"), ("PUT", "profile"), ("POST", "init"), ("GET", "policy"),
              ("PUT", "policy"), ("GET", "notes"), ("PUT", "notes"), ("DELETE", "notes"))

    def test_route_enumeration_every_s3_route_is_parent_only(self):
        listed = {(method.upper(), path.rsplit("/", 1)[1]) for path, methods in self.app.openapi()["paths"].items()
                  if path.startswith("/v1/parent/devices/{device_id}/") for method in methods}
        self.assertTrue(set(self.ROUTES) <= listed, set(self.ROUTES) - listed)
        anonymous = type(self.client)(self.app, base_url="https://testserver")
        self.addCleanup(anonymous.close)
        for method, name in self.ROUTES:
            with self.subTest(route=f"{method} {name}"):
                url = f"{BASE_URL}/{name}"
                # No session: 401 before any body validation.
                self.assertEqual(anonymous.request(method, url, json={}).status_code, 401)
                # A device token is not a parent session.
                self.assertEqual(anonymous.request(method, url, json={}, headers={
                    "Authorization": "Bearer device-token-" + "a" * 32}).status_code, 401)
                if method != "GET":  # session cookie without the CSRF token
                    self.assertEqual(self.client.request(method, url, json={}).status_code, 403)
                else:
                    self.assertEqual(self.client.get(url).status_code, 200)

    def test_foreign_origin_is_refused_and_unknown_device_404(self):
        self.assertEqual(self.client.get(f"{BASE_URL}/profile", headers={"Origin": "https://evil.example"}).status_code, 403)
        self.assertEqual(self.client.get("/v1/parent/devices/nope-device/profile").status_code, 404)
        self.assertEqual(self.client.get("/v1/parent/devices/x/profile").status_code, 404)

    def test_device_routes_do_not_accept_parent_cookie_and_responses_are_no_store(self):
        self.assertEqual(self.client.get(f"/v1/devices/{DEVICE}/commands").status_code, 401)
        for name in ("profile", "policy", "notes"):
            self.assertEqual(self.client.get(f"{BASE_URL}/{name}").headers["cache-control"], "no-store")


class InitTests(ProfileBase):
    def test_init_flow_queues_confirmed_temperament_and_voice_pack(self):
        self.approve_pack()
        self.put_state(self.snapshot(inventory=[PACK]))
        view = self.client.get(f"{BASE_URL}/profile").json()
        self.assertFalse(view["initialized"])
        self.assertEqual(view["available_voice_packs"], [{"content_id": 3, "revision": 1, "title": "温柔声音包"}])
        result = self.init(voice_pack_id=3)
        self.assertEqual(result.status_code, 200, result.text)
        body = result.json()
        self.assertEqual((body["rev"], body["pet_name"], body["temperament"], body["voice_pack_id"]),
                         (1, "团团", "lively", 3))
        self.assertEqual({k: v["status"] for k, v in body["commands"].items()},
                         {"temperament": "queued", "voice_pack": "queued"})
        # S2's channel: the personality needs the parent on the device, the voice pack does not.
        state = self.put_state(self.snapshot(inventory=[PACK]))
        wire = {c["kind"]: c for c in state["commands"]}
        self.assertEqual(wire["SET_TEMPERAMENT"]["payload"], {"temperament": "lively"})
        self.assertTrue(wire["SET_TEMPERAMENT"]["requires_confirm"])
        self.assertEqual(wire["SELECT_VOICE_PACK"]["payload"], {"content_id": 3})
        self.assertFalse(wire["SELECT_VOICE_PACK"]["requires_confirm"])
        self.assertEqual(wire["SET_TEMPERAMENT"]["command_id"] >> 63, 0)
        # Waiting for the parent on the device, then confirmed / applied.
        t, v = wire["SET_TEMPERAMENT"]["command_id"], wire["SELECT_VOICE_PACK"]["command_id"]
        self.put_state(self.snapshot(inventory=[PACK], awaiting=[t]))
        view = self.client.get(f"{BASE_URL}/profile").json()
        self.assertEqual(view["commands"]["temperament"]["status"], "awaiting_device_confirm")
        self.assertEqual(view["commands"]["voice_pack"]["status"], "delivered")
        self.put_state(self.snapshot(inventory=[PACK], receipts=[(t, "applied"), (v, "applied")]))
        view = self.client.get(f"{BASE_URL}/profile").json()
        self.assertEqual({k: c["status"] for k, c in view["commands"].items()},
                         {"temperament": "applied", "voice_pack": "applied"})

    def test_rejected_on_device_is_shown_and_the_profile_keeps_the_panel_choice(self):
        self.init()
        state = self.put_state(self.snapshot())
        t = state["commands"][0]["command_id"]
        self.put_state(self.snapshot(receipts=[(t, "rejected")]))
        view = self.client.get(f"{BASE_URL}/profile").json()
        self.assertEqual(view["commands"]["temperament"]["status"], "rejected")
        self.assertEqual(view["temperament"], "lively")

    def test_unconfirmed_personality_expires_after_seven_days_other_commands_after_one(self):
        self.approve_pack()
        self.put_state(self.snapshot(inventory=[PACK]))
        self.init(voice_pack_id=3)
        rows = self.table("SELECT kind, expires_at - created_at FROM commands")
        self.assertEqual(dict(rows), {"SET_TEMPERAMENT": 7 * 86400, "SELECT_VOICE_PACK": 86400})
        self.now += 8 * 86400
        view = self.app.state.profile_service.get_profile(DEVICE)  # the parent session idles out after 1 h
        self.assertEqual({k: c["status"] for k, c in view["commands"].items()},
                         {"temperament": "expired", "voice_pack": "expired"})

    def test_name_temperament_and_voice_pack_are_validated(self):
        for name in ("", "团团1", "abc1", "a" * 13, "团" * 7, "http://a.b", "tuan tuan", "<b>x</b>", "团团\n"):
            with self.subTest(name=name):
                self.assertEqual(self.init(pet_name=name).status_code, 422)
        self.assertEqual(self.init(temperament="angry").status_code, 422)
        self.assertEqual(self.client.post(f"{BASE_URL}/init", headers={"X-CSRF-Token": self.csrf},
                                          json={"pet_name": "团团", "temperament": "gentle", "x": 1}).status_code, 422)
        self.assertEqual(self.init(pet_name="Tuan").status_code, 200)
        self.assertEqual(self.table("SELECT COUNT(*) FROM profiles"), [(1,)])

    def test_voice_pack_must_be_in_inventory_and_approved_with_the_same_crc(self):
        self.put_state(self.snapshot(inventory=[PACK]))
        self.assertEqual(self.init(voice_pack_id=3).json()["detail"], "VOICE_PACK_UNAVAILABLE")  # not in catalogue
        self.approve_pack(crc=0x1234)  # approved package, but a different file than the device holds
        self.assertEqual(self.init(voice_pack_id=3).status_code, 409)
        self.assertEqual(self.init(voice_pack_id=9).status_code, 409)  # not in the device inventory
        self.assertEqual(self.table("SELECT COUNT(*) FROM profiles"), [(0,)])
        self.assertEqual(self.table("SELECT COUNT(*) FROM commands"), [(0,)])
        with connect(self.path) as db:
            db.execute("UPDATE content_packages SET status='retired'")
        self.approve_pack(crc=0xABCD)
        self.assertEqual(self.init(voice_pack_id=3).status_code, 200)

    def test_init_twice_and_update_before_init_conflict(self):
        self.assertEqual(self.parent("PUT", f"{BASE_URL}/profile", json={"base_rev": 0, "pet_name": "团团"}).json()["detail"],
                         "NOT_INITIALIZED")
        self.assertEqual(self.init().status_code, 200)
        self.assertEqual(self.init().json()["detail"], "ALREADY_INITIALIZED")

    def test_revoked_device_cannot_be_changed(self):
        with connect(self.path) as db:
            db.execute("UPDATE devices SET status='revoked'")
        self.assertEqual(self.init().json()["detail"], "DEVICE_NOT_ACTIVE")
        self.assertEqual(self.client.get(f"{BASE_URL}/profile").status_code, 200)


class ProfileUpdateTests(ProfileBase):
    def setUp(self):
        super().setUp()
        self.init(temperament="gentle")

    def put(self, **body):
        return self.parent("PUT", f"{BASE_URL}/profile", json=body)

    def test_name_change_is_panel_only_and_reaches_the_voice_summary(self):
        before = len(self.pending())
        result = self.put(base_rev=1, pet_name="豆豆")
        self.assertEqual((result.status_code, result.json()["rev"], result.json()["pet_name"]), (200, 2, "豆豆"))
        self.assertEqual(len(self.pending()), before)  # no device command for a name
        summary = self.client.get(f"/v1/voice/{DEVICE}/summary", headers={"Authorization": "Bearer voice-token-" + "c" * 32})
        self.assertEqual(summary.json()["name"], "豆豆")

    def test_stale_base_rev_is_a_conflict_and_changes_nothing(self):
        self.assertEqual(self.put(base_rev=1, pet_name="豆豆").status_code, 200)
        stale = self.put(base_rev=1, pet_name="朵朵")
        self.assertEqual((stale.status_code, stale.json()["detail"]), (409, "PROFILE_REV_CONFLICT"))
        self.assertEqual(self.client.get(f"{BASE_URL}/profile").json()["pet_name"], "豆豆")

    def test_new_temperament_supersedes_the_pending_one(self):
        self.assertEqual(self.put(base_rev=1, temperament="curious").status_code, 200)
        rows = self.pending("SET_TEMPERAMENT")
        self.assertEqual([(r[2], r[3]) for r in rows], [("expired", "SUPERSEDED"), ("pending", None)])
        state = self.put_state(self.snapshot())
        self.assertEqual([c["payload"] for c in state["commands"] if c["kind"] == "SET_TEMPERAMENT"],
                         [{"temperament": "curious"}])
        # Re-submitting the same value is not a change: no new command, no rev bump.
        same = self.put(base_rev=2, temperament="curious")
        self.assertEqual((same.json()["rev"], len(self.pending("SET_TEMPERAMENT"))), (2, 2))

    def test_empty_update_and_bad_values_rejected(self):
        self.assertEqual(self.put(base_rev=1).status_code, 422)
        self.assertEqual(self.put(base_rev=1, temperament="x").status_code, 422)
        self.assertEqual(self.put(base_rev=1, pet_name="a1").status_code, 422)
        self.assertEqual(self.put(base_rev=True, pet_name="豆豆").status_code, 422)

    def test_audit_records_operation_types_not_content(self):
        self.put(base_rev=1, pet_name="豆豆")
        rows = self.table("SELECT actor, action, target, result FROM audit_log WHERE action LIKE 'profile.%'")
        self.assertEqual(rows, [("parent", "profile.init", DEVICE, "success"), ("parent", "profile.update", DEVICE, "success")])
        for row in self.table("SELECT * FROM audit_log"):
            self.assertNotIn("豆豆", " ".join(map(str, row)))


class PolicyTests(ProfileBase):
    def put(self, **body):
        return self.parent("PUT", f"{BASE_URL}/policy", json=body)

    def get(self):
        return self.client.get(f"{BASE_URL}/policy").json()

    def test_defaults_open_chat_off_and_unselectable(self):
        view = self.get()
        self.assertEqual((view["rev"], view["open_chat"], view["open_chat_selectable"], view["command"]),
                         (0, False, False, None))
        self.assertEqual(view["settings"], ParentSettings().as_dict())

    def test_settings_edit_issues_one_unconfirmed_command_with_revisions(self):
        self.put_state(self.snapshot(rev=3))
        result = self.put(base_rev=0, settings={"volume_cap_pct": 70, "motion_enabled": False})
        self.assertEqual(result.status_code, 200, result.text)
        view = result.json()
        self.assertEqual((view["rev"], view["settings"]["volume_cap_pct"], view["notice"]), (4, 70, None))
        self.assertEqual(view["command"]["status"], "queued")
        state = self.put_state(self.snapshot(rev=3))
        (command,) = [c for c in state["commands"] if c["kind"] == "SET_PARENT_SETTINGS"]
        self.assertFalse(command["requires_confirm"])  # D13
        self.assertEqual(command["payload"]["base_rev"], 3)
        self.assertEqual(command["payload"]["new_rev"], 4)
        self.assertEqual((settings_of(command).volume_cap_pct, settings_of(command).motion_enabled), (70, False))

    def test_stale_panel_revision_is_refused(self):
        self.assertEqual(self.put(base_rev=0, settings={"volume_cap_pct": 70}).status_code, 200)
        stale = self.put(base_rev=0, settings={"volume_cap_pct": 90})
        self.assertEqual((stale.status_code, stale.json()["detail"]), (409, "POLICY_REV_CONFLICT"))

    def test_invalid_settings_rejected(self):
        for settings in ({"volume_cap_pct": 75}, {"volume_cap_pct": 5}, {"quiet_start_min": 2000},
                         {"evening_min": 60}, {"wake_min": 1260}, {"volume_cap_pct": True}, {"bogus": 1}):
            with self.subTest(settings=settings):
                self.assertEqual(self.put(base_rev=0, settings=settings).status_code, 422)
        self.assertEqual(self.put(base_rev=0).status_code, 422)
        self.assertEqual(self.table("SELECT COUNT(*) FROM policies"), [(0,)])

    def test_panel_edit_then_device_local_edit_merges_conservatively(self):
        self.put_state(self.snapshot())
        self.put(base_rev=0, settings={"motion_enabled": False})  # queued, not yet applied
        # Meanwhile the child-proofing parent lowered the volume in the device menu (rev unsupported: 0).
        state = self.put_state(self.snapshot(ParentSettings(volume_cap_pct=60)))
        view = self.get()
        self.assertEqual(view["notice"], policy.NOTICE_MERGED)
        self.assertEqual((view["settings"]["volume_cap_pct"], view["settings"]["motion_enabled"]), (60, False))
        # Never last-write-wins: the stale motion-only command is replaced by the merged one.
        sent = [c for c in state["commands"] if c["kind"] == "SET_PARENT_SETTINGS"]
        self.assertEqual(len(sent), 1)
        self.assertEqual((settings_of(sent[0]).volume_cap_pct, settings_of(sent[0]).motion_enabled), (60, False))
        states = [r[2] for r in self.pending("SET_PARENT_SETTINGS")]
        self.assertEqual(states, ["expired", "pending"])

    def test_merge_where_device_already_is_safer_issues_nothing_new(self):
        self.put_state(self.snapshot())
        self.put(base_rev=0, settings={"volume_cap_pct": 80})
        self.put_state(self.snapshot(ParentSettings(volume_cap_pct=60)))
        self.assertEqual(self.get()["settings"]["volume_cap_pct"], 60)
        self.assertEqual([r[2] for r in self.pending("SET_PARENT_SETTINGS")], ["expired"])  # nothing left to deliver
        self.assertEqual(self.put_state(self.snapshot(ParentSettings(volume_cap_pct=60)))["commands"], [])

    def test_device_only_change_is_adopted_and_stale_forms_get_409(self):
        self.put_state(self.snapshot())
        self.put(base_rev=0, settings={"volume_cap_pct": 90})
        t = self.pending("SET_PARENT_SETTINGS")[0][0]
        self.put_state(self.snapshot(ParentSettings(volume_cap_pct=90), receipts=[(t, "applied")], rev=1))
        view = self.get()
        self.assertEqual((view["notice"], view["command"]["status"]), (None, "applied"))
        # The device menu changes quiet volume with nothing pending: the device is a legitimate writer.
        self.put_state(self.snapshot(ParentSettings(volume_cap_pct=90, quiet_volume_pct=10), rev=2))
        view = self.get()
        self.assertEqual((view["notice"], view["settings"]["quiet_volume_pct"]), (policy.NOTICE_DEVICE_CHANGED, 10))
        self.assertGreater(view["rev"], 1)
        self.assertEqual(self.put(base_rev=1, settings={"volume_cap_pct": 100}).status_code, 409)
        self.assertEqual(len(self.pending("SET_PARENT_SETTINGS")), 1)  # no command for the adoption

    def test_after_the_device_applied_the_next_edit_does_not_merge(self):
        self.put_state(self.snapshot())
        self.put(base_rev=0, settings={"volume_cap_pct": 70})
        t = self.pending("SET_PARENT_SETTINGS")[0][0]
        self.put_state(self.snapshot(ParentSettings(volume_cap_pct=70), receipts=[(t, "applied")]))
        view = self.put(base_rev=self.get()["rev"], settings={"volume_cap_pct": 100}).json()
        self.assertEqual((view["notice"], view["settings"]["volume_cap_pct"]), (None, 100))

    def test_revision_free_devices_are_tolerated_end_to_end(self):
        # A codec-v1 device reports rev 0 forever; every flow above works on content alone.
        self.put_state(self.snapshot(rev=0))
        self.put(base_rev=0, settings={"quiet_volume_pct": 20})
        state = self.put_state(self.snapshot(rev=0))
        (command,) = [c for c in state["commands"] if c["kind"] == "SET_PARENT_SETTINGS"]
        self.assertEqual((command["payload"]["base_rev"], command["payload"]["new_rev"]), (0, 1))

    def test_put_time_merge_when_the_device_moved_since_the_last_sync(self):
        self.put_state(self.snapshot())
        self.put(base_rev=0, settings={"volume_cap_pct": 90})
        t = self.pending("SET_PARENT_SETTINGS")[0][0]
        self.put_state(self.snapshot(ParentSettings(volume_cap_pct=90), receipts=[(t, "applied")]))
        # Simulate a snapshot the merge hook has not seen: base is older than the device row.
        with connect(self.path) as db:
            db.execute("UPDATE device_state SET parent_settings_json=?",
                       (__import__("json").dumps(ParentSettings(volume_cap_pct=50).as_dict()),))
        rev = self.get()["rev"]
        view = self.put(base_rev=rev, settings={"motion_enabled": False}).json()
        self.assertEqual((view["notice"], view["settings"]["volume_cap_pct"], view["settings"]["motion_enabled"]),
                         (policy.NOTICE_MERGED, 50, False))

    def test_unmergeable_edits_fail_closed(self):
        self.put_state(self.snapshot())
        self.assertEqual(self.put(base_rev=0, open_chat=False).status_code, 200)  # creates the policy row
        # PUT: refused with a review hint, nothing issued.
        with connect(self.path) as db:
            db.execute("UPDATE device_state SET parent_settings_json=?",
                       (__import__("json").dumps(ParentSettings(volume_cap_pct=50).as_dict()),))
        with patch.object(policy, "merge_settings", side_effect=policy.MergeError("INVALID")):
            result = self.put(base_rev=1, settings={"quiet_volume_pct": 10})
        self.assertEqual((result.status_code, result.json()["detail"]), (409, "SETTINGS_MERGE_REVIEW"))
        self.assertEqual(self.table("SELECT COUNT(*) FROM commands"), [(0,)])
        # Snapshot hook: the device's own value is kept and the stale panel edit is dropped.
        with connect(self.path) as db:
            db.execute("UPDATE device_state SET parent_settings_json=?",
                       (__import__("json").dumps(ParentSettings().as_dict()),))
        self.put(base_rev=1, settings={"motion_enabled": False})
        with patch.object(policy, "merge_settings", side_effect=policy.MergeError("INVALID")):
            self.put_state(self.snapshot(ParentSettings(volume_cap_pct=60)))
        view = self.get()
        self.assertEqual((view["notice"], view["settings"]["volume_cap_pct"], view["settings"]["motion_enabled"]),
                         (policy.NOTICE_MERGE_FAILED, 60, True))
        self.assertEqual([r[2] for r in self.pending("SET_PARENT_SETTINGS")], ["expired"])


class OpenChatTests(ProfileBase):
    def put(self, **body):
        return self.parent("PUT", f"{BASE_URL}/policy", json={"base_rev": 0, **body})

    def test_turning_chat_on_needs_a_fresh_password_and_totp(self):
        self.assertEqual(self.put(open_chat=True).json()["detail"], "PARENT_REAUTH_REQUIRED")
        self.assertEqual(self.put(open_chat=True, reauth=self.proof(password="wrong-password")).json()["detail"],
                         "PARENT_REAUTH_FAILED")
        stale = {"password": PASSWORD, "otp": code_at(self.secret, self.now - 300)}
        self.assertEqual(self.put(open_chat=True, reauth=stale).status_code, 401)
        self.assertEqual(self.table("SELECT COUNT(*) FROM policies"), [(0,)])

    def test_even_with_proof_chat_stays_off_until_the_code_level_gate_opens(self):
        result = self.put(open_chat=True, reauth=self.proof())
        self.assertEqual((result.status_code, result.json()["detail"]), (409, "OPEN_CHAT_UNAVAILABLE"))
        self.assertFalse(self.client.get(f"{BASE_URL}/policy").json()["open_chat"])

    def test_with_the_gate_open_it_works_once_per_code_and_off_needs_no_proof(self):
        with patch.object(policy, "OPEN_CHAT_SELECTABLE", True):
            proof = self.proof()
            result = self.put(open_chat=True, reauth=proof)
            self.assertEqual(result.status_code, 200, result.text)
            self.assertTrue(result.json()["open_chat"])
            replay = self.parent("PUT", f"{BASE_URL}/policy", json={"base_rev": 1, "open_chat": True, "reauth": proof})
            self.assertEqual(replay.status_code, 401)  # a TOTP code is never reusable
            off = self.parent("PUT", f"{BASE_URL}/policy", json={"base_rev": 1, "open_chat": False})
            self.assertEqual((off.status_code, off.json()["open_chat"], off.json()["open_chat_disabled_reason"]),
                             (200, False, "parent"))

    def test_five_bad_proofs_lock_the_account(self):
        for _ in range(5):
            self.put(open_chat=True, reauth=self.proof(password="wrong-password"))
        self.assertEqual(self.put(open_chat=True, reauth=self.proof()).status_code, 429)


if __name__ == "__main__":
    unittest.main()
