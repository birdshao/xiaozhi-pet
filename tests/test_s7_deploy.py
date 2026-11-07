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
                "KID_PET_TOTP_KEY=x", "XZ_AUTH_KEY=x", "KID_PET_UPSTREAM_DEVICE_ID=a",
                "KID_PET_BUSINESS_DEVICE_ID=b", "KID_PET_VOICE_TOKEN=x"]), encoding="utf-8")
            run = subprocess.run(["docker", "compose", "--env-file", str(env), "-f", str(DEPLOY / "compose.yaml"),
                                  "config", "-q"], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)


if __name__ == "__main__":
    unittest.main()
