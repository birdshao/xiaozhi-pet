"""Import pinned local source Markdown into *unreviewed* LCIR1 JSON.

Example:
    python -m content_pipeline.cli import-global-asp \
      --input research_content/sources/0008.md \
      --source-path 'zh/0008_你在做什么.md' \
      --ref 0123456789012345678901234567890123456789 \
      --content-id 1001 --skill language.action_verbs \
      --output research_content/normalized/asp-0008.json \
      --registry research_content/catalog/ids.json

The import stage NEVER downloads from the network or sets approved=true.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
from urllib.parse import quote

from content_pipeline.adapters import import_bookdash, import_global_asp
from content_pipeline.schema import (ContentError, content_digest,
                                     register_document, validate_document,
                                     write_json)


def _url(provider: str, ref: str, path: str) -> str:
    repo = ("global-asp/global-asp" if provider == "global-asp"
            else "bookdash/bookdash-books")
    return f"https://github.com/{repo}/blob/{ref}/{quote(path, safe='/')}"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    for command, provider in (("import-global-asp", "global-asp"),
                              ("import-bookdash", "bookdash")):
        p = sub.add_parser(command)
        p.set_defaults(provider=provider)
        p.add_argument("--input", type=Path, required=True,
                       help="Local UTF-8 Markdown snapshot; never fetched remotely")
        p.add_argument("--source-path", required=True,
                       help="Path inside the pinned upstream GitHub repository")
        p.add_argument("--ref", required=True, help="Full 40-digit upstream commit SHA")
        p.add_argument("--content-id", type=int, required=True,
                       help="Reserved book ID 1000..19999")
        p.add_argument("--revision", type=int, default=1)
        p.add_argument("--skill", action="append", default=[])
        p.add_argument("--license-id", default="unknown",
                       help="Exact verified license, otherwise keep unknown/draft")
        p.add_argument("--locale", default="zh-Hans" if provider == "global-asp" else "en")
        p.add_argument("--output", required=True, type=Path)
        p.add_argument("--registry", type=Path,
                       help="Stable ID registry; strongly recommended for multiple books")
    p = sub.add_parser("check")
    p.add_argument("--input", type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        if args.command == "check":
            document = validate_document(json.loads(args.input.read_text(encoding="utf-8")))
        else:
            text = args.input.read_text(encoding="utf-8")
            importer = (import_global_asp if args.provider == "global-asp"
                        else import_bookdash)
            document = importer(text, source_item_id=args.source_path,
                                source_url=_url(args.provider, args.ref, args.source_path),
                                source_ref=args.ref, content_id=args.content_id,
                                revision=args.revision, skills=args.skill,
                                locale=args.locale, license_id=args.license_id)
            if args.registry:
                register_document(args.registry, document)
            write_json(args.output, document)
        print(json.dumps({"item_uid": document["item_uid"],
                          "status": document["review"]["status"],
                          "pages": len(document["pages"]),
                          "sha256": content_digest(document)}, ensure_ascii=False))
        return 0
    except (ValueError, OSError, json.JSONDecodeError) as exc:
        print(f"CONTENT_IMPORT_ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
