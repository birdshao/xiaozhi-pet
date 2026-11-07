import importlib.util
import json
from pathlib import Path
import unittest
from urllib.parse import parse_qs

PATH = Path(__file__).resolve().parents[1] / "deploy" / "ingress" / "pet-ddns.py"
spec = importlib.util.spec_from_file_location("pet_ddns", PATH)
ddns = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ddns)


class FakeResponse:
    def __init__(self, payload):
        self.payload = payload if isinstance(payload, bytes) else json.dumps(payload).encode()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False

    def read(self, n=-1):
        return self.payload[:n] if n > 0 else self.payload


class FakeDnspod:
    """Records every request; Record.List returns `records`."""

    def __init__(self, records):
        self.records, self.calls = records, []

    def __call__(self, req, timeout=0):
        action = req.full_url.rsplit("/", 1)[1]
        self.calls.append((action, {k: v[0] for k, v in parse_qs(req.data.decode()).items()}))
        body = {"status": {"code": "1"}}
        if action == "Record.List":
            body["records"] = self.records
        return FakeResponse(body)


class DdnsTests(unittest.TestCase):
    def test_parse_ip_only_accepts_public_addresses(self):
        self.assertEqual(ddns.parse_ip("223.166.24.116\n", 4), "223.166.24.116")
        self.assertEqual(ddns.parse_ip("当前 IP：223.166.24.116  来自于：中国 上海", 4), "223.166.24.116")
        self.assertIsNone(ddns.parse_ip("192.168.1.1", 4))
        self.assertIsNone(ddns.parse_ip("<html>10.0.0.1 error</html>", 4))
        self.assertEqual(ddns.parse_ip("2408:820c:8f99:6e0::1", 6), "2408:820c:8f99:6e0::1")
        self.assertIsNone(ddns.parse_ip("2408:820c::1", 4))

    def test_fetch_requires_agreement_or_a_single_answer(self):
        answers = {"a": b"1.1.1.1", "b": b"8.8.8.8", "c": b"8.8.8.8"}

        def opener(url, timeout=0):
            return FakeResponse(answers[url])

        self.assertEqual(ddns.fetch_public_ip(4, ["a", "b", "c"], opener), "8.8.8.8")
        self.assertIsNone(ddns.fetch_public_ip(4, ["a", "b"], opener))  # disagreement: leave DNS alone
        self.assertEqual(ddns.fetch_public_ip(4, ["a"], opener), "1.1.1.1")

        def broken(url, timeout=0):
            raise OSError("down")

        self.assertIsNone(ddns.fetch_public_ip(4, ["a"], broken))

    def test_sync_noop_modify_create(self):
        rec = [{"id": "7", "name": "pet", "type": "A", "value": "1.2.3.4"}]
        fake = FakeDnspod(rec)
        self.assertIn("already", ddns.sync("1,tok", "birdshao.cn", "pet", "A", "1.2.3.4", fake))
        self.assertEqual([c[0] for c in fake.calls], ["Record.List"])
        fake = FakeDnspod(rec)
        self.assertIn("modify", ddns.sync("1,tok", "birdshao.cn", "pet", "A", "5.6.7.8", fake))
        action, params = fake.calls[-1]
        self.assertEqual((action, params["record_id"], params["value"]), ("Record.Modify", "7", "5.6.7.8"))
        fake = FakeDnspod([])
        self.assertIn("create", ddns.sync("1,tok", "birdshao.cn", "pet", "A", "5.6.7.8", fake))
        self.assertEqual(fake.calls[-1][0], "Record.Create")

    def test_api_error_does_not_leak_token(self):
        def bad(req, timeout=0):
            return FakeResponse({"status": {"code": "104", "message": "bad token"}})

        with self.assertRaises(RuntimeError) as ctx:
            ddns.api_call("Record.List", "1,secrettoken", {}, bad)
        self.assertNotIn("secrettoken", str(ctx.exception))

    def test_main_needs_credentials(self):
        self.assertEqual(ddns.main({}), 1)


if __name__ == "__main__":
    unittest.main()
