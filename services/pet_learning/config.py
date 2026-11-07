"""Small literal .env loader; environment takes precedence, no shell expansion."""
from dataclasses import dataclass, field
import ipaddress
import os
from pathlib import Path
from urllib.parse import urlsplit

from cryptography.fernet import Fernet


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
    playground: bool = False

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
            playground=env.get("KID_PET_ENABLE_PLAYGROUND") == "1",
        )
        settings.validate()
        return settings

    def validate(self):
        if self.totp_key:
            try:
                Fernet(self.totp_key.encode("ascii"))
            except (ValueError, UnicodeError):
                raise ValueError("KID_PET_TOTP_KEY must be a Fernet key") from None
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
        # Deployment/provider checks live in ops.check_deployment (admin check-config).

    def cipher(self):
        if not self.totp_key:
            raise ValueError("KID_PET_TOTP_KEY is required for parent authentication")
        return Fernet(self.totp_key.encode("ascii"))
