#!/usr/bin/env python3
"""Local parent/developer provisioning utility. No HTTP admin endpoint.

Never pass device credentials on argv; provide KID_PET_DEVICE_TOKEN via env.
Never print the token or children's personal information.
"""
from __future__ import annotations
import argparse
import getpass
import json
import time
import os
from pathlib import Path
from services.pet_learning.auth import ParentAuth
from services.pet_learning.config import Settings
from services.pet_learning.totp import new_secret, provisioning_uri
from services.pet_learning.store import PetStore
from services.pet_learning.quiz import QuizEngine
from services.pet_learning import ops


def report(problems: list[str]) -> None:
    if problems:
        print("\n".join("FAIL: " + p for p in problems))
        raise SystemExit(1)
    print("CHECK_OK")


def run_ops(args) -> None:
    key = os.environ.get("KID_PET_BACKUP_KEY", "")
    content = args.content_dir if getattr(args, "content_dir", None) else Path(
        os.environ.get("KID_PET_CONTENT_DIR") or Path(args.db).parent / "packages")
    if args.command == "backup" and bool(args.out) == bool(args.out_dir):
        raise SystemExit("backup needs exactly one of --out or --out-dir")
    try:
        if args.command == "backup":
            out = args.out or Path(args.out_dir) / time.strftime("pet-%Y%m%d-%H%M%S.enc")
            print("BACKUP_WRITTEN " + str(ops.backup(args.db, content, out, key)))
            if args.out_dir and args.keep:
                print(f"BACKUPS_ROTATED {ops.rotate_backups(args.out_dir, args.keep)}")
        elif args.command == "restore":
            ops.restore(args.archive, args.db, content, key, force=args.force)
            print("RESTORE_OK")
        elif args.command == "cleanup":
            result = ops.cleanup(args.db)
            result["pre_restore_copies"] = ops.prune_restore_copies(args.db, content, days=args.restore_copy_days)
            print(json.dumps(result, sort_keys=True))
        elif args.command == "check-config":
            for warning in ops.permission_warnings(args.env_file, Path(args.env_file).with_name("providers.env")):
                print("WARN: " + warning)
            for warning in ops.deployment_warnings({**ops.read_env(args.env_file), **os.environ}):
                print("WARN: " + warning)
            report(ops.check_env_file(args.env_file))
        else:
            text = args.config.read_text(encoding="utf-8")
            try:
                import yaml
                cfg = yaml.safe_load(text)
            except ImportError:
                cfg = json.loads(text)
            report(ops.check_xiaozhi_config(cfg or {}, args.public_origin))
    except ops.OpsError as error:
        raise SystemExit(str(error)) from None


