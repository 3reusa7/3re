"""Fetch the pinned upstream source archives into an isolated tool directory.

No model downloads, installers, solver substitutions, or certificate bypasses.
The original upstream submodule commits are required. Blossom V is excluded.
"""
import argparse
import hashlib
import json
import tarfile
import urllib.request
from pathlib import Path

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('root', type=Path)
    args = ap.parse_args()
    root = args.root.resolve()
    source = root / 'source' / 'quadwild-bimdf-0.0.2'
    pins = {e['path']: e['sha'] for e in json.loads((root / 'source-tree.json').read_text(encoding='utf-8-sig'))['tree'] if e['mode'] == '160000'}
    repositories = {
        'cmake-library': ('rwth', 'cmake/cmake-library'),
        'libs/CoMISo': ('rwth', 'CoMISo/CoMISo'),
        'libs/OpenMesh': ('rwth', 'OpenMesh/OpenMesh'),
        'libs/eigen': ('gitlab', 'libeigen/eigen'),
        'libs/json': ('github', 'nlohmann/json'),
        'libs/lemon': ('github', 'cgg-bern/lemon'),
        'libs/libigl': ('github', 'libigl/libigl'),
        'libs/satsuma': ('github', 'cgg-bern/libsatsuma'),
        'libs/timekeeper': ('github', 'cgg-bern/libTimekeeper'),
        'libs/vcglib': ('github', 'cnr-isti-vclab/vcglib'),
        'libs/xfield_tracer': ('github', 'cgg-bern/xfield_tracer_fork'),
    }
    for parent, metadata in [('libs/CoMISo', 'nested-comiso.json'), ('libs/OpenMesh', 'nested-openmesh.json')]:
        entries = json.loads((root / metadata).read_text(encoding='utf-8-sig'))
        if isinstance(entries, dict):
            entries = [entries]
        for entry in entries:
            path = parent + '/' + entry['path']
            pins[path] = entry['id']
            repositories[path] = ('rwth', 'CoMISo/Base' if entry['path'] == 'Base' else 'cmake/cmake-library')
    cache = root / 'archives'
    cache.mkdir(exist_ok=True)
    manifest = []
    for path, (host, repo) in repositories.items():
        sha = pins[path]
        name = repo.split('/')[-1]
        if host == 'github':
            url = f'https://codeload.github.com/{repo}/tar.gz/{sha}'
        else:
            domain = 'gitlab.vci.rwth-aachen.de:9000' if host == 'rwth' else 'gitlab.com'
            url = f'https://{domain}/{repo}/-/archive/{sha}/{name}-{sha}.tar.gz'
        archive = cache / f'{name}-{sha}.tar.gz'
        if not archive.exists():
            print(f'Download pinned source: {path}', flush=True)
            temp = archive.with_suffix('.part')
            req = urllib.request.Request(url, headers={'User-Agent': '3re-QuadWild-source-audit'})
            with urllib.request.urlopen(req, timeout=45) as response, temp.open('wb') as out:
                while block := response.read(1024 * 1024):
                    out.write(block)
                    if out.tell() > 120 * 1024 * 1024:
                        raise RuntimeError('Unexpectedly large source archive')
            temp.rename(archive)
        target = source / path
        marker = target / '.3re-source-pin'
        if not marker.exists():
            if target.exists() and any(target.iterdir()):
                raise RuntimeError(f'Refusing to overwrite existing source: {target}')
            target.mkdir(parents=True, exist_ok=True)
            with tarfile.open(archive) as tar:
                members = tar.getmembers()
                prefix = members[0].name.split('/')[0] + '/'
                for member in members:
                    if member.name.rstrip('/') == prefix.rstrip('/'):
                        continue
                    if not member.name.startswith(prefix):
                        raise RuntimeError('Unexpected archive root')
                    member.name = member.name[len(prefix):]
                    tar.extract(member, target, filter='data')
            marker.write_text(sha + '\n')
        if marker.read_text().strip() != sha:
            raise RuntimeError('Wrong cached source pin')
        entry = dict(path=path, commit=sha, url=url, bytes=archive.stat().st_size,
                     sha256=hashlib.sha256(archive.read_bytes()).hexdigest())
        manifest.append(entry)
        (root / 'dependency-sources.json').write_text(json.dumps(manifest, indent=2))
        print(f'OK {path}: {entry["bytes"]} bytes', flush=True)

if __name__ == '__main__':
    main()
