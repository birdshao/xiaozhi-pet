"""Strict Python mirrors of the device's MemoryStore and ParentSettingsCodec.

Byte layouts are defined by firmware/components/kid_app/src/memory_store.cc
and parent_settings.cc; tests/fixtures/memory/ holds golden vectors produced
by tests/cpp/test_memory_golden.cc. A decode either returns exactly what the
device meant or raises: no partial or best-effort parsing (V5_V6 plan §5.2).
Unknown format versions are rejected, never guessed.
"""
from __future__ import annotations

from dataclasses import dataclass, field
import struct
import zlib

MEMORY_BYTES = 288
MEMORY_FORMAT = 1
PREFERENCE_CAPACITY = 16
RECENT_BOOK_CAPACITY = 8
TODAY_CAP = 250
TODAY_AT, YESTERDAY_AT, GROWTH_AT, BOOKS_AT, PREFS_AT, PREF_BYTES = 4, 18, 32, 44, 60, 12
DAY = struct.Struct("<IBBBBBxHH")  # 14 bytes, byte 9 is padding
GROWTH = struct.Struct("<HHHHI")
PREF = struct.Struct("<HHQ")
TEMPERAMENTS = ("curious", "gentle", "lively")  # Temperament enum order
MILESTONES = ("first_book", "five_books", "ten_books", "first_game",
              "ten_games", "first_preference", "seven_days")
ALL_MILESTONES = (1 << len(MILESTONES)) - 1

SETTINGS_BYTES = 28
SETTINGS_VERSION = 1
SETTINGS = struct.Struct("<2sHBxHHHHHHBB4xI")
FLAG_NAP, FLAG_MOTION = 0x01, 0x02
MINUTES_PER_DAY = 24 * 60
VOLUME_STEP, MIN_VOLUME_CAP = 10, 10


class CodecError(ValueError):
    """Raised for any blob the device codec would not decode."""

    def __init__(self, code: str) -> None:
        self.code = code
        super().__init__(code)


def _corrupt(condition: bool, code: str) -> None:
    if condition:
        raise CodecError(code)


@dataclass(frozen=True)
class Day:
    day: int = 0
    feeds: int = 0
    greetings: int = 0
    books: int = 0
    games: int = 0
    choices: int = 0
    last_book: int = 0
    last_game: int = 0

    def active(self) -> bool:
        return bool(self.feeds or self.greetings or self.books or self.games or self.choices)

    def valid(self) -> bool:
        return (max(self.feeds, self.greetings, self.books, self.games, self.choices) <= TODAY_CAP
                and (self.books or not self.last_book) and (self.games or not self.last_game))

    def counts(self) -> dict:
        return {"feeds": self.feeds, "greetings": self.greetings, "books": self.books,
                "games": self.games, "choices": self.choices}


@dataclass(frozen=True)
class Preference:
    slot: int
    option: int
    source_event: int  # device audit trail; never stored or sent onwards


@dataclass(frozen=True)
class Growth:
    books_finished: int = 0
    games_completed: int = 0
    preferences_chosen: int = 0
    active_days: int = 0
    milestones: int = 0


@dataclass(frozen=True)
class Memory:
    temperament: int = 1  # Gentle
    today: Day = field(default_factory=Day)
    yesterday: Day = field(default_factory=Day)
    growth: Growth = field(default_factory=Growth)
    preferences: tuple[Preference, ...] = ()  # index 0 = most recent
    recent_books: tuple[int, ...] = ()        # index 0 = newest

    @property
    def trait(self) -> str:
        return TEMPERAMENTS[self.temperament]

    def empty(self) -> bool:
        return self == Memory(temperament=self.temperament)

    def encode(self) -> bytes:
        b = bytearray(MEMORY_BYTES)
        b[0:4] = bytes((MEMORY_FORMAT, self.temperament, len(self.preferences), len(self.recent_books)))
        for at, d in ((TODAY_AT, self.today), (YESTERDAY_AT, self.yesterday)):
            DAY.pack_into(b, at, d.day, d.feeds, d.greetings, d.books, d.games, d.choices,
                          d.last_book, d.last_game)
        g = self.growth
        GROWTH.pack_into(b, GROWTH_AT, g.books_finished, g.games_completed,
                         g.preferences_chosen, g.active_days, g.milestones)
        for i, book in enumerate(self.recent_books):
            struct.pack_into("<H", b, BOOKS_AT + 2 * i, book)
        for i, p in enumerate(self.preferences):
            PREF.pack_into(b, PREFS_AT + PREF_BYTES * i, p.slot, p.option, p.source_event)
        return bytes(b)

    def minimal_summary(self, current_day: int = 0) -> dict:
        """Same fields as the device's FormatMinimalSummary(MakeMinimalSummary())."""
        stale = bool(current_day and self.today.day and self.today.day != current_day)
        t = Day() if stale else self.today
        return {
            "trait": self.trait,
            "preferences": [{"slot": p.slot, "option": p.option} for p in self.preferences],
            "today": {"feed": t.feeds, "greet": t.greetings, "books": t.books, "games": t.games},
            "last_book": self.recent_books[0] if self.recent_books else 0,
            "milestones": self.growth.milestones,
        }


