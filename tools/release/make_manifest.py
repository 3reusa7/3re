"""Writes update-manifest.json for a staged release (signed afterwards by minisign.mjs).

  python -I tools/release/make_manifest.py --version V --stage DIR --package ZIP --url URL \
      --notes-en EN.txt --notes-ar AR.txt [--commit SHA] [--installer EXE --installer-url URL] --out update-manifest.json

Every file of the staged tree is listed with its size and SHA-256: the updater accepts the unpacked package only
when it matches this list exactly (apps/update/update_service.cpp, verifyTree).
"""
import argparse
import datetime
import hashlib
import json
import os


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def main():
    a = argparse.ArgumentParser()
    for opt in ("--version", "--stage", "--package", "--url", "--notes-en", "--notes-ar", "--out"):
        a.add_argument(opt, required=True)
    a.add_argument("--commit", default="")
    a.add_argument("--installer", default="")
    a.add_argument("--installer-url", default="")
    args = a.parse_args()
    files = []
    for dirpath, _, names in os.walk(args.stage):
        for n in names:
            full = os.path.join(dirpath, n)
            files.append({"path": os.path.relpath(full, args.stage).replace("\\", "/"), "size": os.path.getsize(full), "sha256": sha(full)})
    files.sort(key=lambda f: f["path"])
    packages = [{"platform": "windows-x64", "kind": "portable-zip", "name": os.path.basename(args.package), "url": args.url,
                 "size": os.path.getsize(args.package), "sha256": sha(args.package), "files": files}]
    if args.installer:
        packages.append({"platform": "windows-x64", "kind": "installer", "name": os.path.basename(args.installer),
                         "url": args.installer_url, "size": os.path.getsize(args.installer), "sha256": sha(args.installer)})
    manifest = {
        "format": "3re-update-manifest", "format_version": 1, "product": "3re",
        "version": args.version, "channel": "alpha" if "-" in args.version else "stable",
        "published": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"), "commit": args.commit,
        "notes": {"en": open(args.notes_en, encoding="utf-8").read().strip(), "ar": open(args.notes_ar, encoding="utf-8").read().strip()},
        "packages": packages,
    }
    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, indent=1, ensure_ascii=False)
        f.write("\n")
    print(f"{args.out}: {args.version}, {len(files)} files")


main()