def main() -> None:
    settings = Settings.load()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db", default=settings.database)
    sub = parser.add_subparsers(dest="command", required=True)
    parent = sub.add_parser("create-parent", help="Create the single administrator and bind TOTP locally")
    parent.add_argument("--username", required=True)
    enroll = sub.add_parser("enroll", help="Register a single new device using env credential")
    enroll.add_argument("--device-id", required=True)
    book = sub.add_parser("import-page", help="Locally import a parent-reviewed book page")
    book.add_argument("--book-id", required=True)
    book.add_argument("--title", required=True)
    book.add_argument("--page", required=True, type=int)
    book.add_argument("--text", required=True)
    book.add_argument("--approved", action="store_true", help="Explicit parent approval for this page")
    quiz = sub.add_parser("import-quiz", help="Import a manually reviewed two-choice animal card")
    quiz.add_argument("--card-id", required=True)
    quiz.add_argument("--prompt", required=True)
    quiz.add_argument("--first", required=True)
    quiz.add_argument("--second", required=True)
    quiz.add_argument("--correct", choices=(1, 2), type=int, required=True)
    quiz.add_argument("--success", required=True)
    quiz.add_argument("--retry", required=True)
    quiz.add_argument("--approved", action="store_true", help="Explicit trusted-adult approval")
    backup = sub.add_parser("backup", help="Encrypted online backup (SQLite + content dir)")
    backup.add_argument("--out", type=Path, help="exact output file (never overwritten)")
    backup.add_argument("--out-dir", type=Path, help="write pet-<timestamp>.enc here and keep the newest --keep")
    backup.add_argument("--keep", type=int, default=14)
    restore = sub.add_parser("restore", help="Restore an encrypted backup (needs --force over existing data)")
    restore.add_argument("--archive", required=True, type=Path)
    restore.add_argument("--force", action="store_true")
    cleanup = sub.add_parser("cleanup", help="Delete rows past their retention period (memory data is never touched)")
    cleanup.add_argument("--restore-copy-days", type=int, default=14, help="drop *.pre-restore-* copies older than this")
    for ops_cmd in (backup, restore, cleanup):
        ops_cmd.add_argument("--content-dir", type=Path, help="default: KID_PET_CONTENT_DIR or <db dir>/packages")
    sub.add_parser("rotate-notes-key", help="Re-encrypt every parent note with the first KID_PET_NOTES_KEY")
    check = sub.add_parser("check-config", help="Validate a production .env without starting the service")
    check.add_argument("--env-file", default=".env", type=Path)
    xz = sub.add_parser("check-xiaozhi", help="Check a rendered XiaoZhi config against the child-mode checklist")
    xz.add_argument("config", type=Path, help="YAML (or JSON) file, e.g. data/.config.yaml")
    xz.add_argument("--public-origin", default=os.environ.get("KID_PET_PUBLIC_ORIGIN", ""))
    args = parser.parse_args()
    if args.command in {"backup", "restore", "cleanup", "check-config", "check-xiaozhi"}:
        run_ops(args)
        return
    if args.command == "rotate-notes-key":
        from services.pet_learning.db import connect, migrate
        from services.pet_learning.notes import ParentNotes
        migrate(args.db)
        with connect(args.db) as db:
            try:
                print(f"NOTES_REENCRYPTED {ParentNotes(settings).rotate_all(db)}")
            except Exception as error:  # noqa: BLE001 - never echo note content or keys
                raise SystemExit(f"rotate-notes-key failed: {type(error).__name__}") from None
        return
    store = PetStore(args.db)
    if args.command == "create-parent":
        auth = ParentAuth(args.db, settings)
        settings.cipher()  # Fail before prompting if the separate encryption key is missing.
        password = os.environ.get("KID_PET_PARENT_PASSWORD") or getpass.getpass("Parent password: ")
        secret = new_secret()
        print("Bind this secret in your authenticator (local terminal only): " + secret)
        print(provisioning_uri(args.username, secret))
        confirmation = getpass.getpass("Authenticator TOTP code: ")
        try:
            recovery = auth.create_account(args.username, password, secret, confirmation)
        except ValueError as error:
            raise SystemExit(str(error)) from None
        print("PARENT_CREATED. Store these one-time recovery codes offline:")
        print("\n".join(recovery))
    elif args.command == "enroll":
        token = os.environ.get("KID_PET_DEVICE_TOKEN", "")
        if len(token) < 32:
            raise SystemExit("KID_PET_DEVICE_TOKEN environment variable missing or too short")
        store.enroll_device(args.device_id, token)
        print("DEVICE_PROVISIONED (credential not displayed)")
    elif args.command == "import-page":
        store.add_approved_page(args.book_id, args.title, args.page, args.text,
                                approved=args.approved)
        print("PAGE_APPROVED" if args.approved else "PAGE_DRAFT_NOT_VISIBLE")
    else:
        QuizEngine(args.db).import_card(
            args.card_id, args.prompt, args.first, args.second,
            args.correct, args.success, args.retry, approved=args.approved)
        print("QUIZ_APPROVED" if args.approved else "QUIZ_DRAFT_NOT_VISIBLE")


if __name__ == "__main__":
    main()