def decode_memory(blob: bytes) -> Memory:
    """Mirror of MemoryStore::Decode: strict, canonical, exactly 288 bytes."""
    _corrupt(not isinstance(blob, (bytes, bytearray)) or len(blob) != MEMORY_BYTES, "MEMORY_SIZE")
    if blob[0] != MEMORY_FORMAT:
        raise CodecError("MEMORY_FORMAT_UNSUPPORTED")
    _corrupt(blob[1] >= len(TEMPERAMENTS) or blob[2] > PREFERENCE_CAPACITY
             or blob[3] > RECENT_BOOK_CAPACITY, "MEMORY_CORRUPT")
    today, yesterday = (Day(*DAY.unpack_from(blob, at)) for at in (TODAY_AT, YESTERDAY_AT))
    growth = Growth(*GROWTH.unpack_from(blob, GROWTH_AT))
    _corrupt(not today.valid() or not yesterday.valid()
             or growth.milestones & ~ALL_MILESTONES
             or (yesterday.day and yesterday.day >= today.day)
             or (not yesterday.day and yesterday != Day()), "MEMORY_CORRUPT")
    books = tuple(struct.unpack_from("<H", blob, BOOKS_AT + 2 * i)[0] for i in range(blob[3]))
    _corrupt(0 in books or len(set(books)) != len(books), "MEMORY_CORRUPT")
    prefs = tuple(Preference(*PREF.unpack_from(blob, PREFS_AT + PREF_BYTES * i)) for i in range(blob[2]))
    _corrupt(any(not (p.slot and p.option and p.source_event) for p in prefs)
             or len({p.slot for p in prefs}) != len(prefs), "MEMORY_CORRUPT")
    memory = Memory(blob[1], today, yesterday, growth, prefs, books)
    # Canonical form only: unused entries, padding and reserved bytes are zero.
    _corrupt(memory.encode() != bytes(blob), "MEMORY_CORRUPT")
    return memory


@dataclass(frozen=True)
class ParentSettings:
    wake_min: int = 7 * 60
    evening_min: int = 19 * 60 + 30
    quiet_start_min: int = 20 * 60 + 30
    quiet_end_min: int = 7 * 60
    nap_enabled: bool = False
    nap_start_min: int = 13 * 60
    nap_end_min: int = 14 * 60
    volume_cap_pct: int = 100
    quiet_volume_pct: int = 30
    motion_enabled: bool = True

    def _in_quiet(self, minute: int) -> bool:
        if self.quiet_start_min < self.quiet_end_min:
            return self.quiet_start_min <= minute < self.quiet_end_min
        return minute >= self.quiet_start_min or minute < self.quiet_end_min

    def valid(self) -> bool:
        """ValidParentSettings: RhythmScheduler::Valid plus volume ranges and steps."""
        minutes = (self.wake_min, self.evening_min, self.quiet_start_min, self.quiet_end_min,
                   self.nap_start_min, self.nap_end_min)
        if any(m >= MINUTES_PER_DAY for m in minutes) or self.quiet_start_min == self.quiet_end_min:
            return False
        if self._in_quiet(self.wake_min) or self._in_quiet(self.evening_min):
            return False
        if self.evening_min <= self.wake_min or self.evening_min >= self.quiet_start_min:
            return False
        if self.nap_enabled and (self.nap_start_min >= self.nap_end_min
                                 or self.nap_start_min < self.wake_min
                                 or self.nap_end_min > self.evening_min
                                 or self._in_quiet(self.nap_start_min)):
            return False
        return (MIN_VOLUME_CAP <= self.volume_cap_pct <= 100 and self.volume_cap_pct % VOLUME_STEP == 0
                and 0 <= self.quiet_volume_pct <= 100 and self.quiet_volume_pct % VOLUME_STEP == 0)

    def encode(self) -> bytes:
        if not self.valid():
            raise CodecError("SETTINGS_INVALID")
        flags = (FLAG_NAP if self.nap_enabled else 0) | (FLAG_MOTION if self.motion_enabled else 0)
        body = SETTINGS.pack(b"PS", SETTINGS_VERSION, flags, self.wake_min, self.evening_min,
                             self.quiet_start_min, self.quiet_end_min, self.nap_start_min,
                             self.nap_end_min, self.volume_cap_pct, self.quiet_volume_pct, 0)
        return body[:24] + struct.pack("<I", zlib.crc32(body[:24]))

    def as_dict(self) -> dict:
        return dict(self.__dict__)


def decode_parent_settings(blob: bytes) -> ParentSettings:
    """Mirror of ParentSettingsCodec::Decode for an exactly 28-byte blob.

    The C++ decoder reads the first 28 bytes of a longer buffer; the server
    only ever receives the encoding itself, so any other length is rejected.
    """
    _corrupt(not isinstance(blob, (bytes, bytearray)) or len(blob) != SETTINGS_BYTES, "SETTINGS_SIZE")
    tag, version, flags, *minutes, cap, quiet, crc = SETTINGS.unpack(bytes(blob))
    _corrupt(tag != b"PS", "SETTINGS_CORRUPT")
    if version != SETTINGS_VERSION:
        raise CodecError("SETTINGS_VERSION_UNSUPPORTED")
    _corrupt(flags & ~(FLAG_NAP | FLAG_MOTION) or blob[5] or any(blob[20:24])
             or crc != zlib.crc32(bytes(blob[:24])), "SETTINGS_CORRUPT")
    wake, evening, quiet_start, quiet_end, nap_start, nap_end = minutes
    settings = ParentSettings(wake, evening, quiet_start, quiet_end, bool(flags & FLAG_NAP),
                              nap_start, nap_end, cap, quiet, bool(flags & FLAG_MOTION))
    _corrupt(not settings.valid(), "SETTINGS_CORRUPT")
    return settings
