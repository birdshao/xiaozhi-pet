#!/usr/bin/env python3
"""Render data/.config.yaml for the child-mode XiaoZhi server, verify it, then exec app.py."""
import os
import sys
from pathlib import Path
from urllib.parse import urlsplit

from ruamel.yaml import YAML

sys.path.insert(0, "/opt/pet")
from xz_checklist import check_xiaozhi_config  # noqa: E402

yaml = YAML(typ="safe")
yaml.default_flow_style = False


def set_path(tree, parts, value):
    for part in parts[:-1]:
        tree = tree.setdefault(part, {})
    tree[parts[-1]] = value


def main():
    config = yaml.load(Path("/opt/pet/child.config.yaml").read_text(encoding="utf-8"))
    origin = os.environ.get("KID_PET_PUBLIC_ORIGIN", "")
    config["server"]["auth_key"] = os.environ.get("XZ_AUTH_KEY", "")
    if origin:
        config["server"]["websocket"] = "wss://" + urlsplit(origin).netloc + "/xiaozhi/v1/"
    for env_name, module in (("XZ_ASR_MODULE", "ASR"), ("XZ_LLM_MODULE", "LLM"), ("XZ_TTS_MODULE", "TTS")):
        if os.environ.get(env_name):
            config["selected_module"][module] = os.environ[env_name]
    for name, value in os.environ.items():  # XZ__LLM__<module>__api_key=...
        if name.startswith("XZ__"):
            set_path(config, name[4:].split("__"), yaml.load(value) if value else "")
    problems = check_xiaozhi_config(config, origin if origin.startswith("https://") else "")
    if problems:
        sys.exit("XiaoZhi child-mode checklist failed:\n" + "\n".join(" - " + p for p in problems))
    target = Path("data/.config.yaml")
    target.parent.mkdir(exist_ok=True)
    fd = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as handle:
        yaml.dump(config, handle)
    os.execvp("python", ["python", "app.py"])


if __name__ == "__main__":
    main()
