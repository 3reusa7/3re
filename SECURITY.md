# Security policy

## Reporting a vulnerability

Please do **not** open a public issue for a security problem. Use GitHub's private reporting: the repository's
**Security** tab → **Report a vulnerability**. We answer within 7 days and agree on a disclosure date with you
(normally when a fixed release is out, at most 90 days).

Especially welcome: anything that could make 3re install or run code that the release key did not sign — the update
check, manifest signature, download verification, unpacking, the folder swap (`apps/update/`), the first-run model
downloads (`app/models.json`), or the handling of pictures, projects and GLB files from untrusted sources.

## Supported versions

Only the newest release receives security fixes. 3re updates itself only after you confirm (Settings → Updates).

## How releases are protected

- Each release's `update-manifest.json` is signed with minisign (Ed25519); 3re verifies it with a built-in public key
  (`tools/release/3re-update.pub`) and checks the SHA-256 of every file before installing.
- The signing secret exists only in the repository's Actions secrets and an offline backup.
- Downloaded models are pinned by revision and SHA-256.
- The Windows binaries are **not code-signed** yet; verify the SHA-256 listed in each release
  (`SHA256SUMS.txt`) or the manifest signature (`docs/UPDATES.md`).
