"""Offline, review-gated educational content import pipeline (LCIR1).

Importing is not approving. The firmware never consumes LCIR1 directly.
"""
from .schema import ContentError, content_digest, register_document, validate_document

__all__ = ["ContentError", "content_digest", "register_document", "validate_document"]
