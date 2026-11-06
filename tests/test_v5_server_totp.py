import base64
import unittest

from services.pet_learning.totp import code_at, match_step, new_secret, provisioning_uri


class TotpTests(unittest.TestCase):
    def test_rfc6238_sha1_vectors(self):
        secret = base64.b32encode(b"12345678901234567890").decode()
        for timestamp, expected in [(59, "94287082"), (1111111109, "07081804"),
                                    (1111111111, "14050471"), (1234567890, "89005924"),
                                    (2000000000, "69279037"), (20000000000, "65353130")]:
            with self.subTest(timestamp=timestamp):
                self.assertEqual(code_at(secret, timestamp, digits=8), expected)

    def test_window_format_and_replay(self):
        secret = new_secret()
        now = 1800000000
        for offset in (-30, 0, 30):
            step = (now + offset) // 30
            code = code_at(secret, now + offset)
            self.assertEqual(match_step(secret, code, now), step)
            self.assertIsNone(match_step(secret, code, now, last_step=step))
        for offset in (-60, 60):
            self.assertIsNone(match_step(secret, code_at(secret, now + offset), now))
        for invalid in ("", "１２３４５６", "12345", "1234567", "abcdef"):
            self.assertIsNone(match_step(secret, invalid, now))
        self.assertIn("issuer=xiaozhi-pet", provisioning_uri("parent", secret))
