"""Packs the maintainer-built binaries that the release workflow cannot build yet into one pinned zip.

  python -I tools/release/pack_build_inputs.py OUT_DIR

Reads the verified stores in .toolchain (tools/stores.ps1) and writes OUT_DIR/3re-build-inputs-<N>.zip and
tools/release/build-inputs.json (zip size and SHA-256, every file's SHA-256). The maintainer uploads the zip as an
asset of the release "build-inputs-<N>"; the release workflow downloads it and checks every pin.

Contents (all redistributable; sources in brackets):
  engine/trellis-cli.exe, BUILD.txt, texture-candidates.patch  3re's trellis.cpp fork (MIT) [tools/trellis/engine-patch]
  engine/pixel3d-trellis-post-safe.exe                         verified cleanup build of tools/trellis/post_geometry_safe.cpp
  engine/libgcc_s_seh-1.dll, libstdc++-6.dll, libwinpthread-1.dll   MinGW runtime of the engine (GCC runtime exception)
  msvc/*.dll                                                   Microsoft Visual C++ runtime 14.51 (app-local)
  quadwild/**                                                  QuadWild-BiMDF runtime, licence and corresponding source (GPL-3.0)
"""
import hashlib
import json
import os
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TC = os.path.realpath(os.path.join(ROOT, ".toolchain"))
VERSION = 2

SOURCES = [
    ("engine/trellis-cli.exe", "trellis-fork/tex-candidates-4e19aa96/trellis-cli.exe"),
    ("engine/BUILD.txt", "trellis-fork/tex-candidates-4e19aa96/BUILD.txt"),
    ("engine/texture-candidates.patch", "trellis-fork/tex-candidates-4e19aa96/texture-candidates.patch"),
    ("engine/pixel3d-trellis-post-safe.exe", "trellis-post/safe-415020aa/pixel3d-trellis-post-safe.exe"),
    ("engine/libgcc_s_seh-1.dll", "trellis/runtime/libgcc_s_seh-1.dll"),
    ("engine/libstdc++-6.dll", "trellis/runtime/libstdc++-6.dll"),
    ("engine/libwinpthread-1.dll", "trellis/runtime/libwinpthread-1.dll"),
]
PINNED = {  # must match tools/stores.ps1 / tools/package.ps1
    "engine/trellis-cli.exe": "4e19aa96208ee56cebdc647aa34c605708c369d5eae553ca71dff3fc1a00e3ba",
    "engine/pixel3d-trellis-post-safe.exe": "415020aaf90c6930d3f5e10916813a68381f2e043afd8be82c2cae97b51eb307",
}


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def main():
    out_dir = sys.argv[1]
    entries = list(SOURCES)
    for name in sorted(os.listdir(os.path.join(TC, "msvc-runtime", "14.51.36247"))):
        entries.append(("msvc/" + name, "msvc-runtime/14.51.36247/" + name))
    qw = os.path.join(TC, "quadwild-bimdf", "3re-v5")
    for dirpath, _, names in os.walk(qw):
        for n in sorted(names):
            rel = os.path.relpath(os.path.join(dirpath, n), qw).replace("\\", "/")
            entries.append(("quadwild/" + rel, "quadwild-bimdf/3re-v5/" + rel))
    files = []
    for arc, src in entries:
        full = os.path.join(TC, src)
        digest = sha(full)
        if arc in PINNED and digest != PINNED[arc]:
            sys.exit(f"{src} does not match its pin ({digest})")
        files.append({"path": arc, "size": os.path.getsize(full), "sha256": digest, "_src": full})
    os.makedirs(out_dir, exist_ok=True)
    name = f"3re-build-inputs-{VERSION}.zip"
    zpath = os.path.join(out_dir, name)
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for f in files:
            z.write(f["_src"], f["path"])
    manifest = {
        "format": "3re-build-inputs", "version": VERSION, "release_tag": f"build-inputs-{VERSION}", "asset": name,
        "size": os.path.getsize(zpath), "sha256": sha(zpath),
        "files": [{k: v for k, v in f.items() if k != "_src"} for f in files],
    }
    with open(os.path.join(ROOT, "tools", "release", "build-inputs.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, indent=1)
        f.write("\n")
    print(f"{zpath}: {len(files)} files, {manifest['size']} bytes, sha256 {manifest['sha256']}")


main()
