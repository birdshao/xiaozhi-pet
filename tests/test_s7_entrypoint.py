import importlib.util
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "services" / "pet_learning"))  # entrypoint imports xz_checklist as a top-level module
spec = importlib.util.spec_from_file_location("xz_entrypoint", ROOT / "deploy" / "xiaozhi" / "entrypoint.py")
entrypoint = importlib.util.module_from_spec(spec)
try:
    spec.loader.exec_module(entrypoint)
except ImportError:  # neither ruamel.yaml nor PyYAML
    entrypoint = None

BASE = (ROOT / "deploy" / "xiaozhi" / "child.config.yaml").read_text(encoding="utf-8")
ENV = {"KID_PET_PUBLIC_ORIGIN": "https://pet.example.com:8443", "XZ_AUTH_KEY": "k" * 40}


@unittest.skipIf(entrypoint is None, "no YAML library")
class EntrypointTests(unittest.TestCase):
    def test_base_config_with_env_passes_checklist(self):
        config, problems = entrypoint.build_config(BASE, ENV)
        self.assertEqual(problems, [])
        self.assertEqual(config["server"]["websocket"], "wss://pet.example.com:8443/xiaozhi/v1/")
        self.assertEqual(config["server"]["auth_key"], "k" * 40)

    def test_module_selection_and_secret_values_stay_strings(self):
        env = {**ENV, "XZ_TTS_MODULE": "EdgeTTS", "XZ__LLM__ChatGLMLLM__api_key": "1e5",
               "XZ__LLM__ChatGLMLLM__other": "0123", "XZ__TTS__EdgeTTS__flag": "yes",
               "XZ__LLM__ChatGLMLLM__nothing": "null", "XZ__TTS__EdgeTTS__empty": ""}
        config, problems = entrypoint.build_config(BASE, env)
        self.assertEqual(problems, [])
        self.assertEqual(config["selected_module"]["TTS"], "EdgeTTS")
        llm, tts = config["LLM"]["ChatGLMLLM"], config["TTS"]["EdgeTTS"]
        self.assertEqual((llm["api_key"], llm["other"], llm["nothing"]), ("1e5", "0123", "null"))
        self.assertEqual((tts["flag"], tts["empty"]), ("yes", ""))

    def test_typed_values_are_opt_in(self):
        env = {**ENV, "XZ__TTS__EdgeTTS__speed": "yaml:1.2", "XZ__TTS__EdgeTTS__on": "yaml:true"}
        config, _ = entrypoint.build_config(BASE, env)
        self.assertEqual(config["TTS"]["EdgeTTS"]["speed"], 1.2)
        self.assertIs(config["TTS"]["EdgeTTS"]["on"], True)

    def test_unsafe_overrides_are_refused(self):
        for env in ({**ENV, "XZ_AUTH_KEY": "short"},
                    {**ENV, "XZ__server__auth__enabled": "yaml:false"},
                    {**ENV, "XZ__selected_module__Memory": "mem0ai"},
                    {**ENV, "XZ__delete_audio": "yaml:false"}):
            _, problems = entrypoint.build_config(BASE, env)
            self.assertTrue(problems, env)


if __name__ == "__main__":
    unittest.main()
