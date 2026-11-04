#!/usr/bin/env python3
"""End-to-end test of Pixel3D-mcp as a real MCP client over stdio.

Standard library only: the server is driven exactly as any MCP-compatible
client would drive it (JSON-RPC 2.0, one message per line). Nothing here calls
the engine directly. Fixtures A and B must reach PASS; C must stay NEEDS_REVIEW
(export refused) until the leg correspondence and cavity depth are supplied,
then PASS. The M4 editing tools are driven the same way: an edit that breaks
the drawings is reported at once and blocks PASS, masks resolve C, and a
single-view project never claims multi-view accuracy.

usage: e2e_client.py SERVER_EXE FIXTURES_DIR OUT_DIR [GLBCHECK_EXE]
"""
import base64
import json
import os
import queue
import struct
import subprocess
import sys
import threading
import time
import zlib

PROTOCOL = "2025-06-18"
REQUIRED_TOOLS = {
    "new_project", "open_project", "save_project", "import_views", "auto_align", "analyze_views",
    "segment_parts", "match_parts", "reconstruct", "get_conflicts", "get_inferred_geometry",
    "set_part_depth", "edit_part", "cleanup_mesh", "project_textures", "validate", "render_view",
    "render_turntable", "undo", "redo", "checkpoint", "restore_checkpoint", "export_glb",
    # M4 editor
    "paint_depth", "paint_part_mask", "split_part", "merge_parts", "push_pull", "add_volume", "erase_volume",
    "extrude", "cut", "mirror", "transform_part", "set_selection", "get_selection", "create_part", "rename_part",
    "delete_part", "duplicate_part", "sync_part_masks", "preview_edit", "new_single_view_project",
    "set_single_view_depth", "reload_views",
    # M5 export
    "export_model",
    # 1.1 polished styles
    "set_style",
    # 1.2 Low-Poly Studio
    "set_lowpoly_settings", "set_part_type", "lowpoly_suggest_parts", "lowpoly_build", "lowpoly_render",
    "lowpoly_export",
    # 1.2 layered inputs
    "suggest_layer_parts",
}
REQUIRED_RESOURCES = {
    "pixel3d://project", "pixel3d://views", "pixel3d://parts", "pixel3d://mesh/stats",
    "pixel3d://conflicts", "pixel3d://diagnostics", "pixel3d://validation",
}

failures = []


def check(cond, what):
    if cond:
        print(f"  ok   {what}")
    else:
        print(f"  FAIL {what}")
        failures.append(what)
    return cond


class McpClient:
    def __init__(self, exe, log_path):
        self.log = open(log_path, "wb")
        self.proc = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.log)
        self.lines = queue.Queue()
        self.next_id = 1
        self.notifications = []
        self.bad_lines = []
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        for raw in self.proc.stdout:
            self.lines.put(raw)
        self.lines.put(None)

    def send(self, obj):
        self.proc.stdin.write((json.dumps(obj) + "\n").encode("utf-8"))
        self.proc.stdin.flush()

    def send_raw(self, text):
        self.proc.stdin.write(text.encode("utf-8") + b"\n")
        self.proc.stdin.flush()

    def receive(self, timeout=600):
        raw = self.lines.get(timeout=timeout)
        if raw is None:
            raise RuntimeError("server closed stdout")
        try:
            msg = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            self.bad_lines.append(raw)
            raise
        if not isinstance(msg, (dict, list)) or (isinstance(msg, dict) and msg.get("jsonrpc") != "2.0"):
            self.bad_lines.append(raw)
        return msg

    def wait_for(self, rid, timeout=600):
        while True:
            msg = self.receive(timeout)
            if isinstance(msg, dict) and "id" not in msg:
                self.notifications.append(msg)
                continue
            return msg

    def request(self, method, params=None, meta=None):
        rid = self.next_id
        self.next_id += 1
        msg = {"jsonrpc": "2.0", "id": rid, "method": method}
        if params is not None:
            msg["params"] = dict(params)
        if meta:
            msg.setdefault("params", {})["_meta"] = meta
        self.send(msg)
        resp = self.wait_for(rid)
        assert resp.get("id") == rid, f"response id {resp.get('id')} for request {rid}"
        return resp

    def call(self, name, arguments=None, progress=False):
        """tools/call; returns (structured result, image blocks, isError)."""
        meta = {"progressToken": f"p{self.next_id}"} if progress else None
        resp = self.request("tools/call", {"name": name, "arguments": arguments or {}}, meta)
        if "error" in resp:
            raise RuntimeError(f"{name}: protocol error {resp['error']}")
        result = resp["result"]
        text = [c for c in result["content"] if c["type"] == "text"]
        images = [c for c in result["content"] if c["type"] == "image"]
        structured = result.get("structuredContent")
        assert structured is not None, "structuredContent missing"
        assert json.loads(text[0]["text"]) == structured, "text block must equal structuredContent"
        return structured, images, result["isError"]

    def close(self):
        self.proc.stdin.close()
        code = self.proc.wait(timeout=60)
        self.log.close()
        return code


def png_size(b64):
    data = base64.b64decode(b64)
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    w, h = struct.unpack(">II", data[16:24])
    # Decompress to make sure the image data is intact.
    idat = b""
    pos = 8
    while pos < len(data):
        n = struct.unpack(">I", data[pos:pos + 4])[0]
        kind = data[pos + 4:pos + 8]
        if kind == b"IDAT":
            idat += data[pos + 8:pos + 8 + n]
        pos += 12 + n
    zlib.decompress(idat)
    return w, h


def ok_result(r, what):
    s, imgs, is_error = r
    check(s["success"] and not is_error, f"{what}: success" + ("" if s["success"] else f" ({s['errors']})"))
    return s, imgs


