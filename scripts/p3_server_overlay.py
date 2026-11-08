#!/usr/bin/env python3
"""Narrow ASR guard overlay for an EXACT pinned upstream XiaoZhi server checkout.

No hardware I/O, no ASR/Opus protocol modifications. Dry-run unless --apply.
Run only on a disposable upstream server checkout (not production).
"""
from __future__ import annotations
import argparse
from pathlib import Path
import subprocess

PINNED_SERVER_COMMIT = "8f2dda09cf18f1c1ece0f906eee4cc2fabb7fb58"
ANCHOR = "    # 检查是否有明确的退出命令"
INSERT = """    # Child profile uses reviewed PetLearning text before any general LLM path.
    # Non-child profile: the guard returns False and upstream works unchanged.
    from plugins_func.functions.child_voice_guard import handle_child_guard
    if await handle_child_guard(conn, text):
        return True

"""


# ADR-008: the device tool self.pet.request is for the child guard only. Upstream
# lists every device MCP tool in the model's function table (mcp_client.
# get_available_tools -> DeviceMCPExecutor.get_tools / intent_llm), so in any mode
# the overlay hides it there and refuses it if a model names it anyway; the guard
# calls it directly through mcp_handler.call_mcp_tool.
GUARD_ONLY_TOOL = "self_pet_request"
CLIENT_ANCHOR = "        for tool_name, tool_data in self.tools.items():\n"
CLIENT_INSERT = ('            if tool_name == "self_pet_request":  # child guard only (ADR-008)\n'
                 '                continue\n')
EXECUTOR_ANCHOR = '        """执行设备端MCP工具"""\n'
EXECUTOR_INSERT = ('        if tool_name == "self_pet_request":  # child guard only (ADR-008)\n'
                   '            return ActionResponse(action=Action.ERROR, response="not available")\n')


def _insert_after(text: str, anchor: str, insert: str, label: str) -> str:
    if insert in text:
        raise ValueError(label + ": guard-only filter already installed")
    if text.count(anchor) != 1:
        raise ValueError(label + ": upstream anchor changed; reject unsafe patch")
    patched = text.replace(anchor, anchor + insert, 1)
    compile(patched, label, "exec")
    return patched


def overlay_mcp_client(text: str) -> str:
    """Hide self_pet_request from MCPClient.get_available_tools (the model's list)."""
    if text.count("def get_available_tools(") != 1:
        raise ValueError("mcp_client.py: upstream anchor changed; reject unsafe patch")
    return _insert_after(text, CLIENT_ANCHOR, CLIENT_INSERT, "mcp_client.py")


def overlay_mcp_executor(text: str) -> str:
    """Refuse self_pet_request when a model calls it by name."""
    return _insert_after(text, EXECUTOR_ANCHOR, EXECUTOR_INSERT, "mcp_executor.py")


def overlay(text: str) -> str:
    if "from plugins_func.functions.child_voice_guard" in text:
        raise ValueError("Child guard already installed; refuse double injection")
    if text.count(ANCHOR) != 1:
        raise ValueError("Upstream ASR intent anchor changed; reject unsafe patch")
    if text.count("async def handle_user_intent(") != 1:
        raise ValueError("Upstream intent entrypoint no longer unique")
    patched = text.replace(ANCHOR, INSERT + ANCHOR, 1)
    compile(patched, "intentHandler.py", "exec")
    return patched


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True,
                        help="Pinned disposable upstream main/xiaozhi-server directory")
    parser.add_argument("--apply", action="store_true",
                        help="Explicitly modify only disposable source; default dry-run")
    args = parser.parse_args()
    source = args.source.resolve()
    repo = source.parent.parent
    sha = subprocess.check_output(
        ["git", "-C", str(repo), "rev-parse", "HEAD"], text=True,
    ).strip()
    if sha != PINNED_SERVER_COMMIT:
        raise RuntimeError("Self-hosted XiaoZhi commit mismatch; no patch")
    handler = source / "core" / "handle" / "intentHandler.py"
    plugin = source / "plugins_func" / "functions" / "child_voice_guard.py"
    device_mcp = source / "core" / "providers" / "tools" / "device_mcp"
    client, executor = device_mcp / "mcp_client.py", device_mcp / "mcp_executor.py"
    if not handler.is_file() or plugin.exists():
        raise RuntimeError("Missing upstream intent handler or child guard already exists")
    patched = overlay(handler.read_text(encoding="utf-8"))
    patched_client = overlay_mcp_client(client.read_text(encoding="utf-8"))
    patched_executor = overlay_mcp_executor(executor.read_text(encoding="utf-8"))
    module = (Path(__file__).resolve().parents[1] / "integrations" /
              "xiaozhi_server" / "child_voice_guard.py").read_text(encoding="utf-8")
    compile(module, str(plugin), "exec")
    if not args.apply:
        print("P3_CHILD_GUARD_DRY_RUN_OK; upstream unmodified")
        return
    # Validate all inputs before either file write. Caller owns disposable checkout.
    handler.write_text(patched, encoding="utf-8")
    client.write_text(patched_client, encoding="utf-8")
    executor.write_text(patched_executor, encoding="utf-8")
    plugin.write_text(module, encoding="utf-8")
    print("P3_CHILD_GUARD_INSTALLED_IN_DISPOSABLE_SERVER_CHECKOUT")


if __name__ == "__main__":
    main()
