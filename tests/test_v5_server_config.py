from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from cryptography.fernet import Fernet
from fastapi.testclient import TestClient

from services.pet_learning.api import create_app
from services.pet_learning.config import Settings


class ConfigTests(unittest.TestCase):
    def test_env_overrides_literal_dotenv_without_mutating_environment(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / ".env"
            path.write_text('# comment\nKID_PET_DB="from file.db"\nKID_PET_PUBLIC_ORIGIN=${NO_EXPANSION}\n', encoding="utf-8")
            with patch.dict("os.environ", {"KID_PET_DB": "from-env.db"}, clear=True):
                settings = Settings.load(path)
                self.assertEqual(settings.database, "from-env.db")
                self.assertEqual(settings.public_origin, "${NO_EXPANSION}")
            with patch.dict("os.environ", {}, clear=True):
                self.assertEqual(Settings.load(path).database, "from file.db")
                self.assertEqual(Settings.load(Path(tmp) / "missing").database, Settings.database)

    def test_invalid_mode_key_proxy_and_production_settings_fail_closed(self):
        for env in ({"KID_PET_ENV": "typo"}, {"KID_PET_TOTP_KEY": "weak"},
                    {"KID_PET_TRUSTED_PROXY": "*"}, {"KID_PET_ENV": "production"}):
            with patch.dict("os.environ", env, clear=True), self.assertRaises(ValueError):
                Settings.load("nonexistent.env")
        key = Fernet.generate_key().decode()
        with self.assertRaises(ValueError):  # production without the notes key
            Settings(production=True, public_origin="https://pet.example", trusted_proxy="127.0.0.1",
                     totp_key=key).validate()
        with self.assertRaises(ValueError):  # notes key must not be the TOTP key
            Settings(notes_key=key, totp_key=key).validate()
        with self.assertRaises(ValueError):
            Settings(notes_key="weak").validate()
        for origin in ("http://pet.example", "https://pet.example/path", "https://user@pet.example"):
            with self.assertRaises(ValueError):
                Settings(production=True, public_origin=origin, trusted_proxy="127.0.0.1",
                         totp_key=Fernet.generate_key().decode(),
                         notes_key=Fernet.generate_key().decode()).validate()

    def test_production_disables_debug_routes_even_with_playground_env(self):
        settings = Settings(production=True, public_origin="https://pet.example", trusted_proxy="127.0.0.1",
                            totp_key=Fernet.generate_key().decode(), playground=True,
                            notes_key=Fernet.generate_key().decode())
        with tempfile.TemporaryDirectory() as tmp:
            with TestClient(create_app(Path(tmp) / "db", settings=settings)) as client:
                with patch.dict("os.environ", {"KID_PET_ENABLE_PLAYGROUND": "1"}):
                    for route in ("/docs", "/redoc", "/openapi.json", "/play", "/play/play.js"):
                        self.assertEqual(client.get(route).status_code, 404)
                self.assertEqual(client.get("/healthz").status_code, 200)