def blocking(s):
    return sorted(c["kind"] for c in (s.get("validation") or {}).get("blocking_conflicts", []))


def glbcheck(exe, path):
    if not exe:
        return
    p = subprocess.run([exe, path], capture_output=True, text=True)
    rep = json.loads(p.stdout) if p.stdout.strip() else {}
    check(p.returncode == 0 and rep.get("ok"), f"independent tinygltf check of {os.path.basename(path)}: {rep.get('problems')}")


def glb_variant(src, dst, edit):
    """A copy of a GLB with its JSON chunk changed by edit(json) (binary chunk unchanged)."""
    import struct
    with open(src, "rb") as f:
        data = f.read()
    jlen = struct.unpack_from("<I", data, 12)[0]
    doc = json.loads(data[20:20 + jlen])
    rest = data[20 + jlen:]
    edit(doc)
    js = json.dumps(doc, separators=(",", ":")).encode()
    js += b" " * (-len(js) % 4)
    body = struct.pack("<II", len(js), 0x4E4F534A) + js + rest
    with open(dst, "wb") as f:
        f.write(b"glTF" + struct.pack("<II", 2, 12 + len(body)) + body)


def glbcheck_rejects(exe, path, what):
    p = subprocess.run([exe, path], capture_output=True, text=True)
    rep = json.loads(p.stdout) if p.stdout.strip() else {}
    check(p.returncode != 0 and not rep.get("ok"), f"independent check rejects {what}: {rep.get('problems')}")


def lowpoly_glbcheck(exe, glb, out):
    """P2 LP-4: the independent checker accepts a valid skinned GLB (mesh node + skin joints) and
    still rejects genuinely broken node layouts."""
    if not exe:
        return
    glbcheck(exe, glb)
    mesh_node = lambda d: next(i for i, n in enumerate(d["nodes"]) if "mesh" in n)  # noqa: E731

    def stray(d):  # a node that is neither the mesh nor a joint
        d["nodes"].append({"name": "stray"})
        d["scenes"][0]["nodes"].append(len(d["nodes"]) - 1)

    def no_skin(d):  # joints present but the mesh does not use the skin
        del d["nodes"][mesh_node(d)]["skin"]

    def two_meshes(d):  # a second node drawing the mesh
        d["nodes"].append({"mesh": 0})
        d["scenes"][0]["nodes"].append(len(d["nodes"]) - 1)

    def bad_joint(d):  # the skin names the mesh node as a joint
        d["skins"][0]["joints"][-1] = mesh_node(d)

    def orphan_joint(d):  # a joint that is not in the scene
        for n in d["nodes"]:
            n["children"] = [k for k in n.get("children", []) if k != d["skins"][0]["joints"][-1]]
        d["scenes"][0]["nodes"] = [k for k in d["scenes"][0]["nodes"] if k != d["skins"][0]["joints"][-1]]

    for name, edit in (("stray", stray), ("no_skin", no_skin), ("two_meshes", two_meshes), ("bad_joint", bad_joint),
                       ("orphan_joint", orphan_joint)):
        bad = os.path.join(out, f"lowpoly_broken_{name}.glb")
        glb_variant(glb, bad, edit)
        glbcheck_rejects(exe, bad, f"a skinned GLB with a broken node layout ({name})")


def protocol_checks(c):
    print("protocol")
    init = c.request("initialize", {"protocolVersion": PROTOCOL, "capabilities": {},
                                    "clientInfo": {"name": "pixel3d-e2e", "version": "1"}})
    res = init["result"]
    check(res["protocolVersion"] == PROTOCOL, "protocol version negotiated")
    check(set(res["capabilities"]) >= {"tools", "resources", "prompts"}, "capabilities advertised")
    check("NEEDS_REVIEW" in res.get("instructions", ""), "instructions explain the status rules")
    c.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
    tools = c.request("tools/list")["result"]["tools"]
    names = {t["name"] for t in tools}
    check(REQUIRED_TOOLS <= names, f"all required tools listed ({len(names)} tools)")
    check(all(t["inputSchema"]["type"] == "object" for t in tools), "every tool has an object input schema")
    check(all("annotations" in t and "title" in t for t in tools), "tools carry titles and annotations")
    uris = {r["uri"] for r in c.request("resources/list")["result"]["resources"]}
    check(REQUIRED_RESOURCES <= uris, "all required resources listed")
    check(len(c.request("resources/templates/list")["result"]["resourceTemplates"]) == 2, "view image templates listed")
    prompts = c.request("prompts/list")["result"]["prompts"]
    check(any(p["name"] == "reconstruct_from_views" for p in prompts), "workflow prompt listed")
    p = c.request("prompts/get", {"name": "reconstruct_from_views", "arguments": {"front": "f.png"}})["result"]
    text = p["messages"][0]["content"]["text"]
    check("Repair only when determinable" in text and "f.png" in text, "workflow prompt carries the skill")
    check(c.request("ping")["result"] == {}, "ping")
    check(c.request("no/such/method")["error"]["code"] == -32601, "unknown method -> -32601")
    check(c.request("tools/call", {"name": "nope"})["error"]["code"] == -32602, "unknown tool -> -32602")
    s, _, err = c.call("import_views", {"front": "x.png", "nonsense": 1})
    check(err and s["errors"][0]["code"] == "input.invalid", "unknown argument -> tool error input.invalid")
    s, _, err = c.call("import_views", {"views": [{"role": "diagonal", "path": "x.png"}]})
    check(err and "diagonal" in s["errors"][0]["text"], "bad role -> tool error naming the value")
    c.send_raw("this is not json")
    check(c.wait_for(None)["error"]["code"] == -32700, "garbage line -> -32700")
    check(c.request("resources/read", {"uri": "pixel3d://nope"})["error"]["code"] == -32002, "unknown resource -> -32002")
    batch_ids = [c.next_id, c.next_id + 1]
    c.next_id += 2
    c.send([{"jsonrpc": "2.0", "id": batch_ids[0], "method": "ping"},
            {"jsonrpc": "2.0", "id": batch_ids[1], "method": "ping"}])
    batch = c.wait_for(None)
    check(isinstance(batch, list) and sorted(m["id"] for m in batch) == batch_ids, "batch answered as a batch")


