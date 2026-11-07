"""XiaoZhi child-mode / device-auth checklist for the pinned v0.9.8 config keys.

Stdlib only and import-free of the rest of the package: the xiaozhi image copies
this single file and refuses to start when the rendered config fails it.
"""
from __future__ import annotations

from urllib.parse import urlsplit

PLACEHOLDER_MARKERS = ("change-me", "changeme", "change_me", "你的", "example-secret")


def _weak(value: str) -> bool:
    lowered = value.lower()
    return not value or any(marker in lowered for marker in PLACEHOLDER_MARKERS)


def check_xiaozhi_config(cfg: dict, public_origin: str = "") -> list[str]:
    """Child-mode / device-auth checklist against the pinned v0.9.8 config keys."""
    problems = []
    server = cfg.get("server") or {}
    auth = server.get("auth") or {}
    if auth.get("enabled") is not True:
        problems.append("server.auth.enabled must be true (device authentication)")
    if auth.get("allowed_devices"):
        problems.append("server.auth.allowed_devices must be empty (whitelisted devices skip token checks)")
    key = str(server.get("auth_key") or "")
    if len(key) < 32 or _weak(key):
        problems.append("server.auth_key must be a private random value of at least 32 characters")
    if server.get("mqtt_gateway") or server.get("udp_gateway"):
        problems.append("server.mqtt_gateway / udp_gateway must be unset")
    websocket = str(server.get("websocket") or "")
    if not websocket.startswith("wss://"):
        problems.append("server.websocket must be a wss:// address")
    elif public_origin:
        if urlsplit(websocket).netloc != urlsplit(public_origin).netloc:
            problems.append("server.websocket host:port must match KID_PET_PUBLIC_ORIGIN")
    if cfg.get("read_config_from_api"):
        problems.append("read_config_from_api must be false (no manager-api, so no chat history/audio reports)")
    if (cfg.get("manager-api") or {}).get("url"):
        problems.append("manager-api.url must be unset")
    if cfg.get("delete_audio") is not True:
        problems.append("delete_audio must be true")
    modules = cfg.get("selected_module") or {}
    if modules.get("Memory") != "nomem":
        problems.append("selected_module.Memory must be nomem (no XiaoZhi memory)")
    if modules.get("Intent") != "nointent":
        problems.append("selected_module.Intent must be nointent (the child guard owns intent)")
    endpoint = str(cfg.get("mcp_endpoint") or "")
    if endpoint and "你" not in endpoint:
        problems.append("mcp_endpoint must be unset")
    return problems
