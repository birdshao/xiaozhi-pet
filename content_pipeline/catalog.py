"""Deterministic local course index from *real* KPB2/KPG1 package bytes.

This is NOT a signature, a server S4 approval or a firmware course loader.
The existing firmware still reads a single default.kpg; the index is a
machine-readable handoff artifact for future curriculum UI and distribution.
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re

from content_pipeline.schema import ContentError, SKILL, write_json
from tools import game_pack, kpb2

UNIT_ID = re.compile(r"^[a-z][a-z0-9_]{2,63}$")
COURSE_ID = re.compile(r"^[a-z][a-z0-9_.]{3,95}$")


def _object(value, keys: set[str], what: str) -> dict:
    if not isinstance(value, dict) or set(value) != keys:
        raise ContentError(f"{what}: invalid fields")
    return value


def _text(value, what: str, maximum=100) -> str:
    if not isinstance(value, str) or not value.strip() or len(value) > maximum:
        raise ContentError(f"{what}: invalid text")
    return value


def load_skills(path: Path) -> set[str]:
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    _object(data, {"schema_version", "skills"}, "skills")
    if data["schema_version"] != 1 or not isinstance(data["skills"], list):
        raise ContentError("skills: schema v1 required")
    names: set[str] = set()
    for item in data["skills"]:
        _object(item, {"id", "name", "age_band"}, "skill")
        skill = item["id"]
        if not isinstance(skill, str) or not SKILL.fullmatch(skill) or skill in names:
            raise ContentError("skill: invalid or duplicated id")
        if item["age_band"] != "3-5":
            raise ContentError("skill: unsupported age band")
        _text(item["name"], "skill.name", 80)
        names.add(skill)
    if not names:
        raise ContentError("skills: empty")
    return names


def load_course(path: Path, skills: set[str]) -> dict:
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    _object(data, {"schema_version", "course_id", "title", "age_band", "units"},
            "course")
    if data["schema_version"] != "COURSE1" or data["age_band"] != "3-5" or \
            not isinstance(data["course_id"], str) or \
            not COURSE_ID.fullmatch(data["course_id"]):
        raise ContentError("course: invalid identity/schema/age")
    _text(data["title"], "course.title", 100)
    if not isinstance(data["units"], list) or not 1 <= len(data["units"]) <= 30:
        raise ContentError("course: need 1..30 units")
    seen = set()
    for unit in data["units"]:
        _object(unit, {"unit_id", "name", "skill_id", "items"}, "unit")
        uid = unit["unit_id"]
        if not isinstance(uid, str) or not UNIT_ID.fullmatch(uid) or uid in seen:
            raise ContentError("course: invalid or duplicate unit")
        seen.add(uid)
        _text(unit["name"], "unit.name", 80)
        if unit["skill_id"] not in skills:
            raise ContentError("course: unknown skill id")
        items = unit["items"]
        if not isinstance(items, list) or not 1 <= len(items) <= 20:
            raise ContentError("course: unit requires 1..20 items")
        duplicates = set()
        for item in items:
            _object(item, {"kind", "content_id"}, "course.item")
            if item["kind"] not in ("book", "game") or \
                    type(item["content_id"]) is not int or \
                    not 1 <= item["content_id"] <= 65535:
                raise ContentError("course.item: invalid kind/content_id")
            key = (item["kind"], item["content_id"])
            if key in duplicates:
                raise ContentError("course: repeated package in one unit")
            duplicates.add(key)
    return data


def _verified_package(path: Path, *, allow_test_artifacts: bool) -> dict:
    path = Path(path)
    suffix = path.suffix
    if suffix not in (".kpb", ".kpg"):
        raise ContentError("package: expected .kpb or .kpg")
    if not path.is_file():
        raise ContentError("package: not a file")
    maximum = kpb2.MAX_PACKAGE_BYTES if suffix == ".kpb" else game_pack.MAX_BYTES
    if path.stat().st_size > maximum:
        raise ContentError("package: too large")
    data = path.read_bytes()
    sha = hashlib.sha256(data).hexdigest()
    manifest_path = path.with_suffix(".manifest.json")
    if not manifest_path.is_file() or manifest_path.stat().st_size > 512 * 1024:
        raise ContentError("package: missing or oversized manifest")
    meta = json.loads(manifest_path.read_text(encoding="utf-8"))
    if not isinstance(meta, dict) or meta.get("sha256") != sha:
        raise ContentError("package: manifest hash mismatch")
    if not isinstance(meta.get("reviewer"), str) or not meta["reviewer"].strip():
        raise ContentError("package: missing review attribution")
    if suffix == ".kpb":
        parsed = kpb2.parse(data)
        if parsed["format"] != "KPB2" or meta.get("format") != "KPB2" or \
                meta.get("approved") is not True:
            raise ContentError("package: KPB2 must be reviewed")
        kind = "book"
        count = len(parsed["pages"])
        sd_path = f"/books/{path.name}"
        if meta.get("package_bytes") != len(data):
            raise ContentError("package: book length mismatch")
        placeholder = False  # book sidecars lack this flag; still only local index
    else:
        parsed = game_pack.parse(data)
        if meta.get("format") != "KPG1" or meta.get("reviewed") is not True:
            raise ContentError("package: KPG1 must be reviewed")
        kind = "game"
        count = len(parsed["rounds"])
        if path.name != "default.kpg":
            raise ContentError("package: current firmware requires default.kpg")
        sd_path = "/games/default.kpg"
        if meta.get("bytes") != len(data) or meta.get("sd_path") != sd_path:
            raise ContentError("package: game size/path mismatch")
        placeholder = meta.get("placeholder") is True
        if placeholder and not allow_test_artifacts:
            raise ContentError("package: test tone placeholders are not child content")
    if meta.get("content_id") != parsed["content_id"] or \
            meta.get("revision") != parsed["revision"] or \
            meta.get("package_type") != kind:
        raise ContentError("package: sidecar/header mismatch")
    title = _text(meta.get("title"), "package.title", 180)
    return {
        "kind": kind, "content_id": parsed["content_id"],
        "revision": parsed["revision"], "title": title,
        "sha256": sha, "bytes": len(data), "sd_path": sd_path,
        "item_count": count, "test_placeholder": placeholder,
    }


def build_index(course_file: Path, skills_file: Path, packages: list[Path],
                output: Path, *, allow_test_artifacts: bool = False) -> dict:
    """Bind declared units to binary-verified packages. No publication action."""
    course = load_course(course_file, load_skills(skills_file))
    if not packages or len(packages) > 64:
        raise ContentError("catalog: expected 1..64 packages")
    found: dict[tuple[str, int], dict] = {}
    locations: set[str] = set()
    for path in packages:
        pkg = _verified_package(path, allow_test_artifacts=allow_test_artifacts)
        key = (pkg["kind"], pkg["content_id"])
        if key in found or pkg["sd_path"] in locations:
            raise ContentError("catalog: repeated content_id or SD path")
        found[key] = pkg
        locations.add(pkg["sd_path"])
    used = set()
    units = []
    for unit in course["units"]:
        refs = []
        for req in unit["items"]:
            key = (req["kind"], req["content_id"])
            if key not in found:
                raise ContentError(f"catalog: missing package {key}")
            used.add(key)
            p = found[key]
            refs.append({
                "kind": p["kind"], "content_id": p["content_id"],
                "revision": p["revision"], "sha256": p["sha256"],
                "sd_path": p["sd_path"],
            })
        units.append({
            "unit_id": unit["unit_id"], "name": unit["name"],
            "skill_id": unit["skill_id"], "items": refs,
        })
    if set(found) != used:
        raise ContentError("catalog: unreferenced packages")
    status = "test_only" if allow_test_artifacts else "local_unverified"
    index = {
        "schema_version": "COURSEINDEX1",
        "status": status,
        "warning": "Not a device-install manifest, S4 approval, or a signature",
        "course_id": course["course_id"], "title": course["title"],
        "age_band": course["age_band"],
        "packages": sorted(found.values(), key=lambda p: (p["kind"], p["content_id"])),
        "units": units,
    }
    write_json(Path(output), index)
    return index