def fixture(fx, name, role):
    return os.path.join(fx, name, role + ".png")


def views(fx, name):
    return {r: fixture(fx, name, r) for r in ("front", "side", "top")}


def run_a(c, fx, out, glbcheck_exe):
    print("fixture A (synthetic chair, strict) - full workflow")
    s, _ = ok_result(c.call("new_project", {"name": "chair_A", "profile": "strict"}), "new_project")
    s, _ = ok_result(c.call("inspect", {"what": "project"}), "inspect project")
    check(s["data"]["stage"] == "empty", "fresh project is empty")
    s, _ = ok_result(c.call("import_views", views(fx, "A_synthetic_chair")), "import_views")
    check(len(s["changed_ids"]["views"]["added"]) == 3, "changed_ids lists the three views")
    s, _ = ok_result(c.call("auto_align"), "auto_align")
    check(s["data"]["dims"] == [18, 33, 20], f"dims {s['data']['dims']}")
    ok_result(c.call("analyze_views"), "analyze_views")
    ok_result(c.call("segment_parts"), "segment_parts")
    s, _ = ok_result(c.call("match_parts"), "match_parts")
    check(len(s["data"]["parts"]) == 5, "five parts (seat, back, four legs)")
    before = len(c.notifications)
    s, _ = ok_result(c.call("reconstruct", {"mode": "true_solid"}, progress=True), "reconstruct")
    check(s["stats"]["watertight"] and s["stats"]["triangles"] <= 600, f"watertight, {s['stats']['triangles']} triangles")
    check(any(n.get("method") == "notifications/progress" for n in c.notifications[before:]), "progress notifications")
    s, _ = ok_result(c.call("get_conflicts"), "get_conflicts")
    check(not any(x["blocks_pass"] for x in s["data"]["conflicts"]), "no blocking conflicts")
    s, _ = ok_result(c.call("get_inferred_geometry"), "get_inferred_geometry")
    check(any(x["status"] == "inferred_grid" for x in s["data"]["candidates"]), "legs reported as inferred_grid")
    ok_result(c.call("project_textures"), "project_textures")
    s, _ = ok_result(c.call("validate"), "validate")
    check(s["status"] == "PASS", f"A status {s['status']} {blocking(s)}")
    s, imgs = ok_result(c.call("render_view", {"compare": True, "pixels_per_unit": 4}), "render_view compare")
    sizes = [png_size(i["data"]) for i in imgs]
    check(len(imgs) == 6 and all(sizes), f"six PNG images inline {sizes}")
    cmp_ok = all(i["pixels"]["drawn_not_rendered"] == 0 and i["pixels"]["rendered_not_drawn"] == 0
                 for i in s["data"]["images"] if "pixels" in i)
    check(cmp_ok, "compare images: every drawn pixel rendered, nothing extra")
    s, imgs = ok_result(c.call("render_view", {"yaw": 45, "elevation": 25, "size": 192}), "render_view orbit")
    check(len(imgs) == 1 and png_size(imgs[0]["data"]) == (192, 192), "orbit image 192x192")
    s, imgs = ok_result(c.call("render_turntable", {"frames": 24, "size": 64,
                                                    "output_dir": os.path.join(out, "A_turntable")}),
                        "render_turntable")
    check(len(imgs) == 1 and png_size(imgs[0]["data"]) == (384, 256), "contact sheet inline (6x4 of 64 px)")
    check(len(s["data"]["files"]) == 25, "turntable frames + sheet written to output_dir")
    ok_result(c.call("cleanup_mesh", {"min_fragment_volume": 0.5}), "cleanup_mesh")
    s, _ = ok_result(c.call("validate"), "revalidate")
    check(s["status"] == "PASS", "A still PASS after cleanup")
    ok_result(c.call("checkpoint", {"name": "validated"}), "checkpoint")
    s, _ = ok_result(c.call("set_part_depth", {"view": "top", "rect": [4, 6, 10, 12], "depth_from": 1}), "experiment")
    check(s["status"] == "NOT_VALIDATED", "an edit invalidates the status")
    s, _ = ok_result(c.call("undo"), "undo")
    check(s["changed_ids"].get("depth_constraints", {}).get("after") == 0, "undo removed the constraint")
    s, _ = ok_result(c.call("redo"), "redo")
    s, _ = ok_result(c.call("restore_checkpoint", {"name": "validated"}), "restore_checkpoint")
    s, _ = ok_result(c.call("validate"), "validate after restore")
    check(s["status"] == "PASS", "restored state is PASS")
    glb = os.path.join(out, "A.glb")
    s, _ = ok_result(c.call("export_glb", {"path": glb}), "export_glb")
    check(os.path.getsize(glb) == s["data"]["bytes"] and s["data"]["pixels_per_meter"] == 32, "GLB written at project scale")
    glbcheck(glbcheck_exe, glb)
    # Engine presets and other formats: every file is reloaded and re-rendered before it is written.
    for preset, name in (("roblox", "A_roblox.obj"), ("godot", "A_godot.gltf")):
        target = os.path.join(out, name)
        s, _ = ok_result(c.call("export_model", {"path": target, "preset": preset}), f"export_model {preset}")
        files = s["data"]["files"]
        check(s["data"]["check"]["ok"] and len(files) == 3 and all(os.path.exists(f) for f in files),
              f"{preset}: {s['data']['format']} in {s['data']['units']}, front {s['data']['front']}, 3 files")
    s, _, err = c.call("export_model", {"path": os.path.join(out, "x.obj"), "format": "glb"})
    check(err and s["errors"][0]["code"] == "input.invalid", "extension/format mismatch refused")
    p3d = os.path.join(out, "A.p3d")
    ok_result(c.call("save_project", {"path": p3d}), "save_project")
    ok_result(c.call("open_project", {"path": p3d}), "open_project")
    s, _ = ok_result(c.call("validate"), "validate reopened project")
    check(s["status"] == "PASS", "reopened project is PASS")
    v = json.loads(c.request("resources/read", {"uri": "pixel3d://validation"})["result"]["contents"][0]["text"])
    check(v["status"] == "PASS" and v["schema"] == "pixel3d.report/1", "validation resource")
    blob = c.request("resources/read", {"uri": "pixel3d://views/front/aligned.png"})["result"]["contents"][0]["blob"]
    check(png_size(blob) == (18, 33), "aligned front view as a resource")
    for uri in sorted(REQUIRED_RESOURCES):
        j = json.loads(c.request("resources/read", {"uri": uri})["result"]["contents"][0]["text"])
        check(isinstance(j, dict) and j, f"resource {uri}")


