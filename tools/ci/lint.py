"""Lint for the files that are published (tools/oss/public_tree.py): relative links in Markdown resolve, JSON
parses, Python compiles, VERSION is a version. Exit 1 with one line per problem.
usage: python -I tools/ci/lint.py
"""
import json
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "oss"))
import public_tree  # noqa: E402

ROOT = public_tree.ROOT
LINK = re.compile(r"\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)|(?:src|href)=\"([^\"]+)\"")


def check_links(path, text, files, problems):
    for m in LINK.finditer(text):
        target = m.group(1) or m.group(2)
        if re.match(r"^[a-z][a-z0-9+.-]*:", target) or target.startswith("#"):
            continue
        target = target.split("#", 1)[0].split("?", 1)[0]
        if not target:
            continue
        resolved = os.path.normpath(os.path.join(os.path.dirname(path), target)).replace("\\", "/")
        if resolved.startswith(".."):
            continue  # a GitHub route such as ../../releases/latest
        if resolved not in files and not any(f.startswith(resolved.rstrip("/") + "/") for f in files):
            problems.append(f"{path}: link to {target} does not resolve in the public tree")


def main():
    files = public_tree.public_files()
    fileset = set(files)
    problems = []
    for p in files:
        full = os.path.join(ROOT, p)
        if p.startswith("third_party/") or not os.path.isfile(full):
            continue
        ext = os.path.splitext(p)[1].lower()
        if ext == ".md":
            check_links(p, open(full, encoding="utf-8", errors="replace").read(), fileset, problems)
        elif ext == ".json":
            try:
                json.load(open(full, encoding="utf-8"))
            except ValueError as e:
                problems.append(f"{p}: invalid JSON ({e})")
        elif ext == ".py":
            try:
                compile(open(full, encoding="utf-8").read(), p, "exec")
            except SyntaxError as e:
                problems.append(f"{p}: Python syntax error ({e})")
    version = open(os.path.join(ROOT, "VERSION"), encoding="utf-8").read().strip()
    if not re.fullmatch(r"\d+\.\d+\.\d+(-[0-9A-Za-z.]+)?", version):
        problems.append(f"VERSION: not a version: {version!r}")
    for line in problems:
        print(line)
    print(f"lint: {len(files)} public files, {len(problems)} problems")
    sys.exit(1 if problems else 0)


main()
