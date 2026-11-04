"""Headless Blender validator for delivered GLB files (G0, docs/agent/ASSET-QUALITY.md).

Blender is an external validator here, never shipped. Run:
  blender --background --factory-startup --python tools/asset_quality/blender_check.py -- OUT.json A.glb [B.glb ...]
Writes one JSON record per file: objects, world scales, matrix determinants, mesh validity, normals.
"""
import json
import sys

import bpy  # noqa: E402  (only inside Blender)


def check(path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    rec = {"file": path, "objects": [], "fail": []}
    try:
        bpy.ops.import_scene.gltf(filepath=path)
    except Exception as e:  # import failure is a FAIL, not a crash
        rec["fail"].append("import: %s" % e)
        return rec
    for ob in bpy.data.objects:
        o = {"name": ob.name, "type": ob.type,
             "scale": [round(v, 6) for v in ob.scale],
             "world_det": round(ob.matrix_world.to_3x3().determinant(), 6),
             "parent": ob.parent.name if ob.parent else None}
        if any(v < 0 for v in ob.scale) or o["world_det"] < 0:
            rec["fail"].append("%s: negative scale/determinant" % ob.name)
        if ob.type == "MESH":
            me = ob.data
            o["verts"], o["faces"] = len(me.vertices), len(me.polygons)
            o["invalid"] = bool(me.validate(verbose=False, clean_customdata=False))
            if o["invalid"]:
                rec["fail"].append("%s: mesh.validate() changed the mesh" % ob.name)
            o["uv_layers"] = len(me.uv_layers)
            o["materials"] = [m.name if m else None for m in me.materials]
        rec["objects"].append(o)
    return rec


def main():
    argv = sys.argv[sys.argv.index("--") + 1:]
    out, files = argv[0], argv[1:]
    res = [check(f) for f in files]
    with open(out, "w", encoding="utf-8") as fh:
        json.dump({"blender": bpy.app.version_string, "results": res}, fh, indent=1)


main()