def run_b(c, fx, out, glbcheck_exe):
    print("fixture B (hand-drawn chair, hand profile)")
    ok_result(c.call("new_project", {"name": "chair_B", "profile": "hand"}), "new_project")
    ok_result(c.call("import_views", views(fx, "B_hand_chair")), "import_views")
    ok_result(c.call("auto_align"), "auto_align")
    ok_result(c.call("reconstruct"), "reconstruct")
    s, _ = ok_result(c.call("get_conflicts"), "get_conflicts")
    ok_result(c.call("project_textures"), "project_textures")
    s, _ = ok_result(c.call("validate"), "validate")
    check(s["status"] == "PASS", f"B status {s['status']} {blocking(s)}")
    exact = [s["metrics"]["views"][r]["color_exact_fraction"] for r in ("front", "side", "top")]
    check(all(e >= 0.98 for e in exact), f"crisp texture (colour exact {exact})")
    s, imgs = ok_result(c.call("render_turntable", {"size": 64}), "render_turntable")
    check(len(imgs) == 1, "contact sheet")
    glb = os.path.join(out, "B.glb")
    ok_result(c.call("export_glb", {"path": glb}), "export_glb")
    glbcheck(glbcheck_exe, glb)


def classify(fp, dims_w, dims_d):
    """Footprint [x0, z0, x1, z1] in Top pixels -> (left|middle|right, rear|front)."""
    cx, cz = (fp[0] + fp[2]) / 2, (fp[1] + fp[3]) / 2
    side = "left" if cx < dims_w / 3 else "right" if cx > 2 * dims_w / 3 else "middle"
    depth = "front" if cz > dims_d / 2 else "rear"
    return side, depth


def run_c(c, fx, out, glbcheck_exe):
    print("fixture C (asymmetric planter) - stays NEEDS_REVIEW until constraints are supplied")
    ok_result(c.call("new_project", {"name": "planter_C", "profile": "strict"}), "new_project")
    ok_result(c.call("import_views", views(fx, "C_asym_planter")), "import_views")
    s, _ = ok_result(c.call("auto_align"), "auto_align")
    W, H, D = s["data"]["dims"]
    ok_result(c.call("reconstruct"), "reconstruct")
    ok_result(c.call("project_textures"), "project_textures")
    s, _ = ok_result(c.call("validate"), "validate")
    check(s["status"] == "NEEDS_REVIEW", f"C status {s['status']}")
    check(blocking(s) == ["ambiguous_correspondence", "possible_recess"], f"blocking kinds {blocking(s)}")
    glb = os.path.join(out, "C.glb")
    if os.path.exists(glb):
        os.remove(glb)
    s, _, err = c.call("export_glb", {"path": glb})
    check(err and s["errors"][0]["code"] == "export.needs_review" and not os.path.exists(glb),
          "export refuses NEEDS_REVIEW without explicit acceptance")
    s, _ = ok_result(c.call("get_conflicts"), "get_conflicts")
    amb = s["data"]["ambiguous_candidates"]
    check(len(amb) == 6, "six ambiguous leg candidates (3 front strands x 2 side strands)")
    recess = [i for i, x in enumerate(s["data"]["conflicts"]) if x["kind"] == "possible_recess"]
    check(len(recess) == 1, "one possible recess")

    # Guessing is refused: removing every candidate cannot explain the drawings.
    ok_result(c.call("checkpoint", {"name": "before-decisions"}), "checkpoint")
    s, _ = ok_result(c.call("edit_part", {"remove": [x["candidate"] for x in amb]}), "edit_part remove all")
    check(any(w["code"] == "edit.decision_refused" for w in s["warnings"]), "impossible removals are refused")
    s, _ = ok_result(c.call("validate"), "validate")
    check(s["status"] == "NEEDS_REVIEW", "still NEEDS_REVIEW after a refused edit")
    ok_result(c.call("restore_checkpoint", {"name": "before-decisions"}), "restore_checkpoint")

    # Constraint 1 (from the user): three legs - front-left, rear-middle, front-right.
    wanted = {("left", "front"), ("middle", "rear"), ("right", "front")}
    keep = [x["candidate"] for x in amb if classify(x["footprint"], W, D) in wanted]
    remove = [x["candidate"] for x in amb if x["candidate"] not in keep]
    check(len(keep) == 3, f"user statement maps to candidates {keep}")
    s, _ = ok_result(c.call("edit_part", {"keep": keep, "remove": remove}), "edit_part (legs)")
    check(not s["data"]["refused_decisions"], "leg decisions accepted")
    s, _ = ok_result(c.call("validate"), "validate")
    check(s["status"] == "NEEDS_REVIEW" and blocking(s) == ["possible_recess"],
          f"legs resolved, cavity still undetermined: {s['status']} {blocking(s)}")

    # Constraint 2 (from the user): the top is an open cavity 11 px deep.
    s, _ = ok_result(c.call("get_conflicts"), "get_conflicts")
    idx = [i for i, x in enumerate(s["data"]["conflicts"]) if x["kind"] == "possible_recess"][0]
    s, _ = ok_result(c.call("set_part_depth", {"conflict_index": idx, "depth_from": 11, "label": "cavity"}),
                     "set_part_depth (cavity)")
    s, _ = ok_result(c.call("validate"), "validate")
    check(s["status"] == "PASS", f"C PASS once determined: {s['status']} {blocking(s)}")
    s, imgs = ok_result(c.call("render_view", {"yaw": 30, "elevation": 55, "size": 256}), "render cavity from above")
    check(len(imgs) == 1, "cavity render")
    s, imgs = ok_result(c.call("render_turntable", {"size": 64}), "render_turntable")
    ok_result(c.call("export_glb", {"path": glb}), "export_glb")
    glbcheck(glbcheck_exe, glb)
    ok_result(c.call("save_project", {"path": os.path.join(out, "C.p3d")}), "save_project")


