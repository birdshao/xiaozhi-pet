"""LCIR1 import contracts; all fixtures synthetic and require no network."""
from __future__ import annotations

import contextlib
import copy
from io import StringIO
import json
from pathlib import Path
import tempfile
import unittest

from content_pipeline.adapters import import_bookdash, import_global_asp
from content_pipeline.cli import main
from content_pipeline.schema import (ContentError, content_digest,
                                     register_document, validate_document)

SHA = "a" * 40
ASP_PATH = "zh/0008_你在做什么.md"
ASP_URL = "https://github.com/global-asp/global-asp/blob/" + SHA + "/" + ASP_PATH
DASH_PATH = "sample-story/en/index.md"
DASH_URL = "https://github.com/bookdash/bookdash-books/blob/" + SHA + "/" + DASH_PATH

ASP = """# 你在做什么？

##
我在唱歌。

##
她在挥手。

##
* License: [CC-BY]
* Text: Nina Orange
* Illustration: Wiehan de Jager
* Translation: dohliam
* Language: zh
"""

DASH = """---
stylesheet: screen.css
title: "My Tiny Sample"
published: true
layout: default
---
# My Tiny Sample

![A small cat]({{ site.image-set }}/01.jpg)

A cat sees the sun.

![A puppy]({{ site.image-set }}/02.jpg)

A puppy waves hello.
"""


def asp(*, text: str = ASP, id: int = 1001, revision: int = 1,
        license_id: str = "unknown") -> dict:
    return import_global_asp(
        text, source_item_id=ASP_PATH, source_url=ASP_URL,
        source_ref=SHA, content_id=id, revision=revision,
        skills=["language.action_verbs"], license_id=license_id)


def dash(*, text: str = DASH, id: int = 1002) -> dict:
    return import_bookdash(
        text, source_item_id=DASH_PATH, source_url=DASH_URL,
        source_ref=SHA, content_id=id, skills=["language.story_sequence"],
        license_id="CC-BY-4.0")


class ParserTests(unittest.TestCase):
    def test_global_asp_pages_credits_and_unknown_license(self):
        result = asp()
        self.assertEqual(result["title"], "你在做什么？")
        self.assertEqual([p["text"] for p in result["pages"]],
                         ["我在唱歌。", "她在挥手。"])
        self.assertEqual(result["source"]["license_id"], "unknown")
        self.assertEqual(result["source"]["license_hint"], "CC-BY")
        self.assertEqual(result["source"]["creators"]["translation"], ["dohliam"])
        self.assertEqual(result["review"]["status"], "draft")
        self.assertTrue(all(p["image"] is None and p["audio"] is None
                            for p in result["pages"]))

    def test_exact_license_must_match_asp_footer(self):
        self.assertEqual(asp(license_id="CC-BY-3.0")["source"]["license_id"],
                         "CC-BY-3.0")
        with self.assertRaisesRegex(ContentError, "conflicts"):
            asp(license_id="CC-BY-NC-3.0")

    def test_reject_missing_metadata_and_pages(self):
        with self.assertRaisesRegex(ContentError, "metadata"):
            asp(text=ASP.replace("* Illustration: Wiehan de Jager\n", ""))
        with self.assertRaisesRegex(ContentError, "separators"):
            asp(text=ASP.replace("##", "#"))
        with self.assertRaisesRegex(ContentError, "unexpected content"):
            asp(text=ASP.replace("\n##", "\nA lost paragraph\n##", 1))

    def test_bookdash_image_text_pairing(self):
        result = dash()
        self.assertEqual(result["title"], "My Tiny Sample")
        self.assertEqual([p["image_ref"] for p in result["pages"]],
                         ["images/01.jpg", "images/02.jpg"])
        self.assertEqual([p["text"] for p in result["pages"]],
                         ["A cat sees the sun.", "A puppy waves hello."])
        self.assertEqual(result["source"]["license_id"], "CC-BY-4.0")
        self.assertEqual(result["source"]["creators"]["text"], [])

    def test_bookdash_reject_orphan_text_and_unsafe_image(self):
        with self.assertRaisesRegex(ContentError, "orphan"):
            dash(text=DASH.replace("![A small cat]", "unpaired prose\n![A small cat]"))
        with self.assertRaisesRegex(ContentError, "unsafe"):
            dash(text=DASH.replace("01.jpg", "../01.jpg"))
        with self.assertRaisesRegex(ContentError, "only local"):
            dash(text=DASH.replace("{{ site.image-set }}/01.jpg",
                                   "https://example.com/image.png"))
        # Book Dash has intentional picture-only pages. Keep the exact
        # wordless source instead of inventing text for the author.
        blank = dash(text=DASH.replace("A cat sees the sun.", ""))
        self.assertEqual(blank["pages"][0]["text"], "")
        self.assertEqual(blank["pages"][0]["image_ref"], "images/01.jpg")
        # An empty page is forbidden in a non-original/translated LCIR1.
        blank["locale"] = "zh-Hans"
        with self.assertRaisesRegex(ContentError, "empty text"):
            validate_document(blank)

    def test_deterministic_sha_and_revision_contract(self):
        a, b = asp(), asp()
        self.assertEqual(a, b)
        self.assertEqual(content_digest(a), content_digest(b))
        changed = asp(text=ASP.replace("我在唱歌", "我在跳舞"))
        self.assertNotEqual(content_digest(a), content_digest(changed))


