#!/usr/bin/env python3
"""Local parent/developer provisioning utility. No HTTP admin endpoint.

Never pass device credentials on argv; provide KID_PET_DEVICE_TOKEN via env.
Never print the token or children's personal information.
"""
from __future__ import annotations
import argparse
import os
from services.pet_learning.store import PetStore
from services.pet_learning.quiz import QuizEngine


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db", default=os.getenv("KID_PET_DB", "data/pet_learning.sqlite3"))
    sub = parser.add_subparsers(dest="command", required=True)
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
    if args.command == "enroll":
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