def run_edit_a(c, fx, out, glbcheck_exe):
    print("editing A through MCP (selection, depth brush, push/pull, mirror, undo, save/reload)")
    ok_result(c.call("new_project", {"name": "edit_A", "profile": "strict"}), "new_project")
    ok_result(c.call("import_views", views(fx, "A_synthetic_chair")), "import_views")
    ok_result(c.call("project_textures"), "project_textures")
    s, _ = ok_result(c.call("validate"), "validate")
    check(s["status"] == "PASS", "A PASS before editing")
    v0 = s["stats"]["volume"]

    # 2D -> 3D: a Front pixel of the front-left leg selects that part; its masks come back for every view.
    s, _ = ok_result(c.call("set_selection", {"view": "front", "u": 1, "v": 30, "mode": "part"}), "set_selection")
    leg = s["data"]["parts"][0] if s["data"]["parts"] else None
    check(leg is not None and len(s["data"]["parts"]) == 1, f"one part selected ({s['data']['parts']})")
    s, _ = ok_result(c.call("get_selection"), "get_selection")
    check(set(s["data"]["views"]) == {"front", "side", "top"}, "selection projected into Front/Side/Top")
    check(s["data"]["views"]["front"]["box"][0] <= 1, "front mask covers the clicked pixel")

    # Depth brush: lowering the seat is one undoable stroke that keeps the drawings intact.
    stroke = {"view": "top", "stroke": [[6.5, 12.5], [11.5, 12.5]], "radius": 1.5, "hardness": 1, "mode": "lower"}
    s, _ = ok_result(c.call("preview_edit", {"edit": {"op": "depth_stroke", "view": "top",
                                                      "samples": [[9, 12, -1]]}}), "preview_edit")
    check(s["data"]["volume"] < v0 and s["stats"]["volume"] == v0, "preview shows the carved volume, model unchanged")
    s, _ = ok_result(c.call("inspect", {"what": "edits"}), "inspect edits")
    check(s["data"]["count"] == 0, "preview committed nothing")
    s, _ = ok_result(c.call("paint_depth", stroke), "paint_depth lower")
    check(s["data"]["edit"]["op"] == "depth_stroke" and s["stats"]["volume"] < v0, "stroke carved the seat")
    check("live" in s["data"] and s["data"]["live"]["views"], "live metrics returned with the edit")
    check(s["status"] == "NOT_VALIDATED", "an edit invalidates the status")

    # Raising past the drawing is reported at once and blocks PASS / export.
    s, _ = ok_result(c.call("paint_depth", {"view": "top", "stroke": [[9.5, 4.5]], "radius": 0.5, "strength": 3,
                                            "mode": "raise"}), "paint_depth raise")
    check(any(w["code"] == "edit.projection_changed" for w in s["warnings"]), "projection change warned immediately")
    s, _ = ok_result(c.call("validate"), "validate raised")
    check(s["status"] == "NEEDS_REVIEW" and "edit.projection_deviation" in blocking(s),
          f"raised model is NEEDS_REVIEW ({s['status']} {blocking(s)})")
    glb = os.path.join(out, "A_edit_bad.glb")
    s, _, err = c.call("export_glb", {"path": glb})
    check(err and not os.path.exists(glb), "export refuses a model that no longer matches the drawings")
    ok_result(c.call("undo"), "undo raise")

    # Push/pull a Top face, mirror the leg, duplicate it, all undoable steps.
    s, _ = ok_result(c.call("push_pull", {"view": "top", "u": 9, "v": 15, "distance": -1}), "push_pull")
    check(s["data"]["edit"]["op"] == "sweep", "push/pull recorded as a sweep")
    s, _ = ok_result(c.call("mirror", {"part": leg, "axis": "z", "at": 21, "copy": True}), "mirror copy")
    created = s["data"]["created_parts"]
    check(len(created) == 1, f"mirror copy made a new part {created}")
    s, _ = ok_result(c.call("transform_part", {"part": created[0], "translate": [0, 0, 1.4]}), "transform_part")
    s, _ = ok_result(c.call("rename_part", {"part": created[0], "name": "spare leg"}), "rename_part")
    s, _ = ok_result(c.call("inspect", {"what": "edits"}), "inspect edits")
    n_edits = s["data"]["count"]
    check(n_edits == 4, f"four edits in the history ({n_edits})")
    ok_result(c.call("undo"), "undo rename")
    ok_result(c.call("undo"), "undo transform")
    s, _ = ok_result(c.call("redo"), "redo transform")
    ok_result(c.call("redo"), "redo rename")
    vol = s["stats"]["volume"]

    # Save / reload keeps the edits and replays them to the same geometry.
    p3d = os.path.join(out, "A_edit.p3d")
    ok_result(c.call("save_project", {"path": p3d}), "save_project")
    ok_result(c.call("open_project", {"path": p3d}), "open_project")
    s, _ = ok_result(c.call("project_textures"), "project_textures after reload")
    check(abs(s["stats"]["volume"] - vol) < 1e-6, f"reloaded volume {s['stats']['volume']} == {vol}")
    s, _ = ok_result(c.call("inspect", {"what": "edits"}), "inspect edits after reload")
    check(s["data"]["count"] == n_edits, "edits survived save/reload")
    parts = json.loads(c.request("resources/read", {"uri": "pixel3d://parts"})["result"]["contents"][0]["text"])
    check("spare leg" in json.dumps(parts), "part name survived save/reload")
    s, _ = ok_result(c.call("validate"), "validate edited")
    check(s["status"] != "PASS" or not blocking(s), "PASS never carries blocking conflicts")
    for uri in ("pixel3d://edits", "pixel3d://live", "pixel3d://selection"):
        j = json.loads(c.request("resources/read", {"uri": uri})["result"]["contents"][0]["text"])
        check(isinstance(j, dict), f"resource {uri}")


