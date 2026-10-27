#!/usr/bin/env python3
"""Narrow ASR guard overlay for an EXACT pinned upstream XiaoZhi server checkout.

No hardware I/O, no ASR/Opus protocol modifications. Dry-run unless --apply.
Run only on a disposable upstream server checkout (not production).
"""
from __future__ import annotations
import argparse
from pathlib import Path
import subprocess

PINNED_SERVER_COMMIT = "f1965167cc2993556cd3e963f8657e2686006c77"
ANCHOR = "    # 检查是否有明确的退出命令"
INSERT = """    # Child profile uses reviewed PetLearning text before any general LLM path.
    # Non-child profile: the guard returns False and upstream works unchanged.
    from plugins_func.functions.child_voice_guard import handle_child_guard
    if await handle_child_guard(conn, text):
        return True

"""


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
    if not handler.is_file() or plugin.exists():
        raise RuntimeError("Missing upstream intent handler or child guard already exists")
    patched = overlay(handler.read_text(encoding="utf-8"))
    module = (Path(__file__).resolve().parents[1] / "integrations" /
              "xiaozhi_server" / "child_voice_guard.py").read_text(encoding="utf-8")
    compile(module, str(plugin), "exec")
    if not args.apply:
        print("P3_CHILD_GUARD_DRY_RUN_OK; upstream unmodified")
        return
    # Validate all inputs before either file write. Caller owns disposable checkout.
    handler.write_text(patched, encoding="utf-8")
    plugin.write_text(module, encoding="utf-8")
    print("P3_CHILD_GUARD_INSTALLED_IN_DISPOSABLE_SERVER_CHECKOUT")


if __name__ == "__main__":
    main()
