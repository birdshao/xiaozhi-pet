import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
DEPLOY = ROOT / "deploy"


def text(*parts):
    return DEPLOY.joinpath(*parts).read_text(encoding="utf-8")


class DeployFilesTests(unittest.TestCase):
    def test_xiaozhi_build_pin_matches_version_lock(self):
        lock = json.loads((ROOT / "version-lock.json").read_text(encoding="utf-8"))
        arg = re.search(r"ARG XZ_COMMIT=([0-9a-f]{40})", text("Dockerfile.xiaozhi")).group(1)
        self.assertEqual(arg, lock["voice_server"]["commit"])

    def test_single_published_port_and_hardening(self):
        compose = text("compose.yaml")
        self.assertEqual(len(re.findall(r"^\s+ports:", compose, re.M)), 1)
        self.assertIn("read_only: true", compose)
        self.assertIn("cap_drop: [ALL]", compose)
        self.assertIn("KID_PET_ENV: production", compose)
        self.assertNotIn("0.0.0.0:", compose)

    def test_env_example_has_every_required_variable_and_no_secrets(self):
        example = text(".env.example")
        required = set(re.findall(r"\$\{([A-Z_]+):\?", text("compose.yaml")))
        keys = {line.split("=", 1)[0] for line in example.splitlines() if "=" in line and not line.startswith("#")}
        self.assertLessEqual(required, keys)
        for secret in ("KID_PET_TOTP_KEY", "KID_PET_BACKUP_KEY", "XZ_AUTH_KEY", "KID_PET_VOICE_TOKEN"):
            self.assertRegex(example, rf"(?m)^{secret}=$", secret)

    def test_gateway_routes_only_allow_listed_paths(self):
        conf = text("nginx", "gateway.conf.template")
        locations = re.findall(r"location (?:\^~ |= )?(/\S*)", conf)
        self.assertEqual(locations[-1], "/")
        self.assertIn("return 404", conf.split("location / ")[-1])
        for blocked in ("/v1/voice", "/play", "/docs", "/openapi.json", "/mcp/"):
            self.assertFalse(any(loc.startswith(blocked) for loc in locations), blocked)
        self.assertIn("client_max_body_size 64m", conf)
        self.assertIn("access_log off", conf)

    def test_local_asr_and_child_audio_stay_on_box(self):
        config = text("xiaozhi", "child.config.yaml")
        self.assertIn("ASR: SherpaParaformerASR", config)
        self.assertIn("sherpa-onnx-paraformer-zh-small-2024-03-09", text("compose.yaml"))
        self.assertRegex(text("fetch_models.sh"), r"SHA256=[0-9a-f]{64}")

    def test_compose_structure(self):
        import ipaddress
        try:
            import yaml
        except ImportError:
            self.skipTest("PyYAML not installed")
        compose = yaml.safe_load(text("compose.yaml"))
        services = compose["services"]
        subnet = ipaddress.ip_network(compose["networks"]["pet"]["ipam"]["config"][0]["subnet"])
        pet_ip = services["pet-learning"]["networks"]["pet"]["ipv4_address"]
        xz_ip = services["xiaozhi-server"]["networks"]["pet"]["ipv4_address"]
        self.assertEqual(services["pet-learning"]["environment"]["KID_PET_VOICE_ALLOWED_PEERS"], xz_ip)
        for ip in (pet_ip, xz_ip, services["gateway"]["networks"]["pet"]["ipv4_address"]):
            self.assertIn(ipaddress.ip_address(ip), subnet)
        self.assertEqual(services["xiaozhi-server"]["environment"]["KID_PET_API_URL"], f"http://{pet_ip}:8765")
        published = [(name, p) for name, svc in services.items() for p in svc.get("ports", [])]
        self.assertEqual(len(published), 1)
        self.assertEqual(published[0][0], "gateway")
        # the backup key is only given to the one-off backup job
        self.assertNotIn("KID_PET_BACKUP_KEY", services["pet-learning"]["environment"])
        self.assertIn("KID_PET_BACKUP_KEY", services["backup"]["environment"])
        for job in ("backup", "cleanup"):
            self.assertEqual(services[job]["profiles"], ["ops"])
            self.assertEqual(services[job]["network_mode"], "none")
            self.assertEqual(services[job]["restart"], "no")

    def test_gateway_rate_limits_xiaozhi_routes(self):
        conf = text("nginx", "gateway.conf.template")
        self.assertEqual(len(re.findall(r"limit_req zone=xz_", conf)), 2)
        self.assertIn("limit_req_zone $binary_remote_addr zone=xz_ota", conf)
        self.assertIn("limit_req_zone $binary_remote_addr zone=xz_ws", conf)

    def test_dockerignore_and_maintenance_units(self):
        ignore = (ROOT / ".dockerignore").read_text(encoding="utf-8")
        for entry in ("**/.env", "deploy/models", ".git", "*.sqlite3", "research_content"):
            self.assertIn(entry, ignore)
        service = text("host", "pet-maintenance.service")
        self.assertIn("run --rm --no-deps backup", service)
        self.assertIn("run --rm --no-deps cleanup", service)
        self.assertIn("OnCalendar", text("host", "pet-maintenance.timer"))

    def test_providers_file_is_separate_from_env_and_ignored(self):
        self.assertIn("providers.env", text("compose.yaml"))
        ignore = (ROOT / ".gitignore").read_text(encoding="utf-8")
        self.assertIn("deploy/.env", ignore)
        self.assertIn("deploy/providers.env", ignore)

    @unittest.skipUnless(shutil.which("docker"), "docker not installed")
    def test_compose_config_validates(self):
        with tempfile.TemporaryDirectory() as tmp:
            env = Path(tmp) / "env"
            env.write_text("\n".join([
                "KID_PET_PUBLIC_ORIGIN=https://pet.example.com", "KID_PET_TRUSTED_PROXY=192.168.1.1",
                "KID_PET_TOTP_KEY=x", "KID_PET_BACKUP_KEY=x", "XZ_AUTH_KEY=x", "KID_PET_UPSTREAM_DEVICE_ID=a",
                "KID_PET_BUSINESS_DEVICE_ID=b", "KID_PET_VOICE_TOKEN=x"]), encoding="utf-8")
            run = subprocess.run(["docker", "compose", "--env-file", str(env), "-f", str(DEPLOY / "compose.yaml"),
                                  "config", "-q"], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)


if __name__ == "__main__":
    unittest.main()
