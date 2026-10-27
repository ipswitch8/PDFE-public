# PDFE Security Test Harness

**Status:** OPT-IN. Not part of the default test sequence. Not part of
`pdfe_tests.exe`. Not part of the standard CI green-bar requirement.

This directory holds the security-test harness that runs against PDFE.
The data-layer and UI tests in `tests/test_*.cpp` continue to be the
green-bar suite (2861 GoogleTest cases as of session 2026-04-23).
Security testing is a separate, explicit invocation.

## Threat model in scope

PDFE is a **local-only desktop application**. There is no cloud
upload, no remote signing endpoint of its own, no telemetry. The
security-relevant attack surfaces it does have:

| Surface | Risk | Existing mitigation |
|---|---|---|
| MuPDF parsing of arbitrary PDFs | RCE / crash via malformed objects, integer overflows, OOB reads | We rely on upstream MuPDF for the bulk; this harness fuzzes our wrapper code and runs malicious-pdf payloads to ensure PDFE handles upstream failures gracefully |
| Embedded JavaScript (`/JS`, OpenAction, form events) | Script-driven exfiltration / hangs via the MuJS engine | JS runs in MuJS sandbox; phase 3 verifies no callback attempt escapes |
| `/Launch` link action | Arbitrary local-file execution if followed without prompting | `MainWindow::followLinkAt` shows a confirmation dialog and only invokes `QDesktopServices::openUrl` after explicit user OK; phase 3 verifies the prompt |
| `/SubmitForm` to URL | Form-data exfiltration | `FormSubmitter` enforces a host allowlist (`HostAllowlist`) and uses `QNetworkAccessManager` |
| Embedded files / portfolios | Drop-and-execute via attached files | Extraction is explicit user action only; sanitizer step in `AttachmentManager::sanitizeFilename` blocks path traversal |
| `/Movie`, `/Sound`, `/3D` | Multimedia exec | Out of scope: not implemented. Annotations are read but not played |
| Form JavaScript | Form-driven exfiltration | MuJS sandbox; no `XMLHttpRequest`, no filesystem access |
| Digital signatures | Forged trust if certificate validation is weak | OpenSSL PKCS#7 chain verification; CA-chain fixtures exercise the full flow; certification (`/DocMDP`) is supported |
| Local file access via Launch | Auto-execute risk | Viewer never auto-executes `/Launch`; explicit confirmation dialog |
| Network egress | Data exfiltration | `HostAllowlist` for form submit; phase 3 sandbox blocks all non-loopback for runner |

What is **out of scope** for this harness:

- Server-side / cloud vulnerabilities (PDFE has no server)
- Authentication / session attacks (PDFE has no authentication)
- Web vulnerabilities (PDFE is not a web app — Selenium does not apply)
- Plugin SDK exploits (no plugin architecture by design)

## Opt-in posture

Security tests are **excluded** from the default test sequence to keep
the green-bar fast and to avoid the security harness ever being part
of an "if `ctest` passes, ship it" decision.

| Invocation | Runs |
|---|---|
| `ctest` (no flags) | The 2861 standard GoogleTest cases. Zero security tests. |
| `ctest -L security` | Only the security suite (after `-DPDFE_ENABLE_SECURITY_TESTS=ON` at configure time). |
| `cmake --build … --target pdfe_security_nightly` | Full nightly run (see `scripts/security/run_all.ps1`). Not a dependency of any default target. |

`PDFE_ENABLE_SECURITY_TESTS` defaults to **OFF**. A clean
`cmake --preset windows-vs2022` builds zero security artifacts.

## Layout

| Path | Purpose |
|---|---|
| `corpus/` | Malicious-PDF corpus + manifest (phase 2). Generated PDFs are gitignored; the generator script and manifest are committed |
| `fuzz/` | libFuzzer + AFL++ harnesses (phase 4). Built only when `PDFE_ENABLE_FUZZING=ON` |
| `sast/` | cppcheck + clang-tidy config + baselines (phase 5) |
| `deps/` | SBOM + CVE baseline (phase 6) |
| `sanitizers/` | ASan + UBSan suppression files + baseline (phase 7) |
| `secrets/` | gitleaks config + baseline of accepted false positives (phase 8) |
| `runner/` | Pywinauto driver (`run_corpus.py`), bootstrap (`bootstrap.ps1`), opt-in verifier (`verify_optin.ps1`) |
| `reports/` | All run artifacts (gitignored except for canonical baselines copied to `regression_baselines/`) |
| `regression_baselines/` | Snapshot of every phase's baseline so a single directory holds the regression contract |

## How to run each phase locally

Prerequisites:

```powershell
cd tests\security
.\runner\bootstrap.ps1
```

The bootstrap creates a Python venv, installs `requirements.txt`, and
populates `tool_inventory.json` with which external tools are available
on this machine.

| Phase | Script | Output |
|---|---|---|
| 2 — corpus generation | `scripts\security\gen_malicious_corpus.ps1` | `tests/security/corpus/malicious_pdf/` |
| 3 — sandboxed runner | `tests\security\runner\run_corpus.py` | `tests/security/reports/corpus_run_<timestamp>.json` + `.md` + loopback log |
| 4 — fuzzing | `scripts\security\run_fuzz.ps1` | `tests/security/reports/fuzz/<harness>/` |
| 5 — SAST | `scripts\security\run_sast.ps1` | `tests/security/reports/sast/findings.json` + summary |
| 6 — deps CVE | `scripts\security\run_deps.ps1` | `tests/security/reports/deps/cves.json` + SBOM |
| 7 — sanitizers | `scripts\security\run_sanitizers.ps1` | `tests/security/reports/sanitizers/<run_id>/` |
| 8 — secrets sweep | `scripts\security\run_secrets.ps1` | `tests/security/reports/secrets/findings.json` |
| All | `scripts\security\run_all.ps1` | Composite output of all phases |

## Regression workflow

1. After a phase run produces a new baseline, copy the relevant file
   into `regression_baselines/`.
2. Future runs compare against `regression_baselines/`; any new
   high-severity item fails the `ctest -L security` invocation.
3. To legitimately update a baseline, follow the convention in
   `tests/security/CHANGELOG.md` (commit message prefix
   `security-baseline:` so the security-audit gate picks it up).

## Why this layout

The harness is structured so that **adding new attack-surface tests is
a single-file change**. New tests under any subdirectory are picked up
by the `security` CTest label automatically (per the per-phase CMake
files). The split between `reports/` (transient) and
`regression_baselines/` (committed) keeps the repo small while the
contract is explicit.

## See also

- `docs/security-testing.md` — durable entry-point doc (created in phase 9)
- `docs/manual-verification.md` — manual UI verification (separate from this harness)
- `docs/image-pdf-risks.md` — pre-existing risk catalog the corpus manifest cross-references
- `.claude/pipeline.json` — the 9-phase plan that produced this harness