def run_edit_c(c, fx, out, glbcheck_exe):
    print("editing C through MCP: part masks decide the legs")
    ok_result(c.call("new_project", {"name": "masks_C", "profile": "strict"}), "new_project")
    ok_result(c.call("import_views", views(fx, "C_asym_planter")), "import_views")
    for x_front, z_side in ((1, 11), (8, 1), (15, 11)):
        s, _ = ok_result(c.call("create_part", {"name": "leg", "define": True}), "create_part (define)")
        pid = s["data"]["part"]
        ok_result(c.call("paint_part_mask", {"part": pid, "view": "front", "rect": [x_front, 14, x_front + 2, 22]}),
                  "paint_part_mask front")
        ok_result(c.call("paint_part_mask", {"part": pid, "view": "side", "rect": [z_side, 14, z_side + 2, 22]}),
                  "paint_part_mask side")
    s, _ = ok_result(c.call("validate"), "validate")
    check(s["status"] == "NEEDS_REVIEW" and blocking(s) == ["possible_recess"],
          f"masks resolved the legs, cavity still open: {s['status']} {blocking(s)}")
    s, _ = ok_result(c.call("get_conflicts"), "get_conflicts")
    idx = [i for i, x in enumerate(s["data"]["conflicts"]) if x["kind"] == "possible_recess"][0]
    ok_result(c.call("set_part_depth", {"conflict_index": idx, "depth_from": 11, "label": "cavity"}), "set_part_depth")
    s, _ = ok_result(c.call("validate"), "validate")
    check(s["status"] == "PASS", f"C PASS with mask-defined legs: {s['status']} {blocking(s)}")
    glb = os.path.join(out, "C_masks.glb")
    ok_result(c.call("export_glb", {"path": glb}), "export_glb")
    glbcheck(glbcheck_exe, glb)


def run_single_view(c, fx, out, glbcheck_exe):
    print("single view: one PNG, depth inferred and labelled")
    front = fixture(fx, "B_hand_chair", "front")
    s, _ = ok_result(c.call("new_single_view_project", {"path": front}), "new_single_view_project (default depth)")
    s, _ = ok_result(c.call("validate"), "validate")
    check(s["status"] == "NEEDS_REVIEW" and "single_view_depth" in blocking(s),
          f"default depth is not accepted silently: {s['status']} {blocking(s)}")
    check(s["metrics"]["inferred_volume_fraction"] == 1.0, "all depth is INFERRED")
    s, _ = ok_result(c.call("get_inferred_geometry"), "get_inferred_geometry")
    s, _ = ok_result(c.call("set_single_view_depth", {"preset": "thin"}), "set_single_view_depth thin")
    s, _ = ok_result(c.call("validate"), "validate thin")
    check("single_view_depth" not in blocking(s), f"user-chosen depth accepted: {s['status']} {blocking(s)}")
    kinds = {x["kind"] for x in (s.get("validation") or {}).get("conflicts", [])}
    check("inferred_hidden_geometry" in kinds or s["metrics"]["inferred_volume_fraction"] == 1.0,
          "hidden geometry stays labelled INFERRED")
    vthin = s["stats"]["volume"]
    s, _ = ok_result(c.call("set_single_view_depth", {"depth": 2.5}), "set_single_view_depth 2.5")
    check(s["stats"]["volume"] != vthin, "numeric depth changes the solid")
    s, _, err = c.call("import_views", {"side": fixture(fx, "B_hand_chair", "side")})
    s2, _, err2 = c.call("reconstruct")
    check(err or err2, "single view refuses a second drawing instead of mixing modes")
    ok_result(c.call("undo"), "undo")


