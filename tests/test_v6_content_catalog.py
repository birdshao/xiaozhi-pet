"""S4: content import, sha256-bound approval, manifests, Range download, reports."""
from __future__ import annotations

import hashlib
import io
import json
import struct
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from cryptography.fernet import Fernet
from fastapi.testclient import TestClient

from services.pet_learning import content, distribution
from services.pet_learning.api import create_app
from services.pet_learning.auth import ParentAuth
from services.pet_learning.config import Settings
from services.pet_learning.db import connect
from services.pet_learning.totp import code_at, new_secret
from tools import fake_device, kpb2, sound_pack

ROOT = Path(__file__).resolve().parent.parent
BOOK = (ROOT / "tests/fixtures/kpb2/golden_book.kpb").read_bytes()
KPB1 = (ROOT / "tests/fixtures/kpb2/golden_kpb1.kpb").read_bytes()
GAME = (ROOT / "tests/fixtures/kpg1/golden_game.kpg").read_bytes()
SOUND = sound_pack.encode({1: b"\x10\x00" * 300, 111: b"\x20\x00" * 900}, revision=3)
PASSWORD = "correct-test-password-1234"
CHECKLIST = {"text_reviewed": True, "audio_listened": True}
TOKEN_A, TOKEN_B = "a" * 40, "b" * 40


def book_revision(revision: int) -> bytes:
    """The golden book re-sealed as another revision (header CRC only)."""
    blob = bytearray(BOOK)
    struct.pack_into("<H", blob, 18, revision)
    struct.pack_into("<I", blob, 92, kpb2.crc32(bytes(blob[:92])))
    return bytes(blob)


def sha(blob: bytes) -> str:
    return hashlib.sha256(blob).hexdigest()


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name) / "data" / "pet.sqlite3"
        self.now = 1800000000
        settings = Settings(totp_key=Fernet.generate_key().decode())
        auth = ParentAuth(self.path, settings, clock=lambda: self.now)
        secret = new_secret()
        auth.create_account("parent", PASSWORD, secret, code_at(secret, self.now))
        self.now += 30
        self.app = create_app(self.path, settings=settings)
        self.app.state.parent_auth = auth
        self.client = TestClient(self.app, base_url="https://testserver")
        self.addCleanup(self.client.close)
        self.csrf = self.client.post("/v1/parent/login", json=dict(
            username="parent", password=PASSWORD, otp=code_at(secret, self.now))).json()["csrf_token"]
        store = self.app.state.pet_store
        store.enroll_device("pet-a", TOKEN_A)
        store.enroll_device("pet-b", TOKEN_B)
        self.catalog = self.app.state.content_catalog

    # parent helpers
    def upload(self, blob, query="", **kw):
        headers = {"X-CSRF-Token": self.csrf, "Content-Type": "application/octet-stream"}
        return self.client.post("/v1/parent/content" + query, content=blob, headers=headers, **kw)

    def approve(self, package_id, blob, checklist=None):
        return self.client.post(f"/v1/parent/content/{package_id}/approve", headers={"X-CSRF-Token": self.csrf},
                                json={"sha256": sha(blob), "checklist": checklist or CHECKLIST})

    def publish(self, blob, query=""):
        package_id = self.upload(blob, query).json()["package"]["package_id"]
        self.assertEqual(self.approve(package_id, blob).status_code, 200)
        return package_id

    def assign(self, device, *items):
        return self.client.put(f"/v1/parent/devices/{device}/content", headers={"X-CSRF-Token": self.csrf},
                               json={"items": [{"package_id": i, "wanted": w} for i, w in items]})

    # device helpers
    def auth(self, token=TOKEN_A):
        return {"Authorization": f"Bearer {token}"}

    def manifest(self, device="pet-a", token=TOKEN_A):
        return self.client.get(f"/v1/devices/{device}/content/manifest", headers=self.auth(token))

    def fetch(self, blob, device="pet-a", token=TOKEN_A, **headers):
        return self.client.get(f"/v1/content/packages/{sha(blob)}",
                               headers={**self.auth(token), "X-Device-Id": device, **headers})


