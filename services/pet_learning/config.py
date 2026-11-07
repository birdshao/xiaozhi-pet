"""Small literal .env loader; environment takes precedence, no shell expansion."""
from dataclasses import dataclass, field
import ipaddress
import os
from pathlib import Path
from urllib.parse import urlsplit
from zoneinfo import ZoneInfo

from cryptography.fernet import Fernet, MultiFernet


def read_env(path):
    values = {}
    if not Path(path).exists():
        return values
    for number, line in enumerate(Path(path).read_text(encoding="utf-8-sig").splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        key, separator, value = line.partition("=")
        if not separator or not key.strip().isidentifier():
            raise ValueError(f"Invalid .env assignment at line {number}")
        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
            value = value[1:-1]
        values[key.strip()] = value
    return values


@dataclass(frozen=True)
class Settings:
    database: str = "data/pet_learning.sqlite3"
    production: bool = False
    public_origin: str = ""
    trusted_proxy: str = ""
    totp_key: str = field(default="", repr=False)
    # Parent notes encryption key (D9, S3): separate from the TOTP and backup keys.
    notes_key: str = field(default="", repr=False)
    playground: bool = False
    # Voice-service identity (S5): a bearer token separate from every device token.
    voice_token: str = field(default="", repr=False)
    # Peers (IPs/CIDRs) allowed to call /v1/voice/*: the XiaoZhi container address in compose.
    voice_allowed_peers: str = "127.0.0.1,::1"
    # Parent-configured display timezone for "几点了" until S3 stores it per device.
    timezone: str = "Asia/Shanghai"

    @classmethod
    def load(cls, env_file=".env"):
        env = {**read_env(env_file), **os.environ}
        mode = env.get("KID_PET_ENV", "development")
        if mode not in {"development", "production"}:
            raise ValueError("KID_PET_ENV must be development or production")
        settings = cls(
            database=env.get("KID_PET_DB", cls.database),
            production=mode == "production",
            public_origin=env.get("KID_PET_PUBLIC_ORIGIN", ""),
            trusted_proxy=env.get("KID_PET_TRUSTED_PROXY", ""),
            totp_key=env.get("KID_PET_TOTP_KEY", ""),
            notes_key=env.get("KID_PET_NOTES_KEY", ""),
            playground=env.get("KID_PET_ENABLE_PLAYGROUND") == "1",
            voice_token=env.get("KID_PET_VOICE_TOKEN", ""),
            voice_allowed_peers=env.get("KID_PET_VOICE_ALLOWED_PEERS", cls.voice_allowed_peers),
            timezone=env.get("KID_PET_TIMEZONE", cls.timezone),
        )
        settings.validate()
        return settings

    def validate(self):
        if self.totp_key:
            try:
                Fernet(self.totp_key.encode("ascii"))
            except (ValueError, UnicodeError):
                raise ValueError("KID_PET_TOTP_KEY must be a Fernet key") from None
        if self.notes_key:
            keys = self.notes_keys()
            if not keys:
                raise ValueError("KID_PET_NOTES_KEY must contain at least one Fernet key")
            try:
                for key in keys:
                    Fernet(key.encode("ascii"))
            except (ValueError, UnicodeError):
                raise ValueError("KID_PET_NOTES_KEY must be Fernet keys (comma separated; the first encrypts)") from None
            if len(set(keys)) != len(keys) or self.totp_key in keys:
                raise ValueError("KID_PET_NOTES_KEY must be distinct keys and differ from KID_PET_TOTP_KEY")
        if self.voice_token and not 32 <= len(self.voice_token) <= 256:
            raise ValueError("KID_PET_VOICE_TOKEN must contain 32-256 characters")
        self.voice_peer_networks()
        try:
            ZoneInfo(self.timezone)
        except (ValueError, OSError, KeyError):
            raise ValueError("KID_PET_TIMEZONE must be an IANA timezone name") from None
        if self.trusted_proxy:
            ipaddress.ip_address(self.trusted_proxy)
        if self.production:
            origin = urlsplit(self.public_origin)
            if (origin.scheme != "https" or not origin.hostname or origin.username
                    or origin.password or origin.query or origin.fragment
                    or origin.path not in {"", "/"}):
                raise ValueError("Production requires an HTTPS KID_PET_PUBLIC_ORIGIN")
            if origin.hostname in {"localhost", "0.0.0.0"} or origin.hostname.endswith(".localhost"):
                raise ValueError("Production KID_PET_PUBLIC_ORIGIN must not be a loopback name")
            if not self.totp_key or not self.trusted_proxy:
                raise ValueError("Production requires TOTP key and trusted proxy")
            if not self.notes_key:
                raise ValueError("Production requires KID_PET_NOTES_KEY (parent notes are encrypted at rest)")
        # Deployment/provider checks live in ops.check_deployment (admin check-config).

    def voice_peer_networks(self):
        try:
            networks = [ipaddress.ip_network(item.strip(), strict=False)
                        for item in self.voice_allowed_peers.split(",") if item.strip()]
        except ValueError:
            raise ValueError("KID_PET_VOICE_ALLOWED_PEERS must be IPs or CIDRs") from None
        if not networks:
            raise ValueError("KID_PET_VOICE_ALLOWED_PEERS must not be empty")
        return networks

    def cipher(self):
        if not self.totp_key:
            raise ValueError("KID_PET_TOTP_KEY is required for parent authentication")
        return Fernet(self.totp_key.encode("ascii"))

    def notes_keys(self):
        return [k.strip() for k in self.notes_key.split(",") if k.strip()]

    def notes_cipher(self):
        """Fail closed: without the key notes are unreadable and unwritable, never stored plain.

        Comma separated keys rotate: the first encrypts, every key can decrypt."""
        if not self.notes_key:
            raise ValueError("KID_PET_NOTES_KEY is required for parent notes")
        return MultiFernet([Fernet(k.encode("ascii")) for k in self.notes_keys()])
