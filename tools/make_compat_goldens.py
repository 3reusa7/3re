#!/usr/bin/env python3
"""Writes the cross-version compatibility goldens with a RELEASED Pixel3D-mcp.

Each project is built and saved by the old release, and the GLB that release
exported from it is recorded (SHA-256 of the bytes). tests/test_compat.cpp then
opens the same files with the current engine and must export byte-identical
GLBs (with the generator string pinned to the old version).

usage: make_compat_goldens.py OLD_MCP_EXE FIXTURES_DIR OUT_DIR
Run once per released format; the outputs are committed under
tests/fixtures/golden/. Standard library only.
"""
import hashlib
import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tests", "mcp"))
from e2e_client import McpClient  # noqa: E402


def call(c, name, args=None):
    s, _, err = c.call(name, args or {})
    if err or not s["success"]:
        raise SystemExit(f"{name} failed: {s.get('errors')}")
    return s


def main():
    exe, fixtures, out = sys.argv[1:4]
    os.makedirs(out, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix="p3d_compat_")
    c = McpClient(exe, os.path.join(tmp, "server.log"))
    init = c.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                    "clientInfo": {"name": "compat-goldens", "version": "1"}})
    c.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
    version = init["result"]["serverInfo"]["version"]
    expected = {"generator": f"Pixel3D {version}", "projects": {}}

    def export(name, accept=False):
        glb = os.path.join(tmp, name + ".glb")
        args = {"path": glb}
        if accept:
            args["accept_status"] = call(c, "validate")["status"]
        call(c, "export_model", args)
        with open(glb, "rb") as f:
            return hashlib.sha256(f.read()).hexdigest()

    def fx(name, view):
        return os.path.join(fixtures, name, view + ".png")

    # The format-1 golden that is already in the repository, as the old release exports it.
    call(c, "open_project", {"path": os.path.join(out, "project_v1.p3d")})
    expected["projects"]["project_v1.p3d"] = {"glb": export("v1")}

    # Format 2 with editor inputs: a user part painted on the views, an added volume and a push/pull.
    call(c, "new_project", {"name": "compat edits", "profile": "hand"})
    call(c, "import_views", {"front": fx("B_hand_chair", "front"), "side": fx("B_hand_chair", "side"),
                             "top": fx("B_hand_chair", "top")})
    call(c, "reconstruct")
    part = call(c, "create_part", {"name": "label"})["data"]["part"]
    call(c, "paint_part_mask", {"part": part, "view": "front", "rect": [0, 0, 4, 4]})
    call(c, "add_volume", {"view": "front", "rect": [1, 1, 3, 3], "depth_from": 0, "depth_to": 2})
    call(c, "push_pull", {"view": "front", "u": 2, "v": 2, "distance": -1})
    call(c, "save_project", {"path": os.path.join(out, "project_v2_edits.p3d")})
    expected["projects"]["project_v2_edits.p3d"] = {"glb": export("v2_edits", accept=True)}

    # Format 2, single drawing with a chosen depth.
    call(c, "new_single_view_project", {"path": fx("A_synthetic_chair", "front"), "preset": "thick",
                                        "name": "compat single"})
    call(c, "save_project", {"path": os.path.join(out, "project_v2_single.p3d")})
    expected["projects"]["project_v2_single.p3d"] = {"glb": export("v2_single", accept=True)}

    c.close()
    with open(os.path.join(out, "compat_expected.json"), "w", encoding="utf-8") as f:
        json.dump(expected, f, indent=2)
        f.write("\n")
    print(json.dumps(expected, indent=2))


if __name__ == "__main__":
    main()