class ImportTests(Base):
    def test_valid_packs_become_drafts_and_are_stored_by_sha(self):
        for blob, kind in ((BOOK, "book"), (GAME, "game"), (SOUND, "soundpack")):
            response = self.upload(blob)
            self.assertEqual(response.status_code, 201, response.text)
            package = response.json()["package"]
            self.assertEqual((package["kind"], package["status"], package["sha256"]), (kind, "draft", sha(blob)))
            self.assertEqual(self.catalog.package_path(sha(blob)).read_bytes(), blob)
        self.assertEqual(self.upload(BOOK).json()["created"], False)
        self.assertEqual(len(self.client.get("/v1/parent/content").json()["packages"]), 3)

    def test_card_paths(self):
        self.assertEqual(self.upload(BOOK).json()["package"]["path"], "/books/c101.kpb")
        self.assertEqual(self.upload(GAME, "?pack_id=default").json()["package"]["path"], "/games/default.kpg")
        self.assertEqual(self.upload(SOUND).json()["package"]["path"], "/sounds/default.kps")

    def test_bad_and_unsupported_packs_rejected(self):
        broken = bytearray(BOOK)
        broken[-5] ^= 0xFF
        for blob, status, code in ((b"", 415, "UNSUPPORTED_PACK_FORMAT"),
                                   (b"MZ" + b"\0" * 200, 415, "UNSUPPORTED_PACK_FORMAT"),
                                   (KPB1, 415, "UNSUPPORTED_PACK_FORMAT"),
                                   (bytes(broken), 422, "PACK_INVALID"),
                                   (BOOK[:5000], 422, "PACK_INVALID"),
                                   (GAME[:-4], 422, "PACK_INVALID"),
                                   (SOUND + b"\0\0\0\0", 422, "PACK_INVALID")):
            response = self.upload(blob)
            self.assertEqual((response.status_code, response.json()["detail"]), (status, code))
        self.assertEqual(self.client.get("/v1/parent/content").json()["packages"], [])
        self.assertEqual(list(self.catalog.packages_dir.iterdir()), [])
        self.assertEqual(self.upload(SOUND, "?pack_id=../x").json()["detail"], "PACK_ID_INVALID")
        self.assertEqual(self.upload(BOOK, "?pack_id=abc").json()["detail"], "PACK_ID_INVALID")

    def test_size_cap_and_media_type(self):
        with patch.object(distribution, "MAX_UPLOAD_BYTES", 1000), patch.object(content, "MAX_UPLOAD_BYTES", 1000):
            self.assertEqual(self.upload(b"x" * 1001).status_code, 413)
        self.assertEqual(self.client.post("/v1/parent/content", content=BOOK,
                                          headers={"X-CSRF-Token": self.csrf}).status_code, 415)
        self.assertEqual(content.MAX_UPLOAD_BYTES, 64 * 1024 * 1024)

    def test_parent_auth_required_and_device_token_not_accepted(self):
        anon = TestClient(self.app, base_url="https://testserver")
        self.addCleanup(anon.close)
        for method, path in (("GET", "/v1/parent/content"), ("POST", "/v1/parent/content/1/approve"),
                             ("POST", "/v1/parent/content/1/retire"), ("GET", "/v1/parent/devices/pet-a/content"),
                             ("PUT", "/v1/parent/devices/pet-a/content")):
            response = anon.request(method, path, headers=self.auth(), json={"x": 1} if method != "GET" else None)
            self.assertIn(response.status_code, (401, 422), path)
        self.assertEqual(anon.post("/v1/parent/content", content=BOOK, headers={
            **self.auth(), "Content-Type": "application/octet-stream"}).status_code, 401)
        no_csrf = self.client.post("/v1/parent/content", content=BOOK,
                                   headers={"Content-Type": "application/octet-stream"})
        self.assertEqual(no_csrf.status_code, 403)

    def test_cli_import(self):
        file = Path(self.tmp.name) / "book.kpb"
        file.write_bytes(BOOK)
        with patch("builtins.print"):
            self.assertEqual(content.main(["--db", str(self.path), "import-pack", str(file)]), 0)
        self.assertEqual(self.catalog.list()[0]["status"], "draft")


