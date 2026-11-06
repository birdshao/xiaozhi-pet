#!/usr/bin/env python3
"""Local parent/developer provisioning utility. No HTTP admin endpoint.

Never pass device credentials on argv; provide KID_PET_DEVICE_TOKEN via env.
Never print the token or children's personal information.
"""
from __future__ import annotations
import argparse
import getpass
import os
from services.pet_learning.auth import ParentAuth
from services.pet_learning.config import Settings
from services.pet_learning.totp import new_secret, provisioning_uri
from services.pet_learning.store import PetStore
from services.pet_learning.quiz import QuizEngine


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
    args = parser.parse_args()
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
