"""RFC 6238 SHA-1 TOTP, 30-second steps; no OTP library required."""
import base64
import hashlib
import hmac
import re
import secrets
import struct
from urllib.parse import quote, urlencode


def new_secret():
    return base64.b32encode(secrets.token_bytes(20)).decode("ascii").rstrip("=")


def code_at(secret, timestamp, *, digits=6):
    key = base64.b32decode(secret + "=" * (-len(secret) % 8))
    digest = hmac.new(key, struct.pack(">Q", int(timestamp) // 30), hashlib.sha1).digest()
    offset = digest[-1] & 15
    number = struct.unpack(">I", digest[offset:offset + 4])[0] & 0x7fffffff
    return str(number % (10 ** digits)).zfill(digits)


def match_step(secret, code, timestamp, *, last_step=-1):
    if not re.fullmatch(r"[0-9]{6}", code):
        return None
    step = int(timestamp) // 30
    for candidate in (step, step - 1, step + 1):
        if candidate >= 0 and candidate > last_step and hmac.compare_digest(
                code_at(secret, candidate * 30), code):
            return candidate
    return None


def provisioning_uri(username, secret):
    return "otpauth://totp/" + quote("xiaozhi-pet:" + username, safe="") + "?" + urlencode({
        "secret": secret, "issuer": "xiaozhi-pet", "algorithm": "SHA1", "digits": 6, "period": 30,
    })