class ApprovalTests(Base):
    def test_approval_is_bound_to_sha256(self):
        package_id = self.upload(BOOK).json()["package"]["package_id"]
        wrong = self.client.post(f"/v1/parent/content/{package_id}/approve", headers={"X-CSRF-Token": self.csrf},
                                 json={"sha256": "0" * 64, "checklist": CHECKLIST})
        self.assertEqual((wrong.status_code, wrong.json()["detail"]), (409, "SHA256_MISMATCH"))
        for checklist in ({"a": True, "b": False},):
            self.assertEqual(self.approve(package_id, BOOK, checklist).json()["detail"], "CHECKLIST_INCOMPLETE")
        self.assertEqual(self.client.post(f"/v1/parent/content/{package_id}/approve",
                                          headers={"X-CSRF-Token": self.csrf},
                                          json={"sha256": sha(BOOK), "checklist": {}}).status_code, 422)
        self.assertEqual(self.catalog.get(package_id)["status"], "draft")
        result = self.approve(package_id, BOOK).json()["package"]
        self.assertEqual((result["status"], result["reviewed_by"]), ("approved", "parent"))
        self.assertEqual(self.approve(package_id, BOOK).json()["detail"], "ALREADY_APPROVED")

    def test_one_changed_byte_is_a_new_unreviewed_package(self):
        first = self.publish(BOOK)
        self.assertEqual(self.upload(book_revision(1)).json()["package"]["package_id"], first,
                         "identical bytes are the same package")
        edited = self.upload(book_revision(2)).json()["package"]
        self.assertNotEqual(edited["package_id"], first)
        self.assertEqual(edited["status"], "draft")
        self.assertNotEqual(edited["sha256"], sha(BOOK))

    def test_stored_file_tampering_blocks_approval(self):
        package_id = self.upload(BOOK).json()["package"]["package_id"]
        path = self.catalog.package_path(sha(BOOK))
        data = bytearray(path.read_bytes())
        data[200] ^= 0xFF
        path.write_bytes(bytes(data))
        self.assertEqual(self.approve(package_id, BOOK).json()["detail"], "PACKAGE_FILE_CORRUPT")
        self.assertEqual(self.catalog.get(package_id)["status"], "draft")

    def test_unapproved_is_invisible_everywhere(self):
        package_id = self.upload(BOOK).json()["package"]["package_id"]
        self.assertEqual(self.assign("pet-a", (package_id, "install")).json()["detail"], "PACKAGE_NOT_APPROVED")
        self.assertEqual(self.manifest().json()["install"], [])
        self.assertEqual(self.fetch(BOOK).status_code, 404)
        self.assertIsNone(self.catalog.page_text(101, 1, 1))

    def test_content_pages_lookup_by_content_id_and_revision(self):
        self.publish(BOOK)
        parsed = kpb2.parse(BOOK)
        crc = self.catalog.list()[0]["header_crc"]
        for page in parsed["pages"]:
            self.assertEqual(self.catalog.page_text(101, 1, page["number"]), page["text"])
        self.assertEqual(self.catalog.page_text(101, 1, 1, header_crc=crc), parsed["pages"][0]["text"])
        self.assertIsNone(self.catalog.page_text(101, 1, 1, header_crc=crc ^ 1), "device copy differs")
        self.assertIsNone(self.catalog.page_text(101, 2, 1))
        self.assertIsNone(self.catalog.page_text(102, 1, 1))
        self.assertIsNone(self.catalog.page_text(101, 1, 99))
        self.assertEqual(self.catalog.approved_by_fingerprint("book", 1, crc, 101)["sha256"], sha(BOOK))
        self.assertIsNone(self.catalog.approved_by_fingerprint("book", 1, crc, 7))

    def test_like_names_derived_from_the_reviewed_book(self):
        self.publish(BOOK)
        names = self.catalog.like_names()
        self.assertEqual(names[1]["label"], "最喜欢的动物")
        self.assertEqual(names[1]["options"], {3: "小兔子", 4: "小鸟"})
        self.assertEqual(names[1]["key"], "favorite_animal")

    def test_new_revision_retires_old_and_upgrades_assignments(self):
        old = self.publish(BOOK)
        self.assign("pet-a", (old, "install"))
        calls = []
        self.catalog.on_sync_needed(calls.append)
        new = self.upload(book_revision(2)).json()["package"]["package_id"]
        self.assertEqual(self.approve(new, book_revision(2)).status_code, 200)
        self.assertEqual(calls, ["pet-a"])
        self.assertEqual(self.catalog.get(old)["status"], "retired")
        entries = self.manifest().json()["install"]
        self.assertEqual([(e["sha256"], e["revision"]) for e in entries], [(sha(book_revision(2)), 2)])
        self.assertIsNone(self.catalog.page_text(101, 1, 1), "retired revision gives no text")
        self.assertEqual(self.catalog.page_text(101, 2, 1), kpb2.parse(BOOK)["pages"][0]["text"])
        self.assertEqual(self.fetch(BOOK).status_code, 404, "old package no longer downloadable")
        # An older revision can't be approved over the newer one.
        older = self.upload(book_revision(1)).json()["package"]["package_id"]
        self.assertEqual(self.approve(older, book_revision(1)).json()["detail"], "REVISION_NOT_NEWER")

    def test_retire_marks_installed_for_removal_and_drops_pending(self):
        book, game = self.publish(BOOK), self.publish(GAME)
        self.assign("pet-a", (book, "install"), (game, "install"))
        self.client.post("/v1/devices/pet-a/content/report", headers=self.auth(),
                         json={"sha256": sha(BOOK), "state": "installed"})
        for package in (book, game):
            self.assertEqual(self.client.post(f"/v1/parent/content/{package}/retire",
                                              headers={"X-CSRF-Token": self.csrf}).status_code, 200)
        manifest = self.manifest().json()
        self.assertEqual(manifest["install"], [])
        self.assertEqual([e["sha256"] for e in manifest["remove"]], [sha(BOOK)])
        self.assertEqual(self.client.post(f"/v1/parent/content/{book}/retire",
                                          headers={"X-CSRF-Token": self.csrf}).json()["detail"], "ALREADY_RETIRED")


