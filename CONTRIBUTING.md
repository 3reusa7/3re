# Contributing to 3re

Thank you for helping. 3re is a small project with a strict habit: we measure before we claim. Please keep that
habit in issues, pull requests and docs.

## Ways to help

- **Hardware reports.** Run 3re on your PC and file a
  [hardware report](../../issues/new?template=hardware-report.yml): GPU, VRAM, driver, RAM, generation time, result.
  Every GPU other than the maintainer's RTX 3050 4 GB is untested.
- **Bugs.** Use the [bug template](../../issues/new?template=bug-report.yml); attach the self-test
  (`3re.exe --self-test --json selftest.json`) and the run's report if there is one.
- **Good first issues.** [docs/good-first-issues.md](docs/good-first-issues.md) lists small, self-contained tasks.
- **Docs and translation.** The interface is English today; Arabic is the second language of the docs.

## Pull requests

1. Open an issue first for anything bigger than a small fix, so we agree on the approach.
2. Branch from `main`; keep one topic per pull request.
3. Build and test as in [docs/BUILDING.md](docs/BUILDING.md). CI runs the Windows build, all tests, lint, the licence
   scan and the secret scan on every pull request.
4. Write code like the code around it (C++20, the existing naming and comment density; Qt only outside `core/`).
5. Add or update tests for what you change, and a line in `CHANGELOG.md` under the next version.
6. Never commit model weights, build outputs, personal paths, or pictures you do not have the rights to publish.
   Test pictures must be made by you or be CC0, with their source recorded next to them.

### Changing a model or a method

3re can change how it uses TRELLIS.2 or other models — samplers, quantisation, conditioning, post-processing — but
only after a written study (`docs/research/<topic>.md`: the problem with evidence, sources, feasibility on a 4 GB GPU,
licence, a hypothesis with acceptance criteria written **before** the experiment). Adopt the change only if the
criteria are met; a failed experiment is documented as a negative result.

## Developer Certificate of Origin (sign-off)

3re uses the [Developer Certificate of Origin 1.1](https://developercertificate.org/) instead of a CLA. By signing off
a commit you certify that you wrote it, or otherwise have the right to submit it under the project's licence
(Apache-2.0). Add the sign-off with `git commit -s`; it adds a line like:

```
Signed-off-by: Your Name <you@example.com>
```

The name and e-mail must be yours (GitHub's no-reply address is fine). Pull requests whose commits are not signed
off cannot be merged; `git rebase --signoff main` fixes a branch.

## Licence

Contributions are licensed under the Apache License 2.0, like the rest of 3re (see `LICENSE`, section 5).

## Conduct

Everyone taking part follows the [Code of Conduct](CODE_OF_CONDUCT.md).