def run_style(c, fx, out, glbcheck_exe):
    print("polished style: soft model validated and exported, crisp untouched")
    ok_result(c.call("new_project", {"name": "style_D", "style": "soft"}), "new_project style soft")
    ok_result(c.call("import_views", views(fx, "D_character")), "import_views")
    ok_result(c.call("project_textures"), "project_textures")
    s, _ = ok_result(c.call("render_view", {"views": ["front"]}), "render before validate")
    check(any(m["code"] == "style.not_built" for m in s.get("info", [])),
          "a render before validate says it shows the crisp working model")
    s, _ = ok_result(c.call("validate"), "validate soft")
    check(s["status"] == "PASS", f"soft model PASS ({s['status']} {blocking(s)})")
    rep = s["data"]["report"].get("style", {})
    check(rep.get("style") == "soft" and rep["corners"]["restored_pixels"] == 0,
          f"report describes the polished model: {rep.get('corners')}")
    glb = os.path.join(out, "D_soft.glb")
    s, _ = ok_result(c.call("export_model", {"path": glb}), "export_model soft")
    with open(glb, "rb") as f:
        data = f.read()
    check(b"normalTexture" in data and b"occlusionTexture" in data and b"TEXCOORD_1" in data,
          "soft GLB carries the normal and occlusion maps (UV1)")
    glbcheck(glbcheck_exe, glb)
    s, _ = ok_result(c.call("set_style", {"materials": [{"family": 0, "metallic": 1, "roughness": 0.3}]}),
                     "set_style per-family material")
    s, _ = ok_result(c.call("validate"), "validate after material")
    check(s["status"] == "PASS", "material change keeps PASS")
    s, _, err = c.call("set_style", {"radius": 99})
    check(err and s["errors"][0]["code"] == "input.invalid", "out-of-range tuning refused")
    s, _ = ok_result(c.call("update_settings", {"style": "crisp"}), "back to crisp")
    s, _ = ok_result(c.call("validate"), "validate crisp")
    check(s["data"]["report"].get("style") is None, "crisp builds no polished model")
    ok_result(c.call("undo"), "undo style crisp (validation is not an undo step)")
    s, _ = ok_result(c.call("validate"), "validate after undo")
    check(s["data"]["report"].get("style", {}).get("style") == "soft", "undo restores the soft style")


def run_lowpoly(c, fx, out, glbcheck_exe=None):
    print("low-poly studio: front + side character, suggested parts, build, render, export")
    d = os.path.join(fx, "E_lowpoly_character", "cloaked")
    ok_result(c.call("new_project", {"name": "lowpoly", "mode": "lowpoly_studio"}), "new_project lowpoly_studio")
    ok_result(c.call("import_views", {"front": os.path.join(d, "front.png"), "side": os.path.join(d, "side.png")}),
              "import Front + Side (no Top)")
    s, _, err = c.call("lowpoly_build")
    check(err and s["errors"][0]["code"] == "lowpoly.no_parts", "build without parts is refused")
    s, _ = ok_result(c.call("lowpoly_suggest_parts"), "suggest parts (proposal)")
    check(not s["data"]["applied"] and len(s["data"]["parts"]) >= 3, f"proposal only, {len(s['data']['parts'])} parts")
    s, _ = ok_result(c.call("lowpoly_suggest_parts", {"apply": True}), "apply suggested parts")
    ids = s["data"]["part_ids"]
    s0, _ = ok_result(c.call("inspect", {"what": "lowpoly"}), "inspect lowpoly")
    check(all(p["type"] for p in s0["data"]["parts"]), "every suggested part is typed")
    s, _ = ok_result(c.call("lowpoly_build", {"tri_budget": 800}), "lowpoly_build budget 800")
    lp = s["data"]["lowpoly"]
    check(lp["triangles"] <= 800, f"triangles {lp['triangles']} <= 800 (hard cap)")
    check(lp["glb"]["ok"], "skinned GLB reloads with every attribute")
    check(s["status"] in ("PASS", "NEEDS_REVIEW"), f"status {s['status']}")
    check(s["validation"]["mode"] == "lowpoly_studio", "status comes from the low-poly gate")
    s, imgs = ok_result(c.call("lowpoly_render", {"size": 96, "contact_sheet": True}), "lowpoly_render")
    check(len(imgs) == 5, f"4 yaws + contact sheet inline ({len(imgs)})")
    s, imgs = ok_result(c.call("lowpoly_render", {"size": 96, "pose": True, "yaws": [0, 90]}), "lowpoly_render pose")
    check(len(imgs) == 2, "posed renders")
    s, _, err = c.call("reconstruct")
    check(err and s["errors"][0]["code"] == "state.wrong_mode", "visual hull refused in a Low-Poly Studio project")
    first_type = next(p["type"] for p in s0["data"]["parts"] if p["id"] == ids[0])
    other = "accessory" if first_type != "accessory" else "cloth"
    ok_result(c.call("set_part_type", {"part": ids[0], "type": other}), f"set_part_type {first_type} -> {other}")
    s, _, err = c.call("lowpoly_render")
    check(err and s["errors"][0]["code"] == "state.not_ready", "a type change drops the build")
    ok_result(c.call("undo"), "undo the type change")
    glb = os.path.join(out, "lowpoly_cloaked.glb")
    s, _ = ok_result(c.call("lowpoly_build"), "rebuild")
    if s["status"] == "PASS":
        s, _ = ok_result(c.call("lowpoly_export", {"path": glb, "engine_files": True}), "lowpoly_export")
    else:
        s, _, err = c.call("lowpoly_export", {"path": glb})
        check(err and s["errors"][0]["code"] == "lowpoly.export_refused", "NEEDS_REVIEW refused without acceptance")
        s, _ = ok_result(c.call("lowpoly_export", {"path": glb, "accept_status": "NEEDS_REVIEW", "engine_files": True}),
                         "lowpoly_export (accepted)")
    check(os.path.exists(glb) and os.path.exists(os.path.join(out, "lowpoly_cloaked_toon.gdshader")),
          "GLB and Godot shaders written")
    with open(glb, "rb") as f:
        data = f.read()
    check(data[:4] == b"glTF" and b"JOINTS_0" in data and b"WEIGHTS_0" in data, "skinned glTF binary")
    lowpoly_glbcheck(glbcheck_exe, glb, out)
    ok_result(c.call("save_project", {"path": os.path.join(out, "lowpoly.p3d")}), "save lowpoly project")
    ok_result(c.call("open_project", {"path": os.path.join(out, "lowpoly.p3d")}), "reopen")
    s, _ = ok_result(c.call("inspect", {"what": "project"}), "inspect reopened")
    check(s["data"]["lowpoly_studio"] is True, "mode survives save/load")


