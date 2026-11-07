#!/usr/bin/env python3
"""Render data/.config.yaml for the child-mode XiaoZhi server, verify it, then exec app.py."""
import os
import sys
from pathlib import Path
from urllib.parse import urlsplit

sys.path.insert(0, "/opt/pet")
from xz_checklist import check_xiaozhi_config  # noqa: E402

try:
    from ruamel.yaml import YAML
    _yaml = YAML(typ="safe")
    _yaml.default_flow_style = False
    load, dump = _yaml.load, _yaml.dump
except ImportError:  # only the unit tests run without ruamel (the image always has it)
    import yaml as _pyyaml
    load = _pyyaml.safe_load
    dump = _pyyaml.safe_dump

TYPED_PREFIX = "yaml:"


def set_path(tree, parts, value):
    for part in parts[:-1]:
        tree = tree.setdefault(part, {})
    tree[parts[-1]] = value


def override_value(raw):
    """Strings by default (API keys like 1e5, 0123, yes, null must stay text); `yaml:` opts in to types."""
    if raw.startswith(TYPED_PREFIX):
        return load(raw[len(TYPED_PREFIX):])
    return raw


def build_config(base_text, environ):
    config = load(base_text)
    origin = environ.get("KID_PET_PUBLIC_ORIGIN", "")
    config["server"]["auth_key"] = environ.get("XZ_AUTH_KEY", "")
    override = environ.get("XZ_WEBSOCKET_URL", "")
    if override:  # explicit address wins (LAN bring-up: ws://<VM IP>:8780/xiaozhi/v1/)
        config["server"]["websocket"] = override
    elif origin:
        config["server"]["websocket"] = "wss://" + urlsplit(origin).netloc + "/xiaozhi/v1/"
    for env_name, module in (("XZ_ASR_MODULE", "ASR"), ("XZ_LLM_MODULE", "LLM"), ("XZ_TTS_MODULE", "TTS")):
        if environ.get(env_name):
            config["selected_module"][module] = environ[env_name]
    for name, value in environ.items():  # XZ__LLM__<module>__api_key=...
        if name.startswith("XZ__"):
            set_path(config, name[4:].split("__"), override_value(value))
    problems = check_xiaozhi_config(config, origin if origin.startswith("https://") and not override else "")
    return config, problems


def main():
    config, problems = build_config(Path("/opt/pet/child.config.yaml").read_text(encoding="utf-8"), os.environ)
    if problems:
        sys.exit("XiaoZhi child-mode checklist failed:\n" + "\n".join(" - " + p for p in problems))
    target = Path("data/.config.yaml")
    target.parent.mkdir(exist_ok=True)
    fd = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as handle:
        dump(config, handle)
    os.execvp("python", ["python", "app.py"])


if __name__ == "__main__":
    main()
