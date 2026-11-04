#!/usr/bin/env python3
"""Integrated end-to-end smoke test of a Pixel3D build (CLI + MCP), as run for 1.1.0-alpha.

1. A real character drawing (Front only; oversize AI pixel-style art, normalised)
   as a single-drawing project in style soft: CLI and MCP, honest status,
   beauty renders at 0/90/180/270 degrees (crisp and soft).
2. A crisp fixture (D_character, Front/Side/Top) in style soft: CLI and MCP.
3. Every GLB through the Khronos glTF-Validator (0 errors required) and the
   packaged pixel3d-glbcheck.
4. One contact sheet with everything (needs Pillow).

usage: alpha_smoke.py BIN_DIR FIXTURES_DIR CHARACTER_FRONT_PNG OUT_DIR GLTF_VALIDATOR_EXE [CHARACTER_FRONT_WEBP]
BIN_DIR holds pixel3d-cli.exe, Pixel3D-mcp.exe and pixel3d-glbcheck.exe.
"""
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tests", "mcp"))
from e2e_client import McpClient  # noqa: E402

results = []


def note(ok, what):
    results.append((ok, what))
    print(f"  {'ok  ' if ok else 'FAIL'} {what}")
    return ok


def run(args, timeout=1800):
    p = subprocess.run(args, capture_output=True, timeout=timeout)
    return p.returncode, p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace")


def call(c, name, args=None):
    s, imgs, err = c.call(name, args or {})
    if err or not s["success"]:
        raise SystemExit(f"{name} failed: {s.get('errors')}")
    return s


def validate_gltf(validator, glb):
    code, out, err = run([validator, "-o", glb], 600)
    try:
        rep = json.loads(out)
        issues = rep["issues"]
        return code == 0 and issues["numErrors"] == 0, issues
    except (ValueError, KeyError):
        return False, {"error": err[-500:]}


