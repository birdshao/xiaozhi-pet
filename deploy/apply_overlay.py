#!/usr/bin/env python3
"""Install the child ASR guard into an upstream xiaozhi-server tree during the image build.

Reuses scripts/p3_server_overlay.overlay() (anchor checks, compile check) but not its
PINNED_SERVER_COMMIT constant: the commit is pinned by version-lock.json and verified by the
Dockerfile before this runs (see docs/DEPLOY_PET_SERVER.md, "pin mismatch").
"""
import argparse
from pathlib import Path

from p3_server_overlay import (TTS_EGRESS_PATHS, overlay, overlay_mcp_client, overlay_mcp_executor,
                               overlay_tts_egress)

parser = argparse.ArgumentParser()
parser.add_argument("--source", type=Path, required=True)
parser.add_argument("--module", type=Path, required=True)
args = parser.parse_args()
handler = args.source / "core" / "handle" / "intentHandler.py"
plugin = args.source / "plugins_func" / "functions" / "child_voice_guard.py"
device_mcp = args.source / "core" / "providers" / "tools" / "device_mcp"
client = device_mcp / "mcp_client.py"
executor = device_mcp / "mcp_executor.py"
if plugin.exists():
    raise SystemExit("child guard already present")
module = args.module.read_text(encoding="utf-8")
compile(module, str(plugin), "exec")
# Compute every patch before writing any (an anchor mismatch leaves upstream untouched).
patched_handler = overlay(handler.read_text(encoding="utf-8"))
patched_client = overlay_mcp_client(client.read_text(encoding="utf-8"))
patched_executor = overlay_mcp_executor(executor.read_text(encoding="utf-8"))
# S6 TTS egress audit: child mode closes every upstream TTS source outside the guard.
egress = {args.source.joinpath(*parts): overlay_tts_egress(name, args.source.joinpath(*parts).read_text(encoding="utf-8"))
          for name, parts in TTS_EGRESS_PATHS.items()}
handler.write_text(patched_handler, encoding="utf-8")
client.write_text(patched_client, encoding="utf-8")
executor.write_text(patched_executor, encoding="utf-8")
for path, text in egress.items():
    path.write_text(text, encoding="utf-8")
plugin.write_text(module, encoding="utf-8")
print("CHILD_GUARD_INSTALLED")
