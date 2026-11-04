"""Builds 3re's own Q4_0 weights from the original checkpoints and compares them with the published ilintar files.

Sources at fixed revisions, each checked against its Hugging Face LFS SHA-256; tools/weights/convert.py and
quantize_gguf.py are trellis.cpp's own, unchanged. The study and its acceptance criteria are in
docs/research/own-gguf-weights.md. Writes <work>/out/*.gguf and <work>/report.json.
usage: python tools/weights/build_q4.py <work folder>
"""
import hashlib
import json
import os
import subprocess
import sys
import urllib.request

import gguf

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.path.abspath(sys.argv[1])
SRC = os.path.join(WORK, "src")
GGUF = os.path.join(WORK, "f16")
OUT = os.path.join(WORK, "out")
REF = os.path.join(WORK, "ref")
SETS = [
    ("microsoft/TRELLIS.2-4B", "af44b45f2e35a493886929c6d786e563ec68364d", "ckpts", "", [
        "ss_flow_img_dit_1_3B_64_bf16", "slat_flow_img2shape_dit_1_3B_512_bf16", "slat_flow_imgshape2tex_dit_1_3B_512_bf16",
        "shape_dec_next_dc_f16c32_fp16", "tex_dec_next_dc_f16c32_fp16"]),
    ("microsoft/TRELLIS-image-large", "25e0d31ffbebe4b5a97464dd851910efc3002d96", "ckpts", "tilarge", ["ss_dec_conv3d_16l8_fp16"]),
    ("timm/vit_large_patch16_dinov3.lvd1689m", "30c1109559f65dea34316b0d4842d35c5771fe11", "", "dinov3", ["model", "config", "LICENSE"]),
]
REFERENCE = ("ilintar/trellis2-gguf", "a57397bd3d351599d9729fc144b3f87c3f87d65b", "q4")
COMPONENTS = ["dinov3", "ss_flow", "ss_dec", "shape_flow_512", "shape_dec", "tex_flow_512", "tex_dec"]


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 22), b""):
            h.update(block)
    return h.hexdigest()


def fetch(url, target, size, oid):
    os.makedirs(os.path.dirname(target), exist_ok=True)
    subprocess.run(["curl", "-fsSL", "--retry", "5", "-o", target, url], check=True)
    if os.path.getsize(target) != size or (oid and sha(target) != oid):
        raise SystemExit(f"bad download: {url}")
    print("OK ", os.path.basename(target), size, flush=True)


def tree(repo, rev, sub):
    url = f"https://huggingface.co/api/models/{repo}/tree/{rev}" + (f"/{sub}" if sub else "")
    with urllib.request.urlopen(url, timeout=60) as r:
        return json.load(r)


def sources():
    pins = []
    for repo, rev, sub, dest, stems in SETS:
        for e in tree(repo, rev, sub):
            name = os.path.basename(e["path"])
            stem, ext = os.path.splitext(name)
            if stem in stems and ext in (".safetensors", ".json", ".md", ""):
                oid = (e.get("lfs") or {}).get("oid")
                fetch(f"https://huggingface.co/{repo}/resolve/{rev}/{e['path']}", os.path.join(SRC, dest, sub, name), e["size"], oid)
                pins.append({"repo": repo, "revision": rev, "path": e["path"], "size": e["size"], "sha256": oid})
    return pins


def references():
    repo, rev, sub = REFERENCE
    for e in tree(repo, rev, sub):
        if os.path.basename(e["path"]) in [c + ".gguf" for c in COMPONENTS]:
            fetch(f"https://huggingface.co/{repo}/resolve/{rev}/{e['path']}", os.path.join(REF, os.path.basename(e["path"])),
                  e["size"], (e.get("lfs") or {}).get("oid"))


def fields(reader):
    skip = {"GGUF.version", "GGUF.tensor_count", "GGUF.kv_count"}
    return {k: repr(f.contents()) for k, f in reader.fields.items() if k not in skip}


def compare(name):
    ours, ref = os.path.join(OUT, name), os.path.join(REF, name)
    row = {"file": name, "size": os.path.getsize(ours), "sha256": sha(ours), "reference_size": os.path.getsize(ref),
           "reference_sha256": sha(ref)}
    row["identical"] = row["sha256"] == row["reference_sha256"]
    a, b = gguf.GGUFReader(ours), gguf.GGUFReader(ref)
    fa, fb = fields(a), fields(b)
    row["metadata_differs"] = sorted(k for k in set(fa) | set(fb) if fa.get(k) != fb.get(k))
    ta, tb = {t.name: t for t in a.tensors}, {t.name: t for t in b.tensors}
    row["tensors"], row["reference_tensors"] = len(ta), len(tb)
    row["only_ours"], row["only_reference"] = sorted(set(ta) - set(tb))[:20], sorted(set(tb) - set(ta))[:20]
    row["type_differs"], row["data_differs"] = [], []
    for n in sorted(set(ta) & set(tb)):
        if ta[n].tensor_type != tb[n].tensor_type or list(ta[n].shape) != list(tb[n].shape):
            row["type_differs"].append(f"{n}: {ta[n].tensor_type.name}{list(ta[n].shape)} / {tb[n].tensor_type.name}{list(tb[n].shape)}")
        elif ta[n].data.tobytes() != tb[n].data.tobytes():
            row["data_differs"].append(n)
    row["type_differs_count"], row["data_differs_count"] = len(row["type_differs"]), len(row["data_differs"])
    row["type_differs"], row["data_differs"] = row["type_differs"][:20], row["data_differs"][:20]
    return row


def main():
    pins = sources()
    references()
    env = dict(os.environ, TRELLIS_MODELS=SRC, TRELLIS_GGUF_OUT=GGUF)
    os.makedirs(GGUF, exist_ok=True)
    os.makedirs(OUT, exist_ok=True)
    rows = []
    for c in COMPONENTS:
        subprocess.run([sys.executable, os.path.join(HERE, "convert.py"), c], env=env, check=True)
        f16, name = os.path.join(GGUF, c + ".gguf"), c + ".gguf"
        subprocess.run([sys.executable, os.path.join(HERE, "quantize_gguf.py"), f16, os.path.join(OUT, name), "Q4_0"], check=True)
        os.remove(f16)
        rows.append(compare(name))
        print(json.dumps({k: v for k, v in rows[-1].items() if not k.startswith("only")}), flush=True)
    report = {"gguf_python": subprocess.run([sys.executable, "-m", "pip", "show", "gguf"], capture_output=True, text=True).stdout.split("\n")[1],
              "sources": pins, "files": rows, "all_identical": all(r["identical"] for r in rows)}
    with open(os.path.join(WORK, "report.json"), "w", encoding="utf-8") as f:
        json.dump(report, f, indent=1)
    print("ALL IDENTICAL" if report["all_identical"] else "NOT ALL IDENTICAL: read report.json", flush=True)


main()
