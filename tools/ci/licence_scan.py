"""Licence scan: every vendored library is listed with an allowed licence and its licence text, the notices point
at files that exist, every downloaded model names its licence and checksum, and the published tree is clean
(tools/oss/public_tree.py scan: no weights, binaries, secrets, personal paths or third-party pictures).
usage: python -I tools/ci/licence_scan.py
"""
import json
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "oss"))
import public_tree  # noqa: E402

ROOT = public_tree.ROOT
ALLOWED = ["MIT", "BSD-2-Clause", "BSD-3-Clause", "Apache-2.0", "BSL-1.0", "CC0-1.0", "public domain", "Zlib"]
# third_party/<dir> -> how the README table and the notices name it
NAMES = {"nlohmann": "nlohmann/json", "stb": "stb_image", "Clipper2": "Clipper2", "manifold": "Manifold", "miniz": "miniz",
         "tinygltf": "tinygltf", "doctest": "doctest", "meshoptimizer": "meshoptimizer", "libwebp": "libwebp",
         "xatlas": "xatlas", "monocypher": "Monocypher"}
NOT_DISTRIBUTED = {"doctest"}  # build/test only
ADDED_BY_PACKAGING = {"LICENSES/gltf-validator-Apache-2.0.txt", "LICENSES/gltf-validator-NOTICES.txt",
                      "LICENSES/onnxruntime-ThirdPartyNotices.txt", "LICENSES/QuadWild-GPL-3.0.txt",
                      "LICENSES/Qt/qtbase-6.8.3.spdx", "LICENSES/Qt/qtsvg-6.8.3.spdx"}


def read(rel):
    return open(os.path.join(ROOT, rel), encoding="utf-8").read()


def main():
    problems = []
    readme, notices = read("third_party/README.md"), read("THIRD-PARTY-NOTICES.txt")
    rows = {}
    for line in readme.splitlines():
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) >= 3 and cells[0] not in ("Library", "---"):
            rows[cells[0]] = cells[2]
    for d in sorted(os.listdir(os.path.join(ROOT, "third_party"))):
        if not os.path.isdir(os.path.join(ROOT, "third_party", d)):
            continue
        name = NAMES.get(d)
        if not name:
            problems.append(f"third_party/{d}: unknown library (add it to tools/ci/licence_scan.py, third_party/README.md and the notices)")
            continue
        row = next((v for k, v in rows.items() if k.lower().startswith(name.lower().split("/")[0])), None)
        if row is None:
            problems.append(f"third_party/{d}: not listed in third_party/README.md")
        elif not any(a.lower() in row.lower() for a in ALLOWED):
            problems.append(f"third_party/{d}: licence '{row}' is not in the allowed list {ALLOWED}")
        if d not in NOT_DISTRIBUTED and name.split("/")[0].lower() not in notices.lower():
            problems.append(f"third_party/{d}: missing from THIRD-PARTY-NOTICES.txt")
    for ref in sorted(set(re.findall(r"LICENSES/[A-Za-z0-9_./-]+\.(?:txt|spdx)", notices))):
        if ref not in ADDED_BY_PACKAGING and not os.path.isfile(os.path.join(ROOT, ref)):
            problems.append(f"THIRD-PARTY-NOTICES.txt names {ref}, which does not exist")
    if "Apache License" not in read("LICENSE") or "Version 2.0" not in read("LICENSE"):
        problems.append("LICENSE is not the Apache License 2.0")
    if not os.path.isfile(os.path.join(ROOT, "NOTICE")):
        problems.append("NOTICE is missing")
    catalog = json.loads(read("apps/update/models.json"))
    for p in catalog["packs"]:
        if not p.get("licence") or not p.get("licence_urls"):
            problems.append(f"models.json pack {p['id']}: no licence or licence link")
        for f in p["files"]:
            if len(f.get("sha256", "")) != 64 or f.get("size", 0) <= 0:
                problems.append(f"models.json pack {p['id']}: {f['path']} has no size or SHA-256")
        if "archive" in p and len(p["archive"].get("sha256", "")) != 64:
            problems.append(f"models.json pack {p['id']}: archive has no SHA-256")
    findings = public_tree.scan(public_tree.public_files())
    problems += [f"{p}: {why}" for p, why in findings]
    for line in problems:
        print(line)
    print(f"licence scan: {len(problems)} problems")
    sys.exit(1 if problems else 0)


main()
