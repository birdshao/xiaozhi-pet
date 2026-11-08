"""XiaoZhi child-mode / device-auth checklist for the pinned v0.9.8 config keys.

Stdlib only and import-free of the rest of the package: the xiaozhi image copies
this single file and refuses to start when the rendered config fails it.
"""
from __future__ import annotations

import ipaddress
from urllib.parse import urlsplit

# Reviewed sentence upstream speaks if its own LLM path ever errors (overlay keeps that path off).
CHILD_ERROR_RESPONSE = "现在还不能语音聊天，我们可以先用按键和小宠物玩。"
PLACEHOLDER_MARKERS = ("change-me", "changeme", "change_me", "你的", "example-secret")


def _weak(value: str) -> bool:
    lowered = value.lower()
    return not value or any(marker in lowered for marker in PLACEHOLDER_MARKERS)


PLACEHOLDER_SUFFIXES = (".example", ".invalid", ".test", ".localhost",
                        "example.com", "example.org", "example.net")


def is_placeholder_host(host: str) -> bool:
    """RFC 2606 reserved names and localhost: never a real address a device can reach."""
    host = (host or "").lower().rstrip(".")
    return host == "localhost" or any(host == s.lstrip(".") or host.endswith(s) for s in PLACEHOLDER_SUFFIXES)


def websocket_problems(url: str, public_origin: str = "") -> list[str]:
    """Rules for the address OTA hands to devices.

    With a public origin: wss:// on the same host:port. Without one (LAN bring-up via
    XZ_WEBSOCKET_URL): wss://, or plain ws:// only to a literal private IP.
    """
    try:
        parts = urlsplit(url)
        host = parts.hostname or ""
        parts.port  # noqa: B018 - raises ValueError on a malformed port
    except ValueError:
        return ["server.websocket is not a valid URL"]
    if is_placeholder_host(host):
        return [f"server.websocket points at a placeholder host ({host or 'empty'}); set the real address"]
    if parts.scheme == "wss":
        if public_origin and parts.netloc != urlsplit(public_origin).netloc:
            return ["server.websocket host:port must match KID_PET_PUBLIC_ORIGIN"]
        return []
    if parts.scheme == "ws" and not public_origin:
        try:
            if ipaddress.ip_address(host).is_private:
                return []
        except ValueError:
            pass
        return ["server.websocket may use ws:// only with a literal private (LAN) IP"]
    return ["server.websocket must be a wss:// address"]


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
    problems += websocket_problems(str(server.get("websocket") or ""), public_origin)
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
    # S6 TTS egress audit (docs/SERVER_S6.md §3): no upstream text may reach TTS in child mode.
    if cfg.get("enable_greeting") is not False:
        problems.append("enable_greeting must be false (no wake-word greeting through chat)")
    if (cfg.get("end_prompt") or {}).get("enable") is not False:
        problems.append("end_prompt.enable must be false (no model-written goodbye on timeout)")
    if cfg.get("enable_stop_tts_notify"):
        problems.append("enable_stop_tts_notify must be false")
    if cfg.get("system_error_response") != CHILD_ERROR_RESPONSE:
        problems.append("system_error_response must be the reviewed child sentence")
    endpoint = str(cfg.get("mcp_endpoint") or "")
    if endpoint and "你" not in endpoint:
        problems.append("mcp_endpoint must be unset")
    return problems
