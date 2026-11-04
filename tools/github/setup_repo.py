"""Labels and good first issues for the 3re GitHub repository.

  python -I tools/github/setup_repo.py docs                 # writes docs/good-first-issues.md from the JSON
  python -I tools/github/setup_repo.py labels OWNER/REPO    # creates or updates .github/labels.json
  python -I tools/github/setup_repo.py issues OWNER/REPO    # opens .github/good-first-issues.json (skips titles that exist)

The token comes from the GH_TOKEN environment variable and is never printed. Run "labels" before "issues".
"""
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def load(name):
    with open(os.path.join(ROOT, ".github", name), encoding="utf-8") as f:
        return json.load(f)


def api(method, path, body=None):
    req = urllib.request.Request("https://api.github.com" + path, method=method,
                                 data=json.dumps(body).encode() if body is not None else None)
    req.add_header("Authorization", "Bearer " + os.environ["GH_TOKEN"])
    req.add_header("Accept", "application/vnd.github+json")
    req.add_header("User-Agent", "3re-setup")
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            raw = r.read()
            return r.status, json.loads(raw) if raw else None
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"null")


def docs():
    issues = load("good-first-issues.json")
    out = ["# Good first issues", "",
           "Small, self-contained tasks taken from real open items. Each one says where to look and when it is done.",
           "On GitHub they carry the `good first issue` label; comment on one before you start so two people do not",
           "do the same work. Generated from `.github/good-first-issues.json` by `tools/github/setup_repo.py docs`.", ""]
    for i, issue in enumerate(issues, 1):
        out += [f"## {i}. {issue['title']}", "", f"Labels: {', '.join('`' + l + '`' for l in issue['labels'])}", "",
                issue["body"], ""]
    with open(os.path.join(ROOT, "docs", "good-first-issues.md"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out))
    print(f"docs/good-first-issues.md: {len(issues)} issues")


def labels(repo):
    for label in load("labels.json"):
        st, _ = api("PATCH", f"/repos/{repo}/labels/{urllib.parse.quote(label['name'])}", label)
        if st == 404:
            st, _ = api("POST", f"/repos/{repo}/labels", label)
        print(f"{st} label {label['name']}")


def issues(repo):
    st, existing = api("GET", f"/repos/{repo}/issues?state=all&per_page=100")
    titles = {i["title"] for i in existing or []}
    for issue in load("good-first-issues.json"):
        if issue["title"] in titles:
            print(f"skip (exists) {issue['title']}")
            continue
        st, _ = api("POST", f"/repos/{repo}/issues", issue)
        print(f"{st} issue {issue['title']}")


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "docs":
        docs()
    elif cmd in ("labels", "issues") and len(sys.argv) == 3:
        (labels if cmd == "labels" else issues)(sys.argv[2])
    else:
        sys.exit(__doc__)
