# Dependabot coverage — accept-and-document decision (SEC-81 / GitHub #64)

**Status:** `.github/dependabot.yml` exists, covers `github-actions` only.
**Finding:** GitHub #64 (SEC-81), meta-audit gap-register.
**Owner:** PDFE maintainers.
**Last reviewed:** 2026-09-07 (backlog-workable-p82, phase-13).

---

## 1. What was missing

`.github/dependabot.yml` did not exist before this phase (verified: `Test-Path`
returned false, and no such file was tracked). #64's proposed work: "Create
`.github/dependabot.yml` (or accept-and-document, given vcpkg is unsupported
by Dependabot)".

## 2. What the new file covers, and why that is only part of the picture

PDFE's highest-value third-party dependencies — MuPDF, QPDF, Qt, FreeType,
HarfBuzz, Tesseract, ONNX Runtime, SQLite — are all resolved through
**vcpkg**. Dependabot has **no `vcpkg` package-ecosystem**; this was checked
against Dependabot's supported-ecosystems list, not assumed. So no
`.github/dependabot.yml` configuration, however written, can watch the
dependencies that matter most for this project.

**Compensating control (already exists, now wired):**
`scripts/security/run_deps.ps1` builds an SPDX SBOM covering both the vcpkg
tree and the Rust `tantivy_bridge` Cargo tree, scans it with OSV-Scanner
(fallback grype) and `cargo audit`, and fails a run when a new CVSS >= 7.0
finding appears that is not in the accepted baseline. Before this phase it was
invoked by nothing (GitHub #145); it is now wired into
`.github/workflows/dependency-audit.yml` (#50/#63; workflow_dispatch only, see
that workflow's own header for why there is no `schedule:` trigger). That
lane, not Dependabot, is PDFE's actual dependency-CVE control.

**What Dependabot's `github-actions` ecosystem DOES add**, and it is a real
gap it closes rather than decoration: this repo runs a self-hosted Windows
runner with a build toolchain and code-signing material nearby, and #147
already found and fixed 44 `uses:` references pinned to mutable tags (one of
them, `lukka/run-vcpkg`, was not even a tag — a branch). SHA-pinning stops the
tag from being repointed underneath CI, but it also means nobody gets bumped
to a newer, patched action version without a human noticing the SHA is stale.
Dependabot's `github-actions` ecosystem watches exactly that and opens a PR
that (per #147's own convention) updates both the SHA and its `# vX.Y.Z`
comment together.

## 3. pip — considered, declined for now, and why (not "there's nothing to watch")

Five `requirements*.txt` files exist in this tree:
`tests/fixtures/image_pdf/requirements.txt`, `tests/security/requirements.txt`,
`tests/ui_automation/requirements.txt` +
`tests/ui_automation/requirements-locked.txt`, and
`tools/font_classifier/requirements.txt`. Dependabot's `pip` ecosystem could in
principle watch any of these directories.

The `tests/ui_automation/` pair is the one CI actually depends on, and it is a
**pip-compile source/lock pair**: `requirements.txt` is compiled with
`pip-compile --generate-hashes` into `requirements-locked.txt`, and CI installs
the locked file with `pip install --require-hashes`. Dependabot bumps
`requirements.txt`; it does not run `pip-compile`. Pointing Dependabot at this
directory would produce PRs that edit the unlocked source file while leaving
the hash-pinned file CI actually installs untouched — a PR that reads as a fix
and would break `pip install --require-hashes` (hash mismatch) the moment
someone merged it believing the lock file moved with it.

Wiring this correctly needs a re-lock step reacting to Dependabot's PR (e.g. a
workflow that re-runs `pip-compile --generate-hashes` and pushes the
regenerated lock file to the same branch) which is a genuine, scoped piece of
follow-up work, not something `dependabot.yml` alone can express. Left out of
this phase's `.github/dependabot.yml` for that reason, stated in the file's
own comments so the next reader does not have to re-derive it.

## 4. Cross-reference

`tests/docs/security_review_cadence.md` (historical-banded; see its own
tracking banner) already listed "GitHub Dependabot alerts" as a Q2 tool before
any `dependabot.yml` existed to produce them — this phase makes that line
actually true for the `github-actions` ecosystem.
