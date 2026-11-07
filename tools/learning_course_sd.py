"""Create a TF-card /games/ tree from real SHA-verified KPG1 course packages.

This is a STAGING-ONLY helper: it writes a /games/ tree into a new local
directory and never writes to a TF card itself; copy that tree to the card
yourself after review. It is not a server approval service. Without
--test-only, it rejects placeholders. A trusted adult must separately review
and approve each source package. The device's KPG1 CRC is NOT a signature.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import tempfile

from content_pipeline.catalog import PACK_STEM, build_index
from content_pipeline.schema import ContentError, write_json


def stage_course_card(*, course_file: Path, skills_file: Path,
                      packages: list[Path], active: str,
                      output: Path, test_only: bool = False) -> dict:
    if not isinstance(active, str) or not PACK_STEM.fullmatch(active):
        raise ContentError("course card: unsafe active pack basename")
    output = Path(output)
    if output.exists():
        raise ContentError("course card: destination already exists")
    if not packages:
        raise ContentError("course card: no game packages provided")
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".course-stage-", dir=output.parent) as t:
        root = Path(t) / "card"
        root.mkdir()
        index_file = root / "course-index.json"
        index = build_index(
            course_file, skills_file, list(map(Path, packages)), index_file,
            allow_test_artifacts=test_only)
        if any(pkg["kind"] != "game" for pkg in index["packages"]):
            raise ContentError("course card: V1 supports KPG1 games only")
        if len(index["packages"]) > 12:
            raise ContentError("course card: at most 12 boot-selectable games")
        lookup = {Path(p["sd_path"]).stem: p for p in index["packages"]}
        if active not in lookup:
            raise ContentError("course card: active course not among supplied packages")
        folder = root / "games"
        folder.mkdir()
        for package in map(Path, packages):
            expected = lookup[package.stem]
            target = folder / package.name
            shutil.copyfile(package, target)
            if hashlib.sha256(target.read_bytes()).hexdigest() != expected["sha256"]:
                raise ContentError("course card: package changed while copying")
            shutil.copyfile(package.with_suffix(".manifest.json"),
                            target.with_suffix(".manifest.json"))
        (folder / "active.txt").write_bytes((active + "\n").encode("ascii"))
        result = {
            "schema_version": "COURSECARD1",
            "status": "test_only" if test_only else "local_unverified",
            "active": active,
            "active_kpg": f"/games/{active}.kpg",
            "packages": [
                {"name": Path(p["sd_path"]).name,
                 "content_id": p["content_id"],
                 "sha256": p["sha256"]}
                for p in index["packages"]
            ],
            "warning": (
                "This is not a signature or cloud approval. "
                "All course audio/pedagogy require adult review. "
                "Never deploy test_only placeholder packs as real lessons."
            ),
        }
        write_json(root / "card-report.json", result)
        (root / "README-CARD.txt").write_text(
            "Xiaozhi Pet LCP-10 - BOOT-ONLY multi-course KPG1 selector\n"
            "Copy games/*.kpg and games/active.txt to the matching TF directories.\n"
            "Use a plain ASCII pack stem in games/active.txt and REBOOT to select.\n"
            "No runtime hot switch. Missing/invalid active KPG1 -> default.kpg -> builtin.\n"
            "Package CRC only detects corruption; approval/signatures are separate.\n"
            + ("WARNING: TEST_ONLY placeholders! Do NOT use for children.\n"
               if test_only else "Adult content review is required.\n"),
            encoding="utf-8",
        )
        root.rename(output)
        return result


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--course", type=Path, required=True)
    p.add_argument("--skills", type=Path, required=True)
    p.add_argument("--package", action="append", type=Path, required=True)
    p.add_argument("--active", required=True)
    p.add_argument("--output-dir", type=Path, required=True)
    p.add_argument("--test-only", action="store_true")
    args = p.parse_args(argv)
    result = stage_course_card(
        course_file=args.course, skills_file=args.skills,
        packages=args.package, active=args.active,
        output=args.output_dir, test_only=args.test_only)
    print(json.dumps(result, ensure_ascii=False, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
