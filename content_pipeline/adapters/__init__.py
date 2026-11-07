"""Adapters parse pinned local source snapshots; they perform no network access."""
from .bookdash import import_bookdash
from .global_asp import import_global_asp

__all__ = ["import_bookdash", "import_global_asp"]
