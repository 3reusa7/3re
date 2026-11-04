"""Zips a staged release folder: entries relative to the folder, '/' separators, sorted, fixed timestamps (the
updater refuses backslash paths, and a sorted, dated-alike zip is easier to compare between builds).
usage: python -I tools/release/zip_tree.py DIR OUT.zip
"""
import os
import sys
import zipfile

src, out = sys.argv[1], sys.argv[2]
paths = []
for dirpath, _, names in os.walk(src):
    for n in names:
        full = os.path.join(dirpath, n)
        paths.append((os.path.relpath(full, src).replace("\\", "/"), full))
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for rel, full in sorted(paths):
        info = zipfile.ZipInfo(rel, date_time=(2026, 1, 1, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        info.external_attr = 0o644 << 16
        with open(full, "rb") as f:
            z.writestr(info, f.read(), compresslevel=9)
print(f"{out}: {len(paths)} files")