def run_layered(c, fx, out):
    print("layered inputs: Aseprite Front + PSD Side, layers as suggested parts; Front only with inferred depth")
    g = os.path.join(fx, "G_layered")
    ok_result(c.call("new_project", {"name": "layered", "mode": "lowpoly_studio"}), "new_project lowpoly_studio")
    ok_result(c.call("import_views", {"front": os.path.join(g, "rgb_layers.aseprite"),
                                      "side": os.path.join(g, "krita_layers.psd")}), "import .aseprite + .psd")
    s, _ = ok_result(c.call("suggest_layer_parts"), "layer parts (proposal)")
    layers = {l["name"]: l for l in s["data"]["layers"]}
    check(not s["data"]["applied"] and set(layers) == {"body", "head", "hat_hidden"}, f"layers {sorted(layers)}")
    check(layers["hat_hidden"]["suggested"] is False, "the hidden layer is listed, not suggested")
    s, _ = ok_result(c.call("suggest_layer_parts", {"apply": True, "accept": ["body", "head"]}), "accept body + head")
    check(len(s["data"]["part_ids"]) == 2, "two parts created")
    s, _ = ok_result(c.call("inspect", {"what": "lowpoly"}), "inspect lowpoly")
    check({p["name"]: p["type"] for p in s["data"]["parts"]} == {"body": "torso", "head": "head"}, "types from the names")
    s, _ = ok_result(c.call("lowpoly_build", {"tri_budget": 500}), "build from layer parts")
    check(s["data"]["lowpoly"]["glb"]["ok"], "skinned GLB reloads")
    # Front only: automatic depth, NEEDS_REVIEW, export only with the user's acceptance.
    ok_result(c.call("new_project", {"name": "front_only", "mode": "lowpoly_studio"}), "new_project front only")
    ok_result(c.call("import_views", {"front": os.path.join(g, "rgb_layers.aseprite")}), "import Front only")
    ok_result(c.call("suggest_layer_parts", {"apply": True}), "accept every layer")
    s, _ = ok_result(c.call("lowpoly_build", {"tri_budget": 500}), "front-only build")
    check(s["status"] == "NEEDS_REVIEW", f"front only stays NEEDS_REVIEW ({s['status']})")
    glb = os.path.join(out, "front_only.glb")
    s, _, err = c.call("lowpoly_export", {"path": glb})
    check(err, "front-only export refused without accept_inferred_depth")
    ok_result(c.call("lowpoly_build", {"accept_inferred_depth": True}), "user accepts inferred depth")
    ok_result(c.call("lowpoly_export", {"path": glb}), "front-only export after acceptance")
    check(os.path.exists(glb), "front-only GLB written")


def cancellation_check(c, fx):
    print("cancellation")
    ok_result(c.call("new_project", {"name": "cancel"}), "new_project")
    ok_result(c.call("import_views", views(fx, "C_asym_planter")), "import_views")
    slow, victim, after = c.next_id, c.next_id + 1, c.next_id + 2
    c.next_id += 3
    c.send({"jsonrpc": "2.0", "id": slow, "method": "tools/call", "params": {"name": "validate", "arguments": {}}})
    c.send({"jsonrpc": "2.0", "id": victim, "method": "ping"})
    c.send({"jsonrpc": "2.0", "method": "notifications/cancelled", "params": {"requestId": victim, "reason": "test"}})
    c.send({"jsonrpc": "2.0", "id": after, "method": "ping"})
    seen = []
    while after not in seen:
        m = c.wait_for(None)
        seen.append(m.get("id"))
    check(slow in seen and victim not in seen, f"cancelled queued request is not answered (ids seen {seen})")


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    exe, fx, out = (os.path.abspath(a) for a in sys.argv[1:4])
    glbcheck_exe = os.path.abspath(sys.argv[4]) if len(sys.argv) > 4 else None
    os.makedirs(out, exist_ok=True)
    t0 = time.time()
    c = McpClient(exe, os.path.join(out, "server_stderr.log"))
    try:
        protocol_checks(c)
        run_a(c, fx, out, glbcheck_exe)
        run_b(c, fx, out, glbcheck_exe)
        run_c(c, fx, out, glbcheck_exe)
        run_edit_a(c, fx, out, glbcheck_exe)
        run_edit_c(c, fx, out, glbcheck_exe)
        run_single_view(c, fx, out, glbcheck_exe)
        run_style(c, fx, out, glbcheck_exe)
        run_lowpoly(c, fx, out, glbcheck_exe)
        run_layered(c, fx, out)
        cancellation_check(c, fx)
    except Exception as e:  # noqa: BLE001 - report and fail
        import traceback
        traceback.print_exc()
        failures.append(f"exception: {e}")
    code = c.close()
    check(code == 0, "server exits cleanly when stdin closes")
    check(not c.bad_lines, "stdout carried only JSON-RPC messages")
    with open(os.path.join(out, "server_stderr.log"), "rb") as f:
        log = f.read().decode("utf-8", "replace")
    check("tools/call" in log, "server logs went to stderr")
    print(f"\n{len(failures)} failure(s) in {time.time() - t0:.1f} s")
    for f in failures:
        print("  -", f)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
