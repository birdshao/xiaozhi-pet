"""Import original sample stories and quizzes as DRAFTS by default.

--approved is a trusted adult assertion that every item has been reviewed.
Never invoked automatically during server startup or device provisioning.
"""
from __future__ import annotations
import argparse
import os
from services.pet_learning.quiz import QuizEngine
from services.pet_learning.store import PetStore
from services.pet_learning.sample_content import BOOK, QUIZZES


def main() -> None:
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db", default=os.environ.get("KID_PET_DB", "data/pet_learning.sqlite3"))
    parser.add_argument("--approved", action="store_true",
                        help="Adult has explicitly reviewed every included text")
    args=parser.parse_args()
    store=PetStore(args.db)
    quiz=QuizEngine(args.db)
    for page_no, text in enumerate(BOOK["pages"], 1):
        store.add_approved_page(BOOK["book_id"], BOOK["title"], page_no, text,
                                approved=args.approved)
    for item in QUIZZES:
        quiz.import_card(*item, approved=args.approved)
    print("SAMPLE_CONTENT_APPROVED" if args.approved else "SAMPLE_CONTENT_DRAFT_ONLY")


if __name__=="__main__":
    main()
