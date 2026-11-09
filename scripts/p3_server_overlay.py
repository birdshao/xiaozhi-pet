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


# S6 TTS egress audit (docs/SERVER_S6.md §3): every upstream source that can make the
# device speak without going through the guard is closed in child mode. Each insert
# is a no-op unless KID_PET_CHILD_MODE=1 (the same test as child_mode_enabled()).
CHILD_TEST = '__import__("os").environ.get("KID_PET_CHILD_MODE", "").strip() == "1"'
TTS_EGRESS = {
    # receiveAudioHandle.py: daily output limit prompt (#18), bind-code audio (#19),
    # and the no-voice timeout "end prompt" fed to chat as if the child said it (#16).
    "receiveAudioHandle.py": (
        ('async def max_out_size(conn: "ConnectionHandler"):\n',
         f"    if {CHILD_TEST}:  # child mode: no upstream prompt (S6 audit #18)\n"
         "        return\n", "after"),
        ('async def check_bind_device(conn: "ConnectionHandler"):\n',
         f"    if {CHILD_TEST}:  # child mode: no bind-code audio (S6 audit #19)\n"
         "        return\n", "after"),
        ("            await startToChat(conn, prompt)\n",
         f"            if {CHILD_TEST}:  # child mode: end silently, no model goodbye (S6 audit #16)\n"
         "                await conn.close()\n"
         "                return\n", "before"),
    ),
    # listenMessageHandler.py: "[device_call]" text is spoken verbatim, bypassing the guard (#15).
    # A wake word "detect" (#12): with enable_greeting false upstream sends stt + tts start +
    # tts stop at once and nothing else, so the board flips speaking -> listening in the same
    # millisecond and the first wake gets no answer. Child mode hands the board's own wake
    # text to the guard instead (PetLearning answers it with the fixed wake greeting), and
    # mutes VAD for 2 s like upstream's greeting branches. Non-child mode is unchanged.
    "listenMessageHandler.py": (
        ('                    call_text = original_text[len("[device_call]"):].strip()\n',
         f"                    if {CHILD_TEST}:  # child mode: never speak client text (S6 audit #15)\n"
         "                        return\n", "after"),
        ("                if is_wakeup_words and not enable_greeting:\n",
         f"                if is_wakeup_words and {CHILD_TEST}:  # child mode: the guard greets (S6 audit #12)\n"
         "                    conn.just_woken_up = True\n"
         "                    await startToChat(conn, original_text)\n"
         "                    return\n", "before"),
    ),
    # connection.py: the normal LLM chat (#1-#6: stream, tool replies, system_error_response).
    "connection.py": (
        ("    def chat(self, query, depth=0):\n",
         f"        if {CHILD_TEST}:  # child mode: the guard owns every reply (S6 audit #1-#6)\n"
         "            return None\n", "after"),
    ),
}


def overlay_tts_egress(name: str, text: str) -> str:
    """Apply the S6 child-mode patches for one upstream file; strict anchors, compile check."""
    if "(S6 audit #" in text:
        raise ValueError(name + ": TTS egress patch already installed")
    for anchor, insert, where in TTS_EGRESS[name]:
        if text.count(anchor) != 1:
            raise ValueError(name + ": upstream anchor changed; reject unsafe patch")
        text = text.replace(anchor, anchor + insert if where == "after" else insert + anchor, 1)
    compile(text, name, "exec")
    return text


TTS_EGRESS_PATHS = {
    "receiveAudioHandle.py": ("core", "handle", "receiveAudioHandle.py"),
    "listenMessageHandler.py": ("core", "handle", "textHandler", "listenMessageHandler.py"),
    "connection.py": ("core", "connection.py"),
}


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
    egress = {source.joinpath(*parts): overlay_tts_egress(name, source.joinpath(*parts).read_text(encoding="utf-8"))
              for name, parts in TTS_EGRESS_PATHS.items()}
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
    for path, patched_text in egress.items():
        path.write_text(patched_text, encoding="utf-8")
    plugin.write_text(module, encoding="utf-8")
    print("P3_CHILD_GUARD_INSTALLED_IN_DISPOSABLE_SERVER_CHECKOUT")


if __name__ == "__main__":
    main()
