"""Release texts from CHANGELOG.md and docs/release-notes/<version>.ar.md.

  python -I tools/release/release_notes.py VERSION body OUT.md    # GitHub Release body (EN + Arabic summary)
  python -I tools/release/release_notes.py VERSION en OUT.txt     # plain-words notes for the update manifest
  python -I tools/release/release_notes.py VERSION ar OUT.txt     # Arabic notes for the update manifest

A CHANGELOG section starts with "## <version>" (anything may follow on that line) and ends at the next "## ".
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def base(version):
    return re.sub(r"\.dryrun\.\d+$", "", version)


def find(version):
    text = open(os.path.join(ROOT, "CHANGELOG.md"), encoding="utf-8").read()
    return re.search(rf"^## {re.escape(version)}(?=[\s—-]|$).*?$(.*?)(?=^## |\Z)", text, re.M | re.S)


def section(version):
    # A dry run (2.10.0-alpha.dryrun.1) uses the section of the version it rehearses.
    m = find(base(version))
    if not m:
        sys.exit(f"CHANGELOG.md has no section for {version}")
    title = m.group(0).splitlines()[0][3:].strip()
    return title, m.group(1).strip()


def plain(md):
    """Markdown bullets to plain words: no emphasis marks, no code ticks, no links' URLs."""
    out = []
    for line in md.splitlines():
        line = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", line)
        line = line.replace("**", "").replace("`", "")
        line = re.sub(r"^\s*[-*]\s+", "• ", line)
        out.append(line.rstrip())
    return "\n".join(out).strip()


def arabic(version):
    path = os.path.join(ROOT, "docs", "release-notes", f"{base(version)}.ar.md")
    return open(path, encoding="utf-8").read().strip() if os.path.isfile(path) else ""


def main():
    version, kind, out = sys.argv[1], sys.argv[2], sys.argv[3]
    title, body = section(version)
    ar = arabic(version)
    if kind == "en":
        text = plain(body)
    elif kind == "ar":
        text = ar or "ملخص هذا الإصدار بالعربية غير متوفر بعد؛ التفاصيل بالإنجليزية أعلاه."
    elif kind == "body":
        text = "\n".join([
            f"## {title}", "", body, "",
            "### بالعربية", "", ar or "_ملخص عربي غير متوفر لهذا الإصدار._", "",
            "### Downloads", "",
            f"- `3re-{version}-windows-x64.zip` — portable: unzip anywhere and start `3re.exe`.",
            f"- `3re-{version}-windows-x64-setup.exe` — installer for your user (no admin rights).",
            "- The AI models are downloaded on first use (about 5 GB), with each one's size, source and licence shown first.",
            "",
            "> **Unsigned build.** The Windows files are not code-signed yet, so SmartScreen warns on first start: choose",
            "> **More info**, then **Run anyway**. Check the files against `SHA256SUMS.txt`, or verify the signed update manifest:",
            "> `minisign -Vm update-manifest.json -p 3re-update.pub` (key id 6EA665394615A750).",
        ])
    else:
        sys.exit(__doc__)
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write(text + "\n")
    print(f"{out}: {len(text)} characters")


main()