class ManifestTests(Base):
    def test_manifest_lists_only_this_devices_approved_packages(self):
        book, sound = self.publish(BOOK), self.publish(SOUND)
        self.assign("pet-a", (book, "install"), (sound, "install"))
        self.assign("pet-b", (sound, "install"))
        a = self.manifest().json()
        self.assertEqual([e["kind"] for e in a["install"]], ["book", "soundpack"])
        entry = a["install"][0]
        self.assertEqual(entry, {"kind": "book", "content_id": 101, "revision": 1, "sha256": sha(BOOK),
                                 "header_crc": struct.unpack_from("<I", BOOK, 92)[0], "bytes": len(BOOK),
                                 "path": "/books/c101.kpb", "url": f"/v1/content/packages/{sha(BOOK)}"})
        self.assertEqual([e["kind"] for e in self.manifest("pet-b", TOKEN_B).json()["install"]], ["soundpack"])

    def test_device_a_cannot_read_or_fetch_device_b_assignment(self):
        book = self.publish(BOOK)
        self.assign("pet-b", (book, "install"))
        self.assertEqual(self.manifest("pet-b", TOKEN_A).status_code, 401)
        self.assertEqual(self.manifest("pet-b", "x" * 40).status_code, 401)
        self.assertEqual(self.client.get("/v1/devices/pet-b/content/manifest").status_code, 401)
        self.assertEqual(self.manifest("pet-a").json()["install"], [])
        self.assertEqual(self.fetch(BOOK, "pet-a", TOKEN_A).status_code, 404)
        self.assertEqual(self.fetch(BOOK, "pet-b", TOKEN_A).status_code, 401)
        self.assertEqual(self.fetch(BOOK, "pet-b", TOKEN_B).status_code, 200)
        self.assertEqual(self.client.get(f"/v1/content/packages/{sha(BOOK)}", headers=self.auth()).status_code, 401)
        self.assertEqual(self.client.get(f"/v1/content/packages/{sha(BOOK)}").status_code, 401)
        self.assertEqual(self.client.post("/v1/devices/pet-b/content/report", headers=self.auth(),
                                          json={"sha256": sha(BOOK), "state": "installed"}).status_code, 401)

    def test_assignment_validation_and_sync_hook(self):
        book = self.publish(BOOK)
        calls = []
        self.catalog.on_sync_needed(calls.append)
        self.assertEqual(self.assign("nope-1", (book, "install")).status_code, 404)
        self.assertEqual(self.assign("pet-a", (999, "install")).status_code, 404)
        self.assertEqual(self.assign("pet-a", (book, "remove")).json()["detail"], "NOT_ASSIGNED")
        self.assertEqual(calls, [])
        view = self.assign("pet-a", (book, "install")).json()
        self.assertEqual(calls, ["pet-a"])
        self.assertEqual([(a["wanted"], a["state"]) for a in view["assignments"]], [("install", "pending")])
        self.assertEqual(self.client.put("/v1/parent/devices/pet-a/content", headers={"X-CSRF-Token": self.csrf},
                                         json={"items": [{"package_id": book, "wanted": "bogus"}]}).status_code, 422)

    def test_hook_failure_does_not_undo_the_assignment(self):
        book = self.publish(BOOK)
        self.catalog.on_sync_needed(lambda device: 1 / 0)
        self.assertEqual(self.assign("pet-a", (book, "install")).status_code, 200)
        self.assertEqual(len(self.manifest().json()["install"]), 1)

    def test_no_space_withholds_and_parent_sees_it(self):
        book, game = self.publish(BOOK), self.publish(GAME)
        self.assign("pet-a", (book, "install"), (game, "install"))
        room = (len(BOOK) // 1024) + 1024 + 1  # fits the book (plus reserve) but not both
        self.client.put("/v1/devices/pet-a/content/inventory", headers=self.auth(),
                        json={"items": [], "sd_free_kib": room})
        manifest = self.manifest().json()
        self.assertEqual([e["sha256"] for e in manifest["install"]], [sha(BOOK)])
        self.assertEqual(manifest["withheld"], [{"sha256": sha(GAME), "reason": "NO_SPACE", "bytes": len(GAME)}])
        view = self.client.get("/v1/parent/devices/pet-a/content").json()
        reasons = {a["sha256"]: a["reason"] for a in view["assignments"]}
        self.assertEqual(reasons[sha(GAME)], "NO_SPACE")
        self.assertEqual(view["card"]["sd_free_kib"], room)
        self.client.put("/v1/devices/pet-a/content/inventory", headers=self.auth(),
                        json={"items": [], "sd_free_kib": 10 ** 6})
        self.assertEqual(len(self.manifest().json()["install"]), 2)


class RangeTests(Base):
    def setUp(self):
        super().setUp()
        self.book = self.publish(BOOK)
        self.assign("pet-a", (self.book, "install"))
        self.size = len(BOOK)

    def test_full_download_headers_and_hash(self):
        response = self.fetch(BOOK)
        self.assertEqual(response.status_code, 200)
        self.assertEqual(sha(response.content), sha(BOOK))
        for name, value in (("etag", f'"{sha(BOOK)}"'), ("accept-ranges", "bytes"),
                            ("content-length", str(self.size)),
                            ("content-type", "application/octet-stream"),
                            ("content-disposition", f'attachment; filename="{sha(BOOK)}.bin"')):
            self.assertEqual(response.headers[name], value)

    def test_ranges(self):
        n = self.size
        cases = [("bytes=0-99", 0, 99), ("bytes=100-", 100, n - 1), ("bytes=-50", n - 50, n - 1),
                 (f"bytes={n - 1}-{n + 999}", n - 1, n - 1), ("bytes=10-10", 10, 10),
                 (f"bytes=-{n + 500}", 0, n - 1), ("BYTES=5-9", 5, 9)]
        for header, first, last in cases:
            response = self.fetch(BOOK, Range=header)
            self.assertEqual(response.status_code, 206, header)
            self.assertEqual(response.content, BOOK[first:last + 1], header)
            self.assertEqual(response.headers["content-range"], f"bytes {first}-{last}/{n}")
            self.assertEqual(response.headers["content-length"], str(last - first + 1))

    def test_unsatisfiable_ranges_get_416(self):
        n = self.size
        for header in (f"bytes={n}-", f"bytes={n + 10}-{n + 20}", "bytes=-0"):
            response = self.fetch(BOOK, Range=header)
            self.assertEqual(response.status_code, 416, header)
            self.assertEqual(response.headers["content-range"], f"bytes */{n}")
            self.assertEqual(response.content, b"")

    def test_malformed_or_unsupported_ranges_are_ignored(self):
        for header in ("bytes=abc", "bytes=-", "bytes=5-2", "items=0-5", "bytes=0-1,5-6", "bytes 0-5", ""):
            response = self.fetch(BOOK, Range=header)
            self.assertEqual((response.status_code, len(response.content)), (200, self.size), header)

    def test_if_range(self):
        stale = self.fetch(BOOK, Range="bytes=0-9", **{"If-Range": '"deadbeef"'})
        self.assertEqual((stale.status_code, len(stale.content)), (200, self.size))
        fresh = self.fetch(BOOK, Range="bytes=0-9", **{"If-Range": f'"{sha(BOOK)}"'})
        self.assertEqual((fresh.status_code, len(fresh.content)), (206, 10))

    def test_resume_reassembles_exact_file(self):
        first = self.fetch(BOOK, Range="bytes=0-70000").content
        rest = self.fetch(BOOK, Range=f"bytes={len(first)}-")
        self.assertEqual(sha(first + rest.content), sha(BOOK))

    def test_head_and_bad_ids(self):
        head = self.client.head(f"/v1/content/packages/{sha(BOOK)}",
                                headers={**self.auth(), "X-Device-Id": "pet-a"})
        self.assertEqual((head.status_code, head.headers["content-length"], head.content), (200, str(self.size), b""))
        for ident in ("../etc/passwd", "ABC", "0" * 63, "0" * 64):
            response = self.client.get(f"/v1/content/packages/{ident}", headers={**self.auth(), "X-Device-Id": "pet-a"})
            self.assertEqual(response.status_code, 404, ident)

    def test_one_download_at_a_time_per_device(self):
        lease = self.app.state.content_distribution.acquire("pet-a")
        busy = self.fetch(BOOK)
        self.assertEqual((busy.status_code, busy.headers["retry-after"]), (429, "5"))
        self.app.state.content_distribution.release("pet-a", lease)
        self.assertEqual(self.fetch(BOOK).status_code, 200)
        self.assertEqual(self.fetch(BOOK).status_code, 200, "lease released after streaming")

    def test_missing_or_resized_file_is_503(self):
        self.catalog.package_path(sha(BOOK)).write_bytes(b"short")
        self.assertEqual(self.fetch(BOOK).status_code, 503)
        self.catalog.package_path(sha(BOOK)).unlink()
        self.assertEqual(self.fetch(BOOK).status_code, 503)

    def test_streams_in_chunks_without_loading_the_file(self):
        with patch.object(distribution, "CHUNK", 1024):
            reads = []
            real_open = open

            class Spy:
                def __init__(self, handle):
                    self.handle = handle

                def __getattr__(self, name):
                    return getattr(self.handle, name)

                def read(self, size=-1):
                    reads.append(size)
                    return self.handle.read(size)

            with patch("builtins.open", lambda *a, **k: Spy(real_open(*a, **k))
                       if str(a[0]).endswith(sha(BOOK)) else real_open(*a, **k)):
                self.assertEqual(sha(self.fetch(BOOK).content), sha(BOOK))
        self.assertTrue(reads and max(reads) <= 1024 and len(reads) > 100)


class ReportTests(Base):
    def setUp(self):
        super().setUp()
        self.book = self.publish(BOOK)
        self.assign("pet-a", (self.book, "install"))

    def report(self, state, token=TOKEN_A, device="pet-a", **extra):
        return self.client.post(f"/v1/devices/{device}/content/report", headers=self.auth(token),
                                json={"sha256": sha(BOOK), "state": state, **extra})

    def state(self):
        return self.client.get("/v1/parent/devices/pet-a/content").json()["assignments"][0]

    def test_install_results_show_in_parent_view(self):
        self.assertEqual(self.report("downloading").status_code, 200)
        self.assertEqual(self.state()["state"], "downloading")
        self.assertEqual(self.report("failed").status_code, 422)
        self.assertEqual(self.report("failed", reason="SHA256_MISMATCH", attempts=3).status_code, 200)
        self.assertEqual((self.state()["state"], self.state()["reason"], self.state()["attempts"]),
                         ("failed", "SHA256_MISMATCH", 3))
        self.assertEqual(self.report("installed").status_code, 200)
        self.assertEqual((self.state()["state"], self.state()["reason"]), ("installed", None))

    def test_report_validation(self):
        self.assertEqual(self.report("removed").json()["detail"], "ASSIGNMENT_NOT_ACTIVE")
        self.assertEqual(self.report("installed", reason="lowercase").status_code, 422)
        self.assertEqual(self.client.post("/v1/devices/pet-a/content/report", headers=self.auth(),
                                          json={"sha256": "0" * 64, "state": "installed"}).json()["detail"],
                         "NOT_ASSIGNED")
        self.assertEqual(self.client.post("/v1/devices/pet-b/content/report", headers=self.auth(TOKEN_B),
                                          json={"sha256": sha(BOOK), "state": "installed"}).json()["detail"],
                         "NOT_ASSIGNED")
        self.assertEqual(self.report("installed", extra="x").status_code, 422)

    def test_removal_flow_forgets_the_package(self):
        self.report("installed")
        self.assign("pet-a", (self.book, "remove"))
        entry = self.manifest().json()["remove"][0]
        self.assertEqual((entry["sha256"], entry["path"], "url" in entry), (sha(BOOK), "/books/c101.kpb", False))
        self.assertEqual(self.report("installed").json()["detail"], "ASSIGNMENT_NOT_ACTIVE")
        self.assertEqual(self.report("removed").status_code, 200)
        self.assertEqual(self.client.get("/v1/parent/devices/pet-a/content").json()["assignments"], [])

    def test_inventory_reconciliation(self):
        crc = struct.unpack_from("<I", BOOK, 92)[0]
        item = lambda **k: {"kind": "book", "content_id": 101, "revision": 1, "header_crc": crc, **k}
        body = {"items": [item(), item(content_id=77, revision=1, header_crc=5),
                          {"kind": "game", "content_id": 900, "revision": 9, "header_crc": 1}],
                "sd_free_kib": 4096}
        checked = self.client.put("/v1/devices/pet-a/content/inventory", headers=self.auth(), json=body).json()["items"]
        self.assertEqual([c["review"] for c in checked], ["approved", "unreviewed", "unreviewed"])
        self.assertEqual(self.state()["state"], "installed", "card already has it")
        # card swapped: the book disappeared, so it must be fetched again
        self.client.put("/v1/devices/pet-a/content/inventory", headers=self.auth(), json={"items": [], "sd_free_kib": 4096})
        self.assertEqual(self.state()["state"], "pending")
        # retired packs on a card are 'outdated'
        self.client.post(f"/v1/parent/content/{self.book}/retire", headers={"X-CSRF-Token": self.csrf})
        again = self.client.put("/v1/devices/pet-a/content/inventory", headers=self.auth(),
                                json={"items": [item()]}).json()["items"]
        self.assertEqual(again[0]["review"], "outdated")
        card = self.client.get("/v1/parent/devices/pet-a/content").json()["card"]
        self.assertEqual(card["items"][0]["review"], "outdated")

    def test_soundpack_matches_by_revision_and_crc_only(self):
        sound = self.publish(SOUND)
        crc = struct.unpack_from("<I", SOUND, 28)[0]
        checked = self.client.put("/v1/devices/pet-a/content/inventory", headers=self.auth(), json={
            "items": [{"kind": "soundpack", "revision": 3, "header_crc": crc}]}).json()["items"]
        self.assertEqual(checked[0]["review"], "approved")
        self.assertEqual(self.catalog.approved_by_fingerprint("soundpack", 3, crc)["package_id"], sound)

    def test_inventory_is_validated(self):
        for body in ({"items": [{"kind": "costume", "revision": 1, "header_crc": 1}]},
                     {"items": [{"kind": "book", "revision": -1, "header_crc": 1}]},
                     {"items": [], "sd_free_kib": -1}, {"items": [], "extra": 1}):
            self.assertEqual(self.client.put("/v1/devices/pet-a/content/inventory", headers=self.auth(),
                                             json=body).status_code, 422)

    def test_audit_entries_without_secrets(self):
        self.report("installed")
        self.fetch(BOOK)
        with connect(self.path) as db:
            rows = [dict(r) for r in db.execute("SELECT * FROM audit_log WHERE action LIKE 'content.%'")]
        actions = {r["action"] for r in rows}
        self.assertTrue({"content.import", "content.approve", "content.assign", "content.report.installed",
                         "content.download"} <= actions, actions)
        text = str(rows)
        for secret in (TOKEN_A, TOKEN_B, self.csrf, PASSWORD):
            self.assertNotIn(secret, text)
        self.assertEqual({r["actor"] for r in rows if r["action"].startswith("content.report")}, {"device"})

    def test_denied_download_is_audited(self):
        self.fetch(GAME)
        with connect(self.path) as db:
            result = db.execute("SELECT result FROM audit_log WHERE action='content.download'").fetchone()[0]
        self.assertEqual(result, "denied")


class FakeDeviceTests(Base):
    def device(self, card, token=TOKEN_A, **kw):
        def send(method, path, headers, body=None):
            with self.client.stream(method, path, headers=headers, content=body) as response:
                return fake_device.Response(response.status_code, dict(response.headers),
                                            iter(list(response.iter_bytes(4096))))
        return fake_device.FakeDevice(send, "pet-a", token, Path(card), log=lambda *a: None, **kw)

    def setUp(self):
        super().setUp()
        self.card = Path(self.tmp.name) / "card"
        self.card.mkdir()
        self.book, self.sound = self.publish(BOOK), self.publish(SOUND)
        self.assign("pet-a", (self.book, "install"), (self.sound, "install"))

    def states(self):
        rows = self.client.get("/v1/parent/devices/pet-a/content").json()["assignments"]
        return {r["kind"]: r["state"] for r in rows}

    def test_full_cycle_download_verify_install_report(self):
        result = self.device(self.card).run()
        self.assertEqual((sorted(result["installed"]), result["failed"], result["inventory_status"]),
                         (sorted([sha(BOOK), sha(SOUND)]), {}, 200))
        self.assertEqual((self.card / "books/c101.kpb").read_bytes(), BOOK)
        self.assertEqual((self.card / "sounds/default.kps").read_bytes(), SOUND)
        self.assertEqual(list(self.card.rglob("*.part")), [])
        self.assertEqual(self.states(), {"book": "installed", "soundpack": "installed"})
        card = self.client.get("/v1/parent/devices/pet-a/content").json()["card"]
        self.assertEqual({i["review"] for i in card["items"]}, {"approved"})
        again = self.device(self.card).run()  # idempotent
        self.assertEqual((again["installed"], len(again["skipped"])), ([], 2))

    def test_interrupted_download_resumes_with_range(self):
        interrupted = self.device(self.card, max_bytes=50000).run()
        self.assertEqual(interrupted["failed"], {sha(BOOK): "INTERRUPTED"})
        part = self.card / "books/c101.kpb.part"
        self.assertTrue(0 < part.stat().st_size <= 50000)
        self.assertFalse((self.card / "books/c101.kpb").exists(), "not renamed before verification")
        seen = []
        original = self.client.stream
        with patch.object(self.client, "stream", lambda m, p, **k: (seen.append(k["headers"].get("Range")),
                                                                   original(m, p, **k))[1]):
            result = self.device(self.card).run()
        self.assertIn(sha(BOOK), result["installed"])
        self.assertTrue(any(r and r.startswith("bytes=") and r != "bytes=0-" for r in seen), seen)
        self.assertEqual((self.card / "books/c101.kpb").read_bytes(), BOOK)

    def test_corrupt_part_and_server_mismatch_never_get_the_final_name(self):
        part = self.card / "books"
        part.mkdir()
        (part / "c101.kpb.part").write_bytes(b"garbage" * 100)
        result = self.device(self.card).run()
        self.assertEqual((self.card / "books/c101.kpb").read_bytes(), BOOK, "bad prefix detected, retried")
        self.assertIn(sha(BOOK), result["installed"])
        # The stored file silently changes (same size): every attempt fails the sha check.
        other = Path(self.tmp.name) / "card2"
        other.mkdir()
        path = self.catalog.package_path(sha(BOOK))
        flipped = bytearray(BOOK)
        flipped[300] ^= 0xFF
        path.write_bytes(bytes(flipped))
        failed = self.device(other).run()
        self.assertEqual(failed["failed"], {sha(BOOK): "SHA256_MISMATCH"})
        self.assertFalse((other / "books/c101.kpb").exists())
        self.assertEqual(list(other.rglob("*.part")), [])
        row = [a for a in self.client.get("/v1/parent/devices/pet-a/content").json()["assignments"]
               if a["kind"] == "book"][0]
        self.assertEqual((row["state"], row["reason"], row["attempts"]), ("failed", "SHA256_MISMATCH", 3))

    def test_not_enough_space_is_reported(self):
        result = self.device(self.card, sd_free_kib=100).run()
        self.assertEqual(set(result["failed"].values()), {"NO_SPACE"})
        self.assertEqual(list(self.card.rglob("*.kp*")), [])
        manifest = self.manifest().json()  # server now knows the card is nearly full
        self.assertEqual(manifest["install"], [])
        self.assertEqual(len(manifest["withheld"]), 2)

    def test_removal_only_deletes_matching_downloaded_pack(self):
        self.device(self.card).run()
        manual = self.card / "books" / "forest_friends.kpb"
        manual.write_bytes(b"KPB1" + b"\0" * 100)
        self.assign("pet-a", (self.book, "remove"))
        result = self.device(self.card).run()
        self.assertEqual(result["removed"], [sha(BOOK)])
        self.assertFalse((self.card / "books/c101.kpb").exists())
        self.assertTrue(manual.exists(), "hand-copied files are never deleted")
        self.assertEqual(self.states(), {"soundpack": "installed"})

    def test_wrong_token_cannot_pull_manifest(self):
        with self.assertRaises(RuntimeError):
            self.device(self.card, token=TOKEN_B).run()

    def test_cli_requires_env_token(self):
        with patch.dict("os.environ", {"KID_PET_DEVICE_TOKEN": ""}), patch("sys.stderr", io.StringIO()):
            self.assertEqual(fake_device.main(["--base", "http://x", "--device-id", "pet-a",
                                               "--card", str(self.card)]), 2)

    def test_card_paths_cannot_escape(self):
        with self.assertRaises(ValueError):
            fake_device.card_file(self.card, "/../../etc/passwd")


class MigrationTests(unittest.TestCase):
    def test_tables_exist_and_device_delete_cascades(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "m.sqlite3"
            catalog = content.ContentCatalog(path)
            from services.pet_learning.store import PetStore
            PetStore(path).enroll_device("pet-a", TOKEN_A)
            package, _ = catalog.import_blob(SOUND)
            with connect(path) as db:
                db.execute("INSERT INTO device_content(device_id,package_id,wanted,updated_at) "
                           "VALUES('pet-a',?,'install',1)", (package["package_id"],))
                db.execute("DELETE FROM devices WHERE device_id='pet-a'")
                self.assertEqual(db.execute("SELECT COUNT(*) FROM device_content").fetchone()[0], 0)
                self.assertEqual(db.execute("SELECT COUNT(*) FROM content_packages").fetchone()[0], 1)


if __name__ == "__main__":
    unittest.main()
