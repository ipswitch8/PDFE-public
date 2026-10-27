# Secrets-scan finding triage

This document records the disposition for every finding in
`tests/security/secrets/baseline.json`. Each entry is one of:

| Disposition | Meaning |
|---|---|
| `false-positive` | The matched string is not a real secret (e.g., a test fixture's deliberately weak CA key, or a documentation example). Add an allowlist rule to `.gitleaks.toml` AND record the hash here. |
| `true-positive-rotated` | A real secret leaked. The credential has been **rotated upstream** (revoked, reissued, repository's old value is no longer valid). The hash stays in baseline because the credential string in history is no longer sensitive. |
| `true-positive-history-rewritten` | A real secret leaked. The history has been **rewritten** with `git filter-repo` to expunge the blob, AND the credential rotated upstream. The hash should drop out of subsequent runs because the blob no longer exists in the repo's history. |

## Process for true positives

When the scanner finds a real secret in history:

1. **Rotate the credential** at its source (cloud provider, CI
   secrets, etc.) so the leaked value is no longer valid. This
   step happens FIRST and must be confirmed before history
   rewrite.
2. **Document the rotation** in this triage.md.
3. **Rewrite history** with `git filter-repo --invert-paths --path
   <leaking-file>` (or `--replace-text` for in-place redaction).
   Coordinate with all collaborators because the rewrite changes
   commit hashes.
4. **Force-push the rewritten history** to the remote (this is
   destructive — get team sign-off first).
5. **Re-run the scanner** to confirm the finding is gone.
6. **Run `run_secrets.ps1 -UpdateBaseline`** so the now-clean
   state becomes the new baseline.

## Process for false positives

1. Add a path glob to `.gitleaks.toml [allowlist] paths` (or a
   stopword to `[allowlist] stopwords`) with a comment explaining
   why the match is benign.
2. Re-run the scanner — the finding should drop.
3. Run `run_secrets.ps1 -UpdateBaseline`.

## Per-entry format

```
### <rule_id> -- <file>:<line> commit=<short_hash>

**Disposition:** false-positive | true-positive-rotated | true-positive-history-rewritten

**Hash:** <sha1 from findings.json>

**Rationale:** (required)

**Rotation date:** (required for true-positive-*)

**History rewrite commit:** (required for true-positive-history-rewritten)
```

## Triaged findings

The first real full-history scan ran on **2026-10-20** during
pub-readiness phase-6 (GitHub #200), with `master` at `233391dc` — the tip
after the fourth history rewrite — over 1,648 commits. `baseline.json` carries
`stamp.head = 62f7fb11`, which is the HEAD at which the file was **re-written**
when the triage was corrected (see the note below); the scan population is the
same one, not a second scan. Both scanners were installed via
`scripts/security/install_tools.ps1`, which verifies each download
against a SHA-256 pinned in the script.

| scanner | version | exit | findings |
|---|---|---|---|
| gitleaks | 8.21.2 | 1 | **129** (118 `generic-api-key`, 10 `private-key`, 1 `sentry-access-token`) |
| trufflehog | 3.83.7 | 0 | **80**, of which **0 VERIFIED** |

**Verdict: no true positives.** All 209 findings are accounted for
across the **five** classes below. The strongest single signal is
trufflehog verifying **zero** of 80 — it attempts live validation of
candidate credentials, so a real key would be expected to verify.

Grouped **by class** rather than one entry per hash. 209 near-identical
rationales is how a baseline becomes unreadable and therefore
unreviewed. The per-class entries, with counts, are in `baseline.json`
under `accepted_findings`.

| class | gitleaks | trufflehog |
|---|---|---|
| vcpkg-abi-hashes | 117 | 62 |
| deliberate-test-keys | 10 | 15 |
| pinned-action-shas | 0 | 1 |
| test-passwords-and-prose | 2 | 0 |
| gtest-name-collision | 0 | 2 |
| **total** | **129** | **80** |

> **This triage was wrong the first time, and the correction is the
> point.** It originally claimed three classes — which summed to **127
> of 129** and **78 of 80**. Four findings were in no class at all, and
> the test-key class misattributed its own hits. Nothing caught it: the
> stamp was non-null, the prose said "all findings fall into the three
> classes below", and the phase read as complete. The `karen` gate found
> it by reconstructing per-file counts from the raw reports.
>
> `tools/gate/test_secrets_baseline_arithmetic.py` now fails if the
> per-class sums stop matching the stamped totals — proven by removing a
> class and watching it report `127 != 129`. It cannot detect a wrong
> *rationale*; only a reader can. It can detect the specific lie that
> everything is accounted for.

### vcpkg-abi-hashes — `docs/security/vcpkg-cache-manifest.json`

**Disposition:** false-positive · 117 gitleaks + 62 trufflehog

**Rationale:** the file is a vcpkg binary-cache manifest — a map from
installed file path to its 64-hex ABI hash — containing **7,366** such
hashes. A generic high-entropy rule reads any 64-hex string as a
possible key. The lone `sentry-access-token` hit sits at the ABI hash
for a Qt6 `QtNetwork` header: the rule matched the hash, not a token.
These digests exist for build reproducibility, are published
deliberately, and carry no authority.

### deliberate-test-keys

**Disposition:** false-positive · 10 gitleaks + 15 trufflehog

**Sources, corrected.** The first version of this entry named the seven
PEM fixtures as the source of the gitleaks hits. That was wrong, and
the `karen` gate caught it:

| scanner | where its hits actually are |
|---|---|
| gitleaks (10) | `tests/fixtures/gen_two_level_ca_signer.py` (**9**) and `tools/gate/test_check_test_key_markers.py` (**1**). **Zero** against any `*_key.pem`. |
| trufflehog (15) | the generator (**8**) and the seven PEM files themselves (**7**) |

**Rationale:** deliberately weak keys so the PDF signature / CRL / TSA
tests can sign and verify offline. Generated locally, never used to
authenticate anything.

**What the mechanism does and does not cover.**
`tools/gate/check_test_key_markers.py` requires a test marker on
committed PEM-bearing files and exits 0 over the tree — so the seven
fixtures and the generator are covered by a *check*, not an assertion.
It does **not** cover the single hit in its own test module, which
embeds PKCS8/PKCS1/ENCRYPTED PEM constants as fixtures: that checker
scans PEM-bearing files, not PEM literals inside itself. That one hit
rests on inspection. Stated because the earlier version implied the
mechanism covered everything in the class, and it does not.

### test-passwords-and-prose

**Disposition:** false-positive · 2 gitleaks

Added after the gate found these in no class at all.

* `tests/test_encryption_roundtrip.cpp` — a literal assigned to a field
  in an encryption round-trip test. A test password is the point of the
  test.
* `tests/ui_automation/dialog_openers.py` — inside a **comment**, where
  a quoted example string tripped the rule.

Neither authenticates anything.

### gtest-name-collision

**Disposition:** false-positive · 2 trufflehog (unverified)

Same origin — found by the gate, not by the triage. Both are committed
gtest listings (`…/phase-23/gtest_list.log` and
`tests/fixtures/gtest_list_output.txt`), and both hits are trufflehog's
`Box` detector matching the **test case name**
`CropPageCommandRedoReappliesCrop`. A test name that happens to fit a
token's character class is not a token.

### pinned-action-shas — `tools/gate/pin_workflow_actions.py`

**Disposition:** false-positive · 1 trufflehog (unverified)

**Rationale:** the file pins GitHub Actions to 40-hex commit SHAs,
which is the supply-chain hardening this repository requires. A token
detector reads 40-hex as a credential. A commit SHA grants nothing.

### If a future scan reports something else

A finding outside these five classes is **not** accepted. Triage it
here first, per the format above. **Do not widen a class to silence a
new file** — that is precisely how a baseline becomes a rubber stamp,
and the same failure mode this repository has already recorded for the
host-name ratchet.
