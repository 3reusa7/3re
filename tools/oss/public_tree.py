"""What the public 3re repository contains, and a scan that proves it is clean.

The private repository keeps the full history and the evaluation material (reports with third-party pictures, eval
specs, maintainer paths). The public repository is an export of the tracked files that pass EXCLUDE below.

  python -I tools/oss/public_tree.py list            # the files that would be published
  python -I tools/oss/public_tree.py scan            # scan them; exit 1 on any finding
  python -I tools/oss/public_tree.py export DIR      # copy them to DIR (an empty folder)

The scan fails on: secrets (token and key formats, credential URLs), personal paths (a real user profile path),
names on the private list, model weights,
executables, archives, files over 5 MB, and pictures outside the folders whose provenance is recorded.
"""
import fnmatch
import io
import os
import re
import shutil
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# Tracked paths that stay private (glob, matched against the '/'-separated path).
EXCLUDE = [
    "docs/reports/**",            # evaluation outputs: third-party pictures and renders, maintainer paths
    "docs/lowpoly_review/**",     # review renders
    "docs/handoff/**",            # session hand-over notes
    "docs/integration-notes.md",
    "docs/superpowers/**",        # internal design specs and plans (maintainer paths)
    "docs/agent/demo/**", "docs/agent/demo-m4/**", "docs/agent/chat-eval/**",
    "docs/agent/specs/**", "docs/agent/specs-m2/**", "docs/agent/specs-m4-heldout/**",
    "docs/agent/AUDIT.md", "docs/agent/PLAN-M3.md", "docs/agent/W2-HOODED-BACK.md", "docs/agent/TEXTURE-DIAGNOSIS.md",
    "docs/oss/**",                # the audit and the private name list
    "CLAUDE.md",                  # internal working rules and dates
    "tools/agent/**",             # evaluation scripts for the private set
    "tools/geometry_eval/**", "tools/p2/**", "tools/multiangle/**", "tools/multiview/**",
    "tools/archive_old_builds.ps1",
    "docs/ux/**",                 # UX review screenshots of the Pixel3D era
]

# Folders whose pictures have a recorded provenance (all made by the project or synthetic).
PICTURE_FOLDERS = ["tests/fixtures/", "branding/", "docs/images/", "apps/gui/resources/"]
PICTURE_EXT = {".png", ".jpg", ".jpeg", ".webp", ".gif", ".bmp", ".tga", ".psd", ".aseprite", ".ico", ".svg"}
BLOCKED_EXT = {".gguf", ".onnx", ".safetensors", ".pt", ".pth", ".ckpt", ".bin", ".npz", ".h5", ".tflite",
               ".exe", ".dll", ".so", ".dylib", ".lib", ".a", ".o", ".obj", ".pdb", ".zip", ".7z", ".tar", ".gz", ".rar"}
MAX_BYTES = 5 * 1024 * 1024

SECRETS = [
    re.compile(rb"\bgh[pousr]_[A-Za-z0-9]{30,}"), re.compile(rb"\bgithub_pat_[A-Za-z0-9_]{40,}"),
    re.compile(rb"\b(AKIA|ASIA)[0-9A-Z]{16}\b"), re.compile(rb"-----BEGIN (RSA |EC |OPENSSH |DSA |PGP |ENCRYPTED )?PRIVATE KEY-----"),
    re.compile(rb"\bhf_[A-Za-z0-9]{30,}"), re.compile(rb"\bsk-(ant-|proj-)?[A-Za-z0-9_-]{32,}"),
    re.compile(rb"\bAIza[0-9A-Za-z_-]{35}\b"), re.compile(rb"\bxox[baprs]-[0-9A-Za-z-]{10,}"),
    re.compile(rb"3RE-MINISIGN-SECRET-V1:[A-Za-z0-9+/=]{20,}"),
    re.compile(rb"\b[a-z][a-z0-9+.-]*://[^/\s:@'\"<>]{1,64}:[^/\s@'\"<>*]{6,}@[A-Za-z0-9.-]+"),
]
# A real user profile in a path (placeholders such as C:\Users\x\ or C:/Users/me/ in tests and docs are fine).
PERSONAL = re.compile(rb"(?i)(c:[\/]{1,2}users[\/]{1,2}(?!x[\/]|me[\/]|user[\/]|public[\/]|<|%|\$)[a-z0-9._-]{2,}|/c/users/[a-z0-9._-]{2,})")
# Names that must never be published live in a private file, which is itself not exported.
NAMES_FILE = os.path.join(ROOT, "docs", "oss", "private-names.txt")
PRIVATE_NAMES = None
if os.path.isfile(NAMES_FILE):
    words = [w.strip() for w in open(NAMES_FILE, encoding="utf-8") if w.strip()]
    PRIVATE_NAMES = re.compile(("(?i)" + "|".join(words)).encode())
NAMING_ALLOWED = set()


def tracked():
    out = subprocess.run(["git", "-C", ROOT, "ls-files", "-z"], capture_output=True, check=True).stdout
    return [p for p in out.decode("utf-8").split("\0") if p]


def public_files():
    return [p for p in tracked() if not any(fnmatch.fnmatch(p, g) for g in EXCLUDE)]


def scan(paths):
    findings = []
    for p in paths:
        full = os.path.join(ROOT, p)
        if not os.path.isfile(full):
            continue
        ext = os.path.splitext(p)[1].lower()
        size = os.path.getsize(full)
        if ext in BLOCKED_EXT:
            findings.append((p, "blocked file type (weights, binaries, archives never go in the repository)"))
        if size > MAX_BYTES:
            findings.append((p, f"larger than 5 MB ({size} bytes)"))
        if ext in PICTURE_EXT and not any(p.startswith(f) for f in PICTURE_FOLDERS):
            findings.append((p, "picture outside the folders with recorded provenance"))
        with open(full, "rb") as f:
            data = f.read()
        if data[:2] == b"PK":  # zip containers (.p3d): search their members too
            with zipfile.ZipFile(io.BytesIO(data)) as z:
                data += b"".join(z.read(n) for n in z.namelist())
        for rx in SECRETS:
            if rx.search(data):
                findings.append((p, "looks like a secret or a credential URL"))
        if PERSONAL.search(data) and not p.startswith("third_party/"):  # upstream examples are not ours
            findings.append((p, "personal path"))
        if p not in NAMING_ALLOWED:
            if PRIVATE_NAMES and PRIVATE_NAMES.search(data):
                findings.append((p, "contains a name from the private list"))
    return findings


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "scan"
    files = public_files()
    if cmd == "list":
        print("\n".join(files))
    elif cmd == "scan":
        findings = scan(files)
        for p, why in findings:
            print(f"{p}: {why}")
        print(f"public tree: {len(files)} files, {len(findings)} findings")
        sys.exit(1 if findings else 0)
    elif cmd == "export" and len(sys.argv) == 3:
        dst = os.path.abspath(sys.argv[2])
        if os.path.exists(dst) and os.listdir(dst):
            sys.exit(f"{dst} is not empty")
        for p in files:
            os.makedirs(os.path.dirname(os.path.join(dst, p)) or dst, exist_ok=True)
            shutil.copy2(os.path.join(ROOT, p), os.path.join(dst, p))
        print(f"exported {len(files)} files to {dst}")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
