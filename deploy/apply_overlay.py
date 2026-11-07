#!/usr/bin/env python3
"""Install the child ASR guard into an upstream xiaozhi-server tree during the image build.

Reuses scripts/p3_server_overlay.overlay() (anchor checks, compile check) but not its
PINNED_SERVER_COMMIT constant: the commit is pinned by version-lock.json and verified by the
Dockerfile before this runs (see docs/DEPLOY_PET_SERVER.md, "pin mismatch").
"""
import argparse
from pathlib import Path

from p3_server_overlay import overlay

parser = argparse.ArgumentParser()
parser.add_argument("--source", type=Path, required=True)
parser.add_argument("--module", type=Path, required=True)
args = parser.parse_args()
handler = args.source / "core" / "handle" / "intentHandler.py"
plugin = args.source / "plugins_func" / "functions" / "child_voice_guard.py"
if plugin.exists():
    raise SystemExit("child guard already present")
module = args.module.read_text(encoding="utf-8")
compile(module, str(plugin), "exec")
handler.write_text(overlay(handler.read_text(encoding="utf-8")), encoding="utf-8")
plugin.write_text(module, encoding="utf-8")
print("CHILD_GUARD_INSTALLED")
