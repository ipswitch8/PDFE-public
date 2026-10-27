# Phase 8 — Full-history secrets scan

Walks the entire git history (every blob in every commit on every
ref via `--all`) looking for committed credentials. Where the
existing security-audit gate hook only sees the current tree,
this dispatcher catches secrets that were committed at one
point and later removed — those blobs are still leaked because
they remain in history.

## OPT-IN posture

Invoked manually or by phase 9's nightly job. Not part of the
default ctest workflow. The scan is read-only against the
repository — it does not push anywhere, does not call out to
secret-validation services (we deliberately disable trufflehog's
optional verification step).

## Tooling prerequisites

| Tool | Required for | Where to get it |
|---|---|---|
| `git` | history walk | already installed (this is a git repo) |
| `gitleaks` | rule-based scan with PDFE-specific allowlist | https://github.com/gitleaks/gitleaks/releases or `scoop install gitleaks` |
| `trufflehog` | high-entropy / detector-based scan | https://github.com/trufflesecurity/trufflehog/releases |

`run_secrets.ps1` tolerates missing tools — it will run with
whichever subset is installed and warn about the rest. If
neither is installed it bails with exit 2.

## Files in this directory

| File | Purpose |
|---|---|
| `.gitleaks.toml`   | gitleaks config: extends defaults, allowlists PDFE test fixtures + build trees |
| `baseline.json`    | Hashes of accepted findings |
| `triage.md`        | Per-finding disposition (false-positive / true-positive-rotated / true-positive-history-rewritten) |
| `README.md`        | This file |

## Running

```powershell
# Default: both scanners
pwsh scripts/security/run_secrets.ps1

# Only gitleaks (e.g., trufflehog not installed)
pwsh scripts/security/run_secrets.ps1 -SkipTrufflehog

# Only trufflehog
pwsh scripts/security/run_secrets.ps1 -SkipGitleaks

# Update baseline after triaging
pwsh scripts/security/run_secrets.ps1 -UpdateBaseline
```

Outputs:

| File | Contents |
|---|---|
| `tests/security/reports/secrets/findings.json` | Normalized findings (tool, rule_id, file, line, commit, secret_kind, entropy, hash) |
| `tests/security/reports/secrets/summary.md`    | Counts per rule + per commit-author bucket |
| `tests/security/reports/secrets/gitleaks.json` | Raw gitleaks JSON output |
| `tests/security/reports/secrets/trufflehog.json` | Raw trufflehog JSONL output |

## What counts as a finding

The dispatcher's `secret_kind` taxonomy:

| Kind | Examples |
|---|---|
| `private_key` | RSA / EC private keys, PKCS#8, PEM files |
| `api_key`     | AWS / GCP / Azure / GitHub / Stripe / Slack tokens |
| `token`       | JWT, generic bearer tokens |
| `password`    | basic-auth strings, hardcoded passwords |
| `other`       | high-entropy strings the tool flagged but didn't classify |

## Severity gate

The dispatcher exits non-zero if **any finding** appears whose
hash is not in `baseline.json.accepted_findings` AND is not
silenced by an `.gitleaks.toml` allowlist rule. Each accepted
entry is `{hash, rationale}` -- the rationale field is mandatory
(per AC3) and should reference the corresponding `triage.md`
entry. To accept a new finding:

1. Categorize as false-positive or true-positive in `triage.md`.
2. For false positives, add an allowlist rule to `.gitleaks.toml`
   AND record the hash in baseline (via `-UpdateBaseline`).
3. For true positives:
   1. Rotate the credential FIRST.
   2. Decide whether history rewrite is necessary (always yes
      for high-value credentials; optional for already-low-value
      ones whose rotation neutralizes the leak).
   3. If rewriting: `git filter-repo --invert-paths --path
      <file>` (coordinate with collaborators — destructive).
   4. Re-run the scanner; the finding should drop.
   5. `-UpdateBaseline` to record the now-clean state.

## Why no live baseline is committed

Same Option A pattern as phases 3-7: the live full-history scan
requires both `gitleaks` and `trufflehog` installed and time to
walk the entire git log (~2-5 minutes for a repo this size).
Producing the canonical baseline is a hands-on step:

1. Install gitleaks (download release binary or `scoop install
   gitleaks`).
2. Install trufflehog (download release binary).
3. `pwsh scripts/security/run_secrets.ps1`.
4. Open `tests/security/reports/secrets/findings.json`. For each
   finding, add a triage entry in `triage.md`.
5. For true positives: rotate + (optionally) rewrite history per
   the procedure above.
6. For false positives: extend `.gitleaks.toml` allowlist.
7. `pwsh scripts/security/run_secrets.ps1 -UpdateBaseline`.

Phase 9's regression check then compares all future runs to that
baseline.

## Limitations

- **Removed-but-never-rewritten secrets are still findings.** A
  credential `git rm`'d in commit 42 still appears in commit 41's
  blob; both gitleaks and trufflehog will find it. The remediation
  is `git filter-repo`, not just `git rm`.
- **Verification is disabled.** trufflehog's `--verify` flag
  would call out to the actual cloud providers to confirm a
  detected key is live. We do NOT enable it: that would leak the
  fact-of-detection to the attacker AND test live credentials,
  which is risky on a CI machine. False positives stay false
  positives until manually rotated/triaged.
- **Author-bucket grouping uses `git show`.** If a commit's
  author email is missing from local config, the finding gets
  `<unknown>` in the per-author breakdown.