def main():
    bin_dir, fx, front, out, validator = (os.path.abspath(a) for a in sys.argv[1:6])
    webp = os.path.abspath(sys.argv[6]) if len(sys.argv) > 6 else None
    cli = os.path.join(bin_dir, "pixel3d-cli.exe")
    mcp = os.path.join(bin_dir, "Pixel3D-mcp.exe")
    glbcheck = os.path.join(bin_dir, "pixel3d-glbcheck.exe")
    os.makedirs(out, exist_ok=True)
    summary = {}
    glbs = []

    print("CLI")
    code, txt, err = run([cli, "--version"])
    summary["version"] = txt.strip()
    note(code == 0, f"version: {txt.strip()}")
    d_fx = os.path.join(fx, "D_character")
    target = os.path.join(out, "cli_D_soft.glb")
    code, txt, err = run([cli, "reconstruct", "--front", os.path.join(d_fx, "front.png"), "--side",
                          os.path.join(d_fx, "side.png"), "--top", os.path.join(d_fx, "top.png"), "--style", "soft",
                          "--out", target, "--report", os.path.join(out, "cli_D_soft.json")])
    open(os.path.join(out, "cli_D_soft.txt"), "w", encoding="utf-8").write(txt + err)
    note(code == 0 and "STATUS: PASS" in txt and os.path.isfile(target), "D_character --style soft: PASS, GLB written")
    glbs.append(target)
    target = os.path.join(out, "cli_character_soft.glb")
    code, txt, err = run([cli, "reconstruct", "--front", front, "--single-view", "medium", "--style", "soft",
                          "--out", target, "--report", os.path.join(out, "cli_character_soft.json")])
    open(os.path.join(out, "cli_character_soft.txt"), "w", encoding="utf-8").write(txt + err)
    status = next((l.split("STATUS:")[1].strip() for l in txt.splitlines() if "STATUS:" in l), "none")
    summary["cli_character_status"] = status
    note(os.path.isfile(target) and status in ("PASS", "NEEDS_REVIEW"),
         f"character (Front only, normalised, single view medium, soft): status {status}, GLB written")
    note("input: Front:" in txt, "normalisation notes printed")
    glbs.append(target)
    if webp:
        code, txt, err = run([cli, "reconstruct", "--front", webp, "--single-view", "medium", "--out",
                              os.path.join(out, "webp.glb")])
        summary["webp"] = (txt + err).strip().splitlines()[0] if (txt + err).strip() else ""
        # 1.1.0 refused WebP with a clear message; 1.2.0 decodes it (libwebp).
        note(code in (0, 2) or "WebP" in (txt + err), f"WebP source read or refused clearly: {summary['webp']}")

    print("MCP")
    c = McpClient(mcp, os.path.join(out, "mcp_stderr.log"))
    c.request("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                             "clientInfo": {"name": "alpha-smoke", "version": "1"}})
    c.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
    turn = {"frames": 4, "size": 384, "elevation": 12, "style": "beauty"}
    # Character, crisp first (the model the user edits), then soft.
    call(c, "new_single_view_project", {"path": front, "name": "character", "style": "crisp"})
    s = call(c, "validate")
    summary["mcp_character_default_depth"] = {"status": s["status"],
                                              "blocking": [b["kind"] for b in s["validation"]["blocking_conflicts"]]}
    note(s["status"] == "NEEDS_REVIEW", f"default depth is not accepted silently: {s['status']}")
    call(c, "set_single_view_depth", {"preset": "medium"})
    s = call(c, "validate")
    summary["mcp_character_crisp"] = s["status"]
    call(c, "render_turntable", dict(turn, output_dir=os.path.join(out, "character_crisp")))
    call(c, "update_settings", {"style": "soft"})
    s = call(c, "validate")
    summary["mcp_character_soft"] = {"status": s["status"], "style": s["data"]["report"].get("style"),
                                     "dims": None}
    note(s["data"]["report"].get("style", {}).get("style") == "soft", f"character soft validated: {s['status']}")
    call(c, "render_turntable", dict(turn, output_dir=os.path.join(out, "character_soft")))
    glb = os.path.join(out, "mcp_character_soft.glb")
    args = {"path": glb}
    if s["status"] != "PASS":
        args["accept_status"] = s["status"]
    e = call(c, "export_model", args)
    note(os.path.isfile(glb), f"character soft GLB exported ({e['data'].get('bytes', '?')} bytes)")
    glbs.append(glb)
    insp = call(c, "inspect", {"what": "project"})
    summary["mcp_character_soft"]["dims"] = insp["data"].get("dims")
    # D fixture: crisp and soft.
    call(c, "new_project", {"name": "D", "style": "soft"})
    call(c, "import_views", {"front": os.path.join(d_fx, "front.png"), "side": os.path.join(d_fx, "side.png"),
                             "top": os.path.join(d_fx, "top.png")})
    s = call(c, "validate")
    summary["mcp_D_soft"] = {"status": s["status"], "style": s["data"]["report"].get("style")}
    note(s["status"] == "PASS", f"D_character soft via MCP: {s['status']}")
    call(c, "render_turntable", dict(turn, output_dir=os.path.join(out, "D_soft")))
    glb = os.path.join(out, "mcp_D_soft.glb")
    call(c, "export_model", {"path": glb})
    glbs.append(glb)
    call(c, "update_settings", {"style": "crisp"})
    call(c, "validate")
    call(c, "render_turntable", dict(turn, output_dir=os.path.join(out, "D_crisp")))
    c.close()

    print("independent checks")
    summary["gltf_validator"] = {}
    for g in glbs:
        ok, issues = validate_gltf(validator, g)
        summary["gltf_validator"][os.path.basename(g)] = issues
        note(ok, f"glTF-Validator {os.path.basename(g)}: {issues.get('numErrors')} errors, "
                 f"{issues.get('numWarnings')} warnings, {issues.get('numInfos')} infos")
        code, txt, _ = run([glbcheck, g], 300)
        note(code == 0, f"pixel3d-glbcheck {os.path.basename(g)}")

    try:
        contact_sheet(out, front)
        note(True, "contact sheet written: " + os.path.join(out, "contact_sheet.png"))
    except Exception as ex:  # noqa: BLE001
        note(False, f"contact sheet: {ex}")
    with open(os.path.join(out, "summary.json"), "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=2)
    failed = [w for ok, w in results if not ok]
    print(f"\n{len(results) - len(failed)}/{len(results)} checks passed")
    return 1 if failed else 0


def contact_sheet(out, front):
    from PIL import Image, ImageDraw
    rows = [("Character: crisp", "character_crisp"), ("Character: soft", "character_soft"),
            ("D_character: crisp", "D_crisp"), ("D_character: soft", "D_soft")]
    cell, label_w = 384, 300
    sheet = Image.new("RGB", (label_w + 4 * cell, len(rows) * cell + 40), (236, 237, 240))
    dr = ImageDraw.Draw(sheet)
    dr.text((10, 10), "Pixel3D 1.1.0-alpha smoke test: beauty renders at yaw 0 / 90 / 180 / 270", fill=(20, 20, 20))
    src = Image.open(front).convert("RGBA")
    src.thumbnail((label_w - 40, cell - 80))
    for r, (label, d) in enumerate(rows):
        y = 40 + r * cell
        dr.text((10, y + 10), label, fill=(20, 20, 20))
        if r < 2:
            bg = Image.new("RGBA", src.size, (236, 237, 240, 255))
            bg.alpha_composite(src)
            sheet.paste(bg.convert("RGB"), (10, y + 40))
        for k in range(4):
            f = os.path.join(out, d, f"frame_{k:02d}.png")
            if os.path.isfile(f):
                im = Image.open(f).convert("RGBA")
                bg = Image.new("RGBA", im.size, (236, 237, 240, 255))
                bg.alpha_composite(im)
                sheet.paste(bg.convert("RGB").resize((cell, cell)), (label_w + k * cell, y))
    sheet.save(os.path.join(out, "contact_sheet.png"))


if __name__ == "__main__":
    sys.exit(main())