class Contracts(unittest.TestCase):
    def test_no_implicit_approval(self):
        d = asp(license_id="CC-BY-3.0")
        d["review"]["status"] = "reviewed"
        with self.assertRaises(ContentError):
            validate_document(d)
        d["review"]["status"] = "draft"
        d["review"]["reviewer"] = "robot"
        with self.assertRaises(ContentError):
            validate_document(d)

    def test_review_stale_hash_rejected(self):
        d = asp(license_id="CC-BY-3.0")
        for p in d["pages"]:
            p["image"], p["audio"] = "page.png", "page.wav"
        d["review"] = {"status": "reviewed", "reviewer": "adult",
                       "checked_at": "2026-11-06", "checked_hash": None, "notes": None}
        d["review"]["checked_hash"] = content_digest(d)
        validate_document(d)
        d["pages"][0]["text"] += " 新文字"
        with self.assertRaisesRegex(ContentError, "stale"):
            validate_document(d)

    def test_no_extra_fields_or_unsafe_paths(self):
        d = asp()
        d["executable"] = "danger"
        with self.assertRaises(ContentError):
            validate_document(d)
        d = asp()
        d["source"]["source_files"][0]["path"] = "../../outside.md"
        with self.assertRaisesRegex(ContentError, "unsafe"):
            validate_document(d)
        d = dash()
        d["pages"][0]["image_ref"] = "images/../secrets.png"
        with self.assertRaisesRegex(ContentError, "unsafe"):
            validate_document(d)

    def test_registry_reject_duplicate_id_and_changed_revision(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "ids.json"
            one = asp()
            register_document(path, one)
            register_document(path, copy.deepcopy(one))  # idempotent
            with self.assertRaisesRegex(ContentError, "already claimed"):
                register_document(path, dash(id=one["content_id"]))
            with self.assertRaisesRegex(ContentError, "revision changed"):
                register_document(path, asp(text=ASP.replace("我在唱歌", "我在跑步")))
            register_document(path, asp(text=ASP.replace("我在唱歌", "我在跑步"),
                                        revision=2))
            state = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(len(state["items"][one["item_uid"]]["revisions"]), 2)

    def test_cli_import_and_check_draft(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            src = root / "story.md"
            dst = root / "normalized.json"
            registry = root / "ids.json"
            src.write_text(ASP, encoding="utf-8")
            args = ["import-global-asp", "--input", str(src),
                    "--source-path", ASP_PATH, "--ref", SHA,
                    "--content-id", "1001", "--output", str(dst),
                    "--registry", str(registry),
                    "--skill", "language.action_verbs"]
            with contextlib.redirect_stdout(StringIO()):
                code = main(args)
            self.assertEqual(code, 0)
            self.assertTrue(dst.exists())
            self.assertEqual(json.loads(dst.read_text(encoding="utf-8"))["review"]["status"],
                             "draft")
            with contextlib.redirect_stdout(StringIO()):
                self.assertEqual(main(["check", "--input", str(dst)]), 0)
            with contextlib.redirect_stderr(StringIO()):
                self.assertEqual(main(args[:-1] + ["other.skill"]), 2)
            self.assertTrue(registry.exists())


if __name__ == "__main__":
    unittest.main()
