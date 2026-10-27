# PDFE security testing — canonical entry-point

<!-- tracking-banner -->
> **STATUS: LIVE OPERATIONAL STATE — keep maintaining this file.**
>
> Not a defect tracker and not superseded by GitHub Issues; a defect
> recorded only here is still untracked. See [`docs/TRACKING.md`](docs/TRACKING.md).

This document is the single canonical reference for PDFE's
security testing posture. It is what new contributors should read
first to understand:

- What threats the test suite is meant to catch
- How OPT-IN security testing differs from regular CI
- How to run each phase locally
- How to update baselines without blowing up the gate
- What's deliberately deferred (MSan, Coverity)

> **Claims in this document are machine-checked.** §(e) carries a
> claim table between `CLAIM-TABLE-BEGIN` / `CLAIM-TABLE-END` markers;
> every row names the workflow (or preset) file it rests on and a
> literal that must appear in that file. `SecurityTestingDocClaims` in
> `tests/test_doc_drift_guards.cpp` re-derives the table on every test
> run and fails if any claim no longer matches its file. That test also
> writes the cross-check evidence to
> `docs/audit/meta-audit-2026-07/followup/phase-9/doc-claims-crosscheck.json`.

## (a) Threat model

> **This section is the single canonical threat model for PDFE.**
> `docs/security-policy.md` states the user-facing controls and
> `tests/docs/security_workflows.md` inventories the CI lanes; neither
> is a threat model, and neither is `docs/compliance-architecture.md`
> (a phase-11 PDF/A + PDF/UA implementation plan that was previously
> miscounted as threat-model coverage — meta-audit gap `SEC-107`).
> A new threat surface is added HERE, in the table below.

PDFE is a native Windows PDF editor that opens attacker-controlled
input (every PDF a user opens is, at some level, untrusted).
Production threat surface, in order of priority:

| Surface | What can go wrong | Mitigated by |
|---|---|---|
| **PDF parser (MuPDF)** | Memory corruption (UAF / OOB / type confusion) crashing the app or executing attacker code | Phase 4 fuzz harnesses + Phase 7 ASan/UBSan |
| **PDF JavaScript (MuJS)** | JS-driven action invokes `/Launch` / `/SubmitForm` / network exfil | Phase 3 sandboxed runner with loopback listener; firewall block |
| **Embedded files / portfolios** | Polyglot files, name-tree cycles, malformed UTF-16 filenames | Phase 4 `fuzz_embedded_files`; Phase 3 corpus has `embedded`/`xfa`/`launch`/`submitform` payloads |
| **Form actions** | `/SubmitForm` to non-loopback, `/ImportData` from disk paths the user didn't authorize | Phase 3 corpus `submitform` category; HostAllowlist runtime check |
| **Cryptographic mistakes** | Hardcoded keys committed to history; weak signatures accepted by trust validation | Phase 8 full-history secrets scan; existing signature tests |
| **Dependency CVEs** | A patched CVE in MuPDF / QPDF / OpenSSL / libpng that PDFE happens to expose | Phase 6 vcpkg + cargo-audit |
| **Static-analysis defects** | Latent bugs hidden in normal Debug/Release builds | Phase 5 cppcheck + clang-tidy |
| **Local caches and derived state** (SEC-103, phase-8) | A cache or scratch database created under a path another account can write becomes a local-escalation surface: it is read back and trusted by PDFE, and it persists attacker-influenced text (font paths, extracted names). Pre-planting the file is the attack — the victim process then adopts whatever is there | `FontAnalysisCache` is `:memory:` at **every** construction site in the product (`src/ui/MainWindow.cpp`, `src/fonts/FontSubstitutionEngine.cpp` — both pass no path), asserted by `FontAnalysisCacheAcl.*`. When a path IS supplied, `pdfe::core::createOwnerOnlyFile` creates it with a DACL naming only the calling user, supplied **at creation** in the `SECURITY_ATTRIBUTES` so there is no window in which it carries the directory's inherited ACL — and an existing file with a wider DACL is **refused**, not adopted, falling back to in-memory. Per-user root: `pdfe::core::perUserDataDir()` (`%LOCALAPPDATA%\PDFE`) |
| **Mapped and substituted drives** (SEC-50, phase-8) | A drive letter is not evidence of locality. `net use Z: \\server\share` and `subst Z: \\server\share` both produce paths that look local, defeat a lexical UNC gate, and reach the same kernel redirector — so the NetBIOS/LLMNR/mDNS broadcast the gate exists to prevent fires anyway | `path_security::is_unc_or_remote` resolves a leading drive letter (`GetDriveTypeW` → `WNetGetConnectionW` → `QueryDosDeviceW`) rather than only inspecting the string. The resolving behaviour is on the name every call site already uses; opting down requires naming `is_lexically_unc_or_remote`. Not cached — a stale verdict here fails open. `tests/test_path_security.cpp::MappedDrive*` |
| **Temporary and scratch files** (SEC-100, GitHub #39) | Every `%TEMP%` write with a name an attacker can predict is a *plant-and-redirect* target: pre-create it as a junction or a hardlink and PDFE's own write lands somewhere it never intended, under PDFE's token. Separately, a WORKING file that a producer writes by name and a consumer re-reads by path (clipboard image paste, the combine service's image wrappers) can be substituted in the interval between the two — and for undo/redo that interval is the rest of the session | `src/core/TempFileSafety.h` is the single hardened family. `diagnosticsDir()` / `diagnosticsSubdir()` nest every TRE diagnostic dump inside one per-process root named from 128 CSPRNG bits, so all of them stop being aimable at once rather than one call site at a time. `PinnedTempFile` removes the substitution window instead of watching it (`CREATE_NEW`, no `FILE_SHARE_WRITE`, no `FILE_SHARE_DELETE`, held for the object's life). `writeFixedNameTempFile` keeps the one name that must stay fixed — `%TEMP%/pdfe_ui_status.txt`, the sentinel eight Python UI tests poll — and hardens the OPEN instead (`FILE_FLAG_OPEN_REPARSE_POINT`; `nNumberOfLinks > 1` refused). `writeNewFileExclusive` for extraction of document-controlled content. **13 tests, `TempFileSafetyPhase60.*`**, two of which exist because a primitive nothing calls proves nothing: `NoProductSiteStillDerivesATempNameFromAnAddressOrCounter` and `EveryMigratedSiteRoutesThroughTheHardenedPrimitive`. Known limit, stated in the test file rather than left to be discovered: the reparse-point flag protects the FINAL path component only — a junction planted on a PARENT directory is still traversed. Derived state under a per-user root is the SEC-103 row above; the plaintext search index under `%TEMP%` is `SearchIndexAcl*` (12 tests) |
| **Build and dependency supply chain** (SEC-101, GitHub #40) | The bytes that end up in `PDFE.exe` are chosen by a vcpkg registry baseline, four overlay ports, and a dozen `uses:`/download steps in CI. Any of them can be swapped without a source diff: a floating `uses:` tag re-pointed at new code, an unverified `curl` of a scanner binary, an overlay port whose upstream `SHA512` was relaxed, or a poisoned entry in the shared binary cache | `CiSupplyChain.*` (**11 tests**, split across `tests/test_ci_supply_chain.cpp` and `tests/test_ci_supplychain.cpp` — two files, one suite name) requires every external `uses:` to be SHA-pinned *and* to name its version in a comment, every remote fetch in a workflow to be hash-verified, and every scanner download to be followed by a SHA check. `SupplyChainInventory.*` (**4 tests**) requires the committed SPDX SBOM to enumerate every direct `vcpkg.json` dependency *with a version*, and requires the generator to fail closed so the assertion cannot be satisfied once by hand. Overlay-source re-pin reasoning: `docs/security/vcpkg-overlay-source-verification.md`, re-runnable as `python tools/gate/verify_vcpkg_overlays.py`. **Not covered:** the `VCPKG_BINARY_SOURCES` binary cache (`clear;files,C:\vcpkg-bincache,readwrite;x-gha,readwrite` in five workflows) has no integrity assertion anywhere — a cache hit is trusted on its ABI hash alone |
| **OCR input and output** (SEC-102, GitHub #41) | OCR is reached from attacker-controlled input twice over. Tesseract is handed a raster decoded from the PDF, and its hOCR *output* is then re-parsed as XML by PDFE — so document content round-trips through a second parser with a second entity/XXE surface. The language models are a third input, and they are not text (see the next row) | hOCR is parsed under the same hardened pugixml policy as every other XML entry point: entity declarations are refused even under the lenient hOCR policy, while a real Tesseract XHTML `DOCTYPE` must still parse — refusing that would break OCR on valid input, which is how a guard gets disabled. `XmlEntitySafety*` (**16 tests**, SEC-41 is the hOCR entry point specifically, including the UTF-16LE decode path fixed by F27), `HocrParser*` (**29**), `OcrPipeline*` (**13**), `OcrLanguage*` (**18**). The SBOM names `tesseract` as a direct dependency (`SupplyChainInventory.SbomEnumeratesEveryDirectVcpkgDependencyWithAVersion`) |
| **Committed third-party model blobs** (SEC-90, GitHub #69) | `tests/fixtures/tessdata/` holds three multi-megabyte binaries (16 MB total) that the OCR tests load and Tesseract executes decisions from. Until now **nothing in the repo said where they came from**, so nobody could re-verify them, tell a legitimate model bump from a substitution, or answer the licence question. A committed binary with no provenance is not checkable by anyone later — it can only be trusted | `tests/fixtures/tessdata/README.md` records, per file, the upstream repository and tag (`tesseract-ocr/tessdata_fast` @ `4.1.0`), the SHA-256, the git blob SHA-1, the byte size, the licence (Apache-2.0), and why the files are committed rather than fetched. The blob SHAs were **matched against the upstream API rather than asserted** — see that README for the exact re-verification command and for what the match does and does not prove. All three are now entries in `THIRD-PARTY-NOTICES.md` and in `tools/gate/check_third_party_notices.py`, which re-hashes the committed bytes on every run; before this they were in neither, and that gate reported *"9 committed third-party artifact(s) attributed … OK"* over an inventory that did not contain them. **Not covered:** the SBOM enumerates vcpkg packages only, so these three files still sit outside it — `tests/security/reports/deps/sbom.spdx.json` names `tesseract`, the engine, and knows nothing about the models it loads |
| **Autosave and crash-recovery copies** (SEC-104, GitHub #42) | Autosave writes a **complete PDF copy of the open document, unsaved edits included**, to `%APPDATA%\<org>\PDFE\recovery\<session>.pdf`, plus a JSON sidecar naming the original path. It survives until a clean save or an explicit discard, so a document a user opened once from a share is now a durable plaintext copy on the local profile | Location only: `AutosaveManager::defaultRecoveryDir()` uses `QStandardPaths::AppDataLocation`, i.e. inside the user profile, whose ACL Windows sets — **not** `pdfe::core::createOwnerOnlyDirectory`. **This is a stated ASSUMPTION, not a control**: PDFE assumes a single-user workstation on which the profile ACL holds, and it does not verify that assumption at runtime or in test. Recorded in `docs/security-policy.md` § "What's NOT yet covered". Behavioural coverage (`test_autosave_recovery.cpp`, `test_autosave_sidecar_integrity.cpp`, `test_autosave_matrix.cpp`) asserts capture and recovery; **no test asserts the directory's permissions**, so a regression that moved this directory somewhere world-readable would be caught by nothing |
| **Update channel** (SEC-105, GitHub #43) | PDFE has no update feed. The danger is the shape of the one somebody eventually writes: an unauthenticated manifest, a payload trusted because TLS delivered it, a downgrade to a version whose vulnerabilities are public, or an "install anyway" escape from a failed verification. Written after the code exists, a threat model ratifies whatever was built | The requirements are pinned **before** the feed lands, and the absence itself is asserted. `UpdateChannelRequirements.*` requires the documented design to signature-verify the manifest, to require a digest *and* Authenticode on the payload, to state that transport security is not authentication, to reject downgrade and replay, and to fail closed with no install-anyway path. `UpdateChannelStatus.*` asserts the channel is still unimplemented and documented as such, that the notify-only check stays bounded by UC-9, and that **the notifier contains no download or execute path**. 9 tests, `tests/test_update_channel_docs.cpp`. Risk acceptance for the unsigned-binary era: `docs/security/unsigned-binary-risk-acceptance.md` |
| **Pinned third-party security patches** (SEC-106, GitHub #44) | `vcpkg-overlay-ports/libmupdf/portfile.cmake:11-12` carries `0001-pdfe-jpx-decode-gate.patch`, which gates JPX decode OFF by default because of an OpenJPEG heap overflow on malformed JPX (#50). A patch is the most silently-droppable kind of control there is: on a MuPDF version bump it simply fails to apply, and **nothing about the resulting build looks different** | `SupplyChainInventory.MupdfOverlayStillAppliesTheJpxDecodeGatePatch` asserts the portfile still lists the patch and that the patch file still exists; `SupplyChainInventory.MupdfManifestConstraintMatchesTheOverlayPortVersion` stops the manifest and the overlay drifting apart, which is how the patch would end up rebased onto a version nobody ships. Procedure for a legitimate bump — re-base, never drop — is `docs/security-policy.md` § "Dependency bump runbook (MuPDF / QPDF)". The user-facing opt-in is `Security/jpxRenderingEnabled` |
| **Deliberately declined process mitigations** (SEC-59, GitHub #58) | Four `SetProcessMitigationPolicy` policies are NOT applied to the main process, each for a stated reason (CIG blocks PDFE's own non-Microsoft-signed binaries; ACG breaks Qt's QML/JS and MuJS; ExtensionPointDisable breaks NVDA/JAWS; FontDisable breaks Qt font fallback). The risk is not the decision — it is that a *declined* policy is invisible: nothing distinguishes "we considered it and said no" from "nobody ever tried", and either can be silently added to or removed from later | The reasoning lives at the decision site, in the `INTENTIONALLY NOT APPLIED` block of `src/app/ProcessMitigations.cpp`, and is restated in `docs/security-policy.md` § "Windows process mitigation policies". Two of the four objections are about what the *main* process loads, and neither applies to `pdfe_preflight` — which links MuPDF and nothing else while handling the most hostile input in the product — so ACG and CIG are applied **there**: `PreflightHardening.AcgAndCigAreActiveInTheHelperProcess`, with `PreflightHardening.TheHardenedHelperStillClassifiesDocumentsCorrectly` as the control that makes it shippable (a hardening failure in that child presents to the user as "PDFE cannot open any PDF"). `ProcessMitigationsTest.*` (**6 tests**) reads the applied policies back out of the running process; `ProcessMitigationsTest.AcgIsNotAppliedToTheMainProcess` pins the declined side. **Not covered:** no architecture test asserts the declined SET matches a named list, so adding or removing a decline is a source-comment change nothing reddens |

Not in threat model: nation-state attackers with kernel access;
side channels; physical access. PDFE is a desktop application,
not a sandbox or trust boundary.

## (b) OPT-IN posture (CRITICAL — read this carefully)

**Security testing is NOT part of the regular test sequence.**

- The standard `ctest` invocation runs the full `pdfe_tests`
  GoogleTest suite and **zero** security tests. (This sentence used to
  quote a fixed suite size of 2068 tests. It was thousands of tests out
  of date and nothing checked it, so the number is gone rather than
  re-guessed — meta-audit gap `SEC-82`. The live figure is whatever
  `ctest -N` reports.)
- `pdfe_tests.exe` (the GoogleTest binary) does NOT contain any
  security test cases. Security suites are separate executables
  and PowerShell scripts registered with CTest **only under the
  `security` label**.
- **CORRECTED 2026-08-01 (meta-audit follow-up F5, gap SEC-23).** This
  section used to say "No CI green-bar requirement gates on the
  security label." That is no longer true, and it was the reason the
  label ran nowhere. `.github/workflows/windows-validation.yml` now
  configures with `-DPDFE_ENABLE_SECURITY_TESTS=ON` and runs
  `ctest -C Debug -L security --output-on-failure` as a **gating**
  step (no `continue-on-error`) **on dispatch** — that lane's nightly
  `schedule: '20 4 * * *'` was removed on 2026-08-13 along with every
  other cron in this repository
  (`docs/audit/ci-manual-only-posture.md`), so these entries run when
  somebody runs them. Five entries are selected — `security_verify_optin`,
  `security_runner_decision_unit`, `security_runner_listener_unit`,
  `security_runner_firewall_log_unit`, `security_regression_check` —
  worst-case 900 s.
- The OPT-IN posture below is otherwise **unchanged for developers**:
  the flag is passed on the CI command line, not added to
  `CMakePresets.json`, so a local `cmake --preset windows-vs2022` still
  builds with `PDFE_ENABLE_SECURITY_TESTS=OFF` and a plain `ctest` still
  runs zero security tests. Only the CI lane opts in.
- The heavier phases (live corpus run, fuzz baseline, SAST sweep,
  dependency CVE check, ASan smoke, secrets scan) remain outside that
  gate — they take minutes-to-hours and need external tools. The
  `pdfe_security_nightly` target that drives them is still invoked by no
  workflow; that is gap SEC-24, recorded DEFERRED in
  `docs/audit/meta-audit-2026-07/followup/phase-5-dispositions.md`.
- Security testing runs only when **explicitly invoked** via:
  - `ctest -L security` (developer machine, opt-in regression)
  - `cmake --build . --target pdfe_security_nightly` (full-intensity
    nightly target, NOT a default build dependency)
  - The optional pre-push hook (see (d) below) — opt-in per
    developer.

Why this matters:

- Most contributors want fast feedback. A full security run is
  hostile to that loop.
- Several phases require external tools (gitleaks, trufflehog,
  cppcheck, osv-scanner, cargo-audit, clang-cl) that can't be
  assumed in every dev environment.
- Phases 3–8 each ship **scaffolding** (dispatcher, config,
  baseline, triage doc) committed to the repo, plus a labeled
  placeholder for the live baseline. The Option A pattern means
  any contributor can run a live phase locally and produce the
  real baseline; the absence of a real baseline doesn't fail the
  build.

## (c) Running each phase locally

### Step 0 (precheck) — verify A/V exclusions

Before any phase, verify that every folder phases 2-8 use is
excluded from anti-virus. Without this, A/V will silently delete
malicious-PDF payloads and fuzz crash artifacts mid-run and the
reports will look "clean" only because their inputs got
quarantined.

```
pwsh scripts/security/verify_av_exclusions.ps1
```

The script drops a tiny EICAR-bearing PDF into each target folder
(EICAR is the standardized harmless-but-AV-detected test string),
waits 5 seconds, and checks whether the file survived. PASS means
the exclusion is effective; FAIL means A/V deleted the file. The
script exits non-zero on any FAIL and prints the exact
`Add-MpPreference -ExclusionPath @(...)` snippet to fix it.

`run_all.ps1` runs this precheck automatically as step 0 and
aborts the whole sequence on failure (override with
`-SkipAvCheck` if you've verified some other way).

### Per-phase dispatchers

| Phase | Dispatcher | What it does |
|---|---|---|
| 3 corpus | `python tests/security/runner/run_corpus.py` (requires admin) | Drives PDFE.exe through every malicious-PDF payload under a network sandbox |
| 4 fuzz | `pwsh scripts/security/run_fuzz.ps1 -Smoke` | libFuzzer campaigns over every harness in `$AllHarnesses`, 5 min each (`-Nightly` = 60 min). No AFL++ — this row used to claim one; the script has never invoked it (`SEC-82`). |
| 5 SAST | `pwsh scripts/security/run_sast.ps1` | cppcheck + clang-tidy across `src/` |
| 6 deps | `pwsh scripts/security/run_deps.ps1` | vcpkg + cargo-audit + osv-scanner |
| 7 ASan/UBSan | `pwsh scripts/security/run_sanitizers.ps1` | clang-cl + sanitizers ctest run |
| 8 secrets | `pwsh scripts/security/run_secrets.ps1` | gitleaks + trufflehog full-history |

Each script has its own README with toolchain prerequisites:

- `tests/security/runner/README.md`
- `tests/security/fuzz/README.md`
- `tests/security/sast/README.md`
- `tests/security/deps/README.md`
- `tests/security/sanitizers/README.md`
- `tests/security/secrets/README.md`

## (d) Regression workflow

After a per-phase live run produces a clean baseline, the durable
gate is `scripts/security/check_regressions.ps1`, registered as
the CTest entry `security_regression_check` under the `security`
label. The script:

1. Reads canonical baselines from `tests/security/regression_baselines/`.
2. Reads fresh per-phase outputs from `tests/security/reports/<phase>/`.
3. Compares: any new high-severity item not in baseline → exit 1.
4. Phases with no fresh output (because the live run wasn't done
   since checkout) are reported as **skipped** rather than fail —
   the gate is honest about what it could and couldn't compare.

**To accept a new finding** (i.e., update the baseline):

1. Run the per-phase dispatcher with `-UpdateBaseline`.
2. Copy the updated per-phase baseline into
   `tests/security/regression_baselines/<phase>_baseline.json`.
3. Add a one-line entry to `tests/security/CHANGELOG.md`.
4. Commit with `security-baseline:` prefix in the commit subject.

The optional pre-push hook (`scripts/security/hooks/pre-push`,
installable via `pwsh scripts/security/install_hooks.ps1`) runs
`ctest -L security` before every push and blocks on regression.
Opt-in per developer; bypass once with `git push --no-verify`.

## (d.1) What actually invokes each dispatcher — and what invokes nothing

**ADDED 2026-09-18 (GitHub #201).** Everything above describes what the
security regime *can* do. This section is the only part that states what
runs it, and it is **machine-checked**, not prose:

```
python tools/gate/check_security_regime.py --check
```

That checker discovers every `scripts/security/*.ps1` in the tree, computes
its real invokers (workflow `run:` lines with comment lines stripped, CTest
registrations, CMake custom targets, and the local gate driver), and fails if
this table disagrees in **either** direction — a row claiming an invoker that
does not exist, *and* a row saying `NONE` for something that is in fact wired.
It runs in `fast-checks.yml`, so it fires on the commit that falsifies this
table rather than on the audit that finds it four months later. #201 exists
because the previous version of this claim was prose.

Status vocabulary: `AUTO` = reachable from `fast-checks.yml`, the only
auto-triggered workflow. `DISPATCH` = reachable only from a
`workflow_dispatch` lane. `CTEST` = a CTest entry. `CMAKE-TARGET` = a custom
CMake target. `LOCAL-GATE` = driven by a gate script the local sweep
(`tools/gate/run_fast_checks_locally.py`) runs. `NONE` = nothing invokes it.

<!-- BEGIN security-regime-dispositions -->

| Entry point | Status | Reason / who runs it |
|---|---|---|
| `scripts/security/check_regressions.ps1` | CTEST + LOCAL-GATE | CTest `security_regression_check` (`ctest -L security`, reachable only from the dispatch-only `windows-validation.yml`), **and** `tools/gate/run_security_regression_check.py`, which the local fast-checks sweep runs on every invocation. The local driver is the half that produces evidence today. |
| `scripts/security/run_deps.ps1` | DISPATCH | `dependency-audit.yml` and `windows-validation.yml`. Needs osv-scanner/cargo-audit and network; minutes, not seconds. Dispatch-only by design — see `dependency-audit.yml`'s header for why no schedule. |
| `scripts/security/run_secrets.ps1` | DISPATCH | `secrets-audit.yml`. Walks the FULL git history with gitleaks + trufflehog; needs `fetch-depth: 0` and both binaries. Dispatch-only by design. |
| `scripts/security/run_all.ps1` | CMAKE-TARGET | Driven by the `pdfe_security_nightly` custom target only, which no workflow builds. **Deliberate:** its runtime is unmeasured and plausibly multi-hour on a single-capacity runner (gap `SEC-24`, DEFERRED), and the owner's standing instruction is that recurring scheduled items are not being resurrected (`docs/audit/reverification-2026-08-10.md` row 3). Not wired, on purpose, until a measured wall-clock budget exists. |
| `scripts/security/run_sast.ps1` | NONE | No lane at all. Needs clang-cl plus a configured build tree, and its raw output (`tests/security/reports/sast/clang-tidy.txt`) embeds absolute build paths carrying the build account name ~3,210 times, so it can never be committed as evidence. Left unwired until the SAST lane question is decided; the committed `sast_baseline.json` is frozen, see below. |
| `scripts/security/run_fuzz.ps1` | NONE | Nothing invokes it, including `windows-fuzz.yml` — that lane builds and drives the libFuzzer harnesses itself and says so at `windows-fuzz.yml:780`. Every other mention of this file in `.github/` is a comment saying it is not reached. Superseded in practice, retained as the local/manual driver. |
| `scripts/security/run_sanitizers.ps1` | NONE | Superseded by `windows-sanitizer.yml`, which drives the ASan/UBSan build directly. Retained as the local driver; `run_sanitizers_local.ps1` is its developer-box variant. |
| `scripts/security/run_sanitizers_local.ps1` | NONE | Developer-box convenience wrapper; by definition has no CI invoker. |
| `scripts/security/gen_malicious_corpus.ps1` | NONE | Corpus *generator*, run once by hand to produce `tests/security/corpus/`; it is not a check and has nothing to gate. |
| `scripts/security/verify_av_exclusions.ps1` | NONE | Step-0 precheck for a human about to run a live phase on their own machine (it drops EICAR-bearing files to prove A/V will not eat scan inputs). Running that unattended in CI is not wanted. |
| `scripts/security/install_tools.ps1` | NONE | One-time machine setup (installs gitleaks/trufflehog/osv-scanner). Not a check. |
| `scripts/security/install_hooks.ps1` | NONE | One-time, opt-in per developer (installs the pre-push hook). Not a check. |
| `scripts/security/get_filehash_shim.ps1` | NONE | Not a check and not a dispatcher: it restores `Get-FileHash` via .NET when the Utility module does not auto-load, so `install_tools.ps1`'s PINNED-DIGEST verification can run at all. Dot-sourced on demand before that installer; nothing to wire into CI, where the cmdlet resolves natively and the shim no-ops. It exists because on 2026-10-20 that verification could not run on this host and the installer correctly FAILED CLOSED rather than installing unverified scanner binaries — the fix was to restore the missing primitive, not to skip the check. Committed (rather than used ad hoc) so the "known-answer tested" claim is inspectable; it self-tests against two published SHA-256 vectors and refuses to install itself if either fails. |

<!-- END security-regime-dispositions -->

### The regression baselines are FROZEN historical artifacts

`tests/security/regression_baselines/` is **not** a live control. Nothing
refreshes these files; each was written by a one-off live run on the date
below and has not been regenerated since. `check_regressions.ps1` compares
against them, so a comparison that passes means "no *new* high-severity item
relative to a snapshot that may be months old" — it does **not** mean the
scanner ran recently. Read them that way.

<!-- BEGIN frozen-baselines -->

| Baseline | Last refreshed (git) |
|---|---|
| `corpus_run_baseline.json` | 2026-04-25 |
| `deps_baseline.json` | 2026-04-25 |
| `fuzz_baseline.json` | 2026-08-22 |
| `sanitizers_baseline.json` | 2026-05-01 |
| `sast_baseline.json` | 2026-04-25 |
| `secrets_baseline.json` | 2026-04-25 |

<!-- END frozen-baselines -->

`check_security_regime.py` fails if a baseline appears in that directory
without a row here, so a new baseline cannot arrive silently and be mistaken
for a live control.

### What #201 did NOT fix, stated plainly

- `secrets-audit.yml` and `dependency-audit.yml` still have **zero runs**.
  They need the self-hosted runner and external binaries, and per the standing
  instruction `fast-checks.yml` remains the only workflow with an automatic
  trigger. Nothing here dispatched them.
- `run_sast.ps1` still has **no lane**, and `pdfe_security_nightly` still has
  **no workflow invoker** — both by the written decisions in the table above.
- `windows-validation.yml`, and therefore `ctest -L security`, is still
  dispatch-only with zero runs. The CTest path to `check_regressions.ps1`
  remains evidence-free; what changed is that the **local** path now exists
  and runs.
- The six baselines above are still stale. This section discloses that rather
  than refreshing them, because refreshing them requires the live runs above.

## (e) Sanitizers + fuzzing: Windows-native, on automatic triggers

**CORRECTED 2026-08-02 (meta-audit follow-up F9, gaps `SEC-29`,
`SEC-30`, `SEC-13`).** This section previously said phases 4
(libFuzzer) and 7 (ASan/UBSan) were "deferred to a Linux build setup"
and listed ASan / UBSan / MSan / TSan / libFuzzer as "Linux-only". That
was not stale — it was **inverted**. The truth is the mirror image:

- Sanitizers and fuzzing run **natively on Windows**, on the self-hosted
  `pdfe-windows` runner, and **on manual dispatch only** — the automatic
  schedules were removed on 2026-08-13
  (`docs/audit/ci-manual-only-posture.md`). Nothing here should be read
  as a claim that these lanes fire by themselves.
- **Linux is the platform that does not work.** PDFE does not compile
  on Linux at all (`src/core/Document.cpp` assumes
  `std::filesystem::path::value_type == wchar_t`, plus other
  Windows-only assumptions), which is why `sanitizer.yml` and
  `fuzz.yml` are *retired, dispatch-only* stubs and not the future.
  See `CI-LIMITATION-PDFE-IS-WINDOWS-ONLY` in
  `tests/docs/known_issues.md`; the port is tracked as `#42`.

Anyone who read the old text and concluded "we have no sanitizer or
fuzz coverage until someone builds a Linux box" was wrong in exactly
the direction that stops work getting done. That is the cost this
correction retires.

### What actually runs, and on what trigger

Every row below is machine-checked — see the claim table further down.

| Lane | Where | Trigger | Notes |
|---|---|---|---|
| **MSVC AddressSanitizer** | `.github/workflows/windows-sanitizer.yml` | `workflow_dispatch` ONLY (every schedule removed 2026-08-13 — `docs/audit/ci-manual-only-posture.md`) | Security-focused gtest subset under `/fsanitize=address`. A `schedule:` was added by F4 (`SEC-16`) as weekly and tightened to Sun/Tue/Thu by F30, then **removed entirely on 2026-08-13** with every other cron in the repository. **The detection window is now UNBOUNDED**, which matters here more than anywhere: #47 and #48 were ASan-ONLY finds, so a UAF on this surface lives until somebody dispatches the lane. `push:` is deliberately absent so a ~6 h build cannot be started by every commit on the single shared runner. `tools/gate/check_asan_cadence.py` still measures real firing gaps rather than matching cron strings; with no cadence it passes only because the manual-only posture is DOCUMENTED, and it says in its own output that the window is unbounded rather than bounded. |
| **libFuzzer campaigns** | `.github/workflows/windows-fuzz.yml` | `workflow_dispatch` ONLY (schedule removed 2026-08-13); a manual run defaults to the full 1800 s | clang-cl `-DPDFE_FUZZ_BACKEND=libfuzzer`, all harnesses in `tests/security/fuzz/CMakeLists.txt`, corpus cached and `-merge`d run-over-run. A real crash fails the run via the separate blocking `real-crash-gate` job. |
| **`ctest -L security`** | `.github/workflows/windows-validation.yml` | `workflow_dispatch` ONLY (schedule removed 2026-08-13) | Configures `-DPDFE_ENABLE_SECURITY_TESTS=ON` and runs `ctest -C Debug -L security` as a **gating** step (no `continue-on-error`). Added by follow-up F5 (`SEC-23`) — see §(b), which the same change corrected. |
| **Local ASan/UBSan** | `CMakePresets.json` | on demand | `windows-vs2022-asan` (clang-cl, ASan+UBSan) and `windows-vs2022-msvc-asan` (MSVC ASan only). Both are opt-in presets with their own binary dirs. |
| **Local fuzz** | `pwsh scripts/security/run_fuzz.ps1 -Smoke` | on demand | Requires `-DPDFE_ENABLE_FUZZING=ON` + clang-cl. |
| Linux ASan/UBSan | `.github/workflows/sanitizer.yml` | dispatch-only, **RETIRED** | Kept only to preserve build intent for a future port. |
| Linux fuzz | `.github/workflows/fuzz.yml` | dispatch-only, **RETIRED** | Same. |

### Sanitizer availability — corrected

| Sanitizer | What it catches | Where it actually runs today |
|---|---|---|
| ASan | Heap UAF, OOB | **Windows, automatic** — `windows-sanitizer.yml`, Sun/Tue/Thu 18:00 UTC (max 3-day gap; weekly until F30 tightened it). Local: either ASan preset. Found real defects #47 and #48. |
| UBSan | Signed overflow, bad shifts, misaligned load | **Windows, on demand** — the clang-cl `windows-vs2022-asan` preset builds with `-fsanitize=undefined`. Not on the CI schedule (the CI lane is the MSVC one, and MSVC has no UBSan). |
| libFuzzer | Coverage-guided fuzz | **Windows, automatic** — `windows-fuzz.yml`, nightly, clang-cl. |
| **MSan** | Uninitialized memory reads | **Nowhere.** clang-cl does not support MSan and PDFE does not build on Linux. Genuinely absent — gap `SEC-17`, recorded **BLOCKED-BY-#42**, not closed. |
| TSan | Data races | **Nowhere.** Same reason (no Windows support, no Linux build). Linux half of gap `SEC-19`, also BLOCKED-BY-#42. |

MSan and TSan are the only two rows the original "deferred to Linux"
framing got right, and they are blocked on the Linux port, not on
someone finding the time.

### Claim table (machine-checked)

Each row is verified on every test run by `SecurityTestingDocClaims`
in `tests/test_doc_drift_guards.cpp`: the named file must exist and
must contain the literal, verbatim. Add a claim here, or the doc is
not allowed to make it.

<!-- CLAIM-TABLE-BEGIN -->

| Claim id | Claim | File it rests on | Literal that must appear in that file |
|---|---|---|---|
| C01 | The Windows MSVC ASan lane is MANUAL ONLY — no cadence, so the detection window is unbounded | `.github/workflows/windows-sanitizer.yml` | `workflow_dispatch:` |
| C02 | The Windows ASan lane is a real sanitizer build | `.github/workflows/windows-sanitizer.yml` | `fsanitize=address` |
| C03 | The Windows libFuzzer campaign is MANUAL ONLY | `.github/workflows/windows-fuzz.yml` | `workflow_dispatch:` |
| C04 | The campaign builds with the libFuzzer backend | `.github/workflows/windows-fuzz.yml` | `-DPDFE_FUZZ_BACKEND=libfuzzer` |
| C05 | A real fuzz crash fails the run (not just the step) | `.github/workflows/windows-fuzz.yml` | `real-crash-gate` |
| C06 | CI configures the security label ON | `.github/workflows/windows-validation.yml` | `-DPDFE_ENABLE_SECURITY_TESTS=ON` |
| C07 | CI actually runs the security-labelled entries | `.github/workflows/windows-validation.yml` | `ctest -C Debug -L security` |
| C08 | That validation lane is MANUAL ONLY | `.github/workflows/windows-validation.yml` | `workflow_dispatch:` |
| C09 | The Linux sanitizer lane is retired, dispatch-only | `.github/workflows/sanitizer.yml` | `Auto-triggers intentionally retired` |
| C10 | The Linux fuzz lane is retired, dispatch-only | `.github/workflows/fuzz.yml` | `Auto-triggers intentionally retired` |
| C11 | Linux is retired because PDFE does not compile there | `.github/workflows/sanitizer.yml` | `CI-LIMITATION-PDFE-IS-WINDOWS-ONLY` |
| C12 | A clang-cl ASan+UBSan preset exists for local runs | `CMakePresets.json` | `windows-vs2022-asan` |
| C13 | A manual fuzz run gets the FULL per-harness duration, not the 300 s the removed schedule branch would have selected (1270 s since phase-73 re-derived the budget for 17 harnesses; was 1800 s) | `.github/workflows/windows-fuzz.yml` | `'1270' }}` |
| C23 | An MSVC ASan preset exists for local runs | `CMakePresets.json` | `windows-vs2022-msvc-asan` |
| C14 | The clang-cl preset really enables UBSan | `CMakePresets.json` | `-fsanitize=undefined` |
| C15 | Fuzz harnesses are registered as build targets | `tests/security/fuzz/CMakeLists.txt` | `pdfe_add_fuzz_target` |
| C16 | The temp/scratch hardening family exists as a single unit, not per-call-site (SEC-100) | `src/core/TempFileSafety.h` | `writeFixedNameTempFile` |
| C17 | The MuPDF overlay still applies the JPX decode-gate patch (SEC-106 / SEC-83) | `vcpkg-overlay-ports/libmupdf/portfile.cmake` | `0001-pdfe-jpx-decode-gate.patch` |
| C18 | The declined mitigation policies are reasoned at the decision site (SEC-59) | `src/app/ProcessMitigations.cpp` | `INTENTIONALLY NOT APPLIED` |
| C19 | The committed SBOM names the OCR engine as a direct dependency (SEC-102) | `tests/security/reports/deps/sbom.spdx.json` | `"name": "tesseract"` |
| C20 | The committed OCR models record their upstream provenance (SEC-90) | `tests/fixtures/tessdata/README.md` | `tesseract-ocr/tessdata_fast` |
| C21 | Autosave recovery copies land in the per-user profile, not a shared location (SEC-104) | `src/core/AutosaveManager.cpp` | `QStandardPaths::AppDataLocation` |
| C22 | The update-channel requirements are pinned by tests before the feed exists (SEC-105) | `tests/test_update_channel_docs.cpp` | `UpdateChannelRequirements` |
| C24 | The MuPDF overlay still applies the JBIG2 decode-gate patch (SEC-56) | `vcpkg-overlay-ports/libmupdf/portfile.cmake` | `0002-pdfe-jbig2-decode-gate.patch` |

<!-- CLAIM-TABLE-END -->

### The historical Windows clang-cl notes

The original section catalogued four clang-cl + Ninja + Windows-SDK
problems hit during the first wiring attempt (clang-cl not on PATH,
`/fsanitize=undefined` slash-form rejection, ASan vs `/MDd`, and an
rc.exe / lld-link manifest failure). Those notes are still accurate as
*history* and are retained in
`tests/security/sanitizers/README.md`. What was wrong was the
conclusion drawn from them — the fourth problem was worked around by
using the **MSVC** ASan path for CI (`windows-vs2022-msvc-asan`) and
keeping clang-cl for fuzzing, which is why both lanes exist today.
See `CI-LIMITATION-CLANG-CL-ASAN-RUNTIME-WIN` in
`tests/docs/known_issues.md`.

### If the Linux port ever lands

It is tracked as `#42`. It would unlock MSan and TSan, and nothing
else that is not already running on Windows. The prerequisites are an
`x64-linux` vcpkg set, Linux clang, new `linux-debug-*` presets, and
POSIX equivalents of the two dispatcher scripts — but the blocker is
`src/core/`'s Windows assumptions, not the tooling.

## (f) Why Coverity is not used

PDFE has **no Coverity license**, and Coverity is commercial. The
phase-1 user decision explicitly dropped Coverity from scope.
Phase 5 (SAST) covers the same ground with cppcheck + clang-tidy,
both open-source. The dispatcher (`scripts/security/run_sast.ps1`)
contains zero Coverity invocations and zero `COVERITY_TOKEN`
conditionals; the only mentions of Coverity in the repo are
prose explaining its deliberate absence.

If a Coverity license becomes available in the future, adding a
Coverity-driven scan would be a separate phase (call it 5b),
parallel to but not replacing cppcheck/clang-tidy. The
findings.json schema (tool / rule_id / severity / file / line /
message / hash) is general enough to absorb Coverity output.

---

## Recommended workflow (minimal user interaction)

For an unattended run that captures everything for later analysis,
use `-AutoElevate`:

```powershell
pwsh scripts/security/run_all.ps1 -Nightly -AutoElevate
```

This:
1. Pops UAC **once** at launch.
2. Re-launches itself in an elevated child via `Start-Process -Verb RunAs`.
3. Runs the elevated child with `Start-Transcript` writing to
   `tests/security/reports/run_all/run_all_<stamp>.log`.
4. Parent waits with `-Wait`, then prints the tail of the
   transcript and the child's exit code.
5. Walk away during the run. After it finishes, all evidence is
   on disk:
   - **Transcript** — `tests/security/reports/run_all/run_all_<stamp>.log`
     (full stdout/stderr from every phase)
   - **Per-phase summaries** — `tests/security/reports/<phase>/summary.md`
   - **Findings JSON** — `tests/security/reports/<phase>/{findings,cves,sbom,...}.json`
   - **Crash artifacts** — `tests/security/reports/fuzz/run_*/<harness>/crashes/`
   - **Regression diff** — `tests/security/reports/regression_check.{json,md}`

Phase 3 is the only step that strictly needs admin (netsh firewall
rules); the others run fine unelevated. `-AutoElevate` simplifies
the user-facing UX to "single command, single UAC accept."

## Quick reference

```
# A/V exclusion precheck (run first; once per dev machine + after AV config changes)
pwsh scripts/security/verify_av_exclusions.ps1

# Full opt-in sequence (precheck + all phases)
pwsh scripts/security/run_all.ps1 -Smoke -AutoElevate
pwsh scripts/security/run_all.ps1 -Nightly -AutoElevate

# Opt-in security regression check (under 30 minutes locally)
ctest --test-dir build/vs2022 -L security -C Debug --output-on-failure

# Single-phase dispatcher
pwsh scripts/security/run_sast.ps1
pwsh scripts/security/run_deps.ps1
pwsh scripts/security/run_secrets.ps1
pwsh scripts/security/run_fuzz.ps1 -Smoke
pwsh scripts/security/run_sanitizers.ps1

# Optional pre-push hook (opt-in per developer)
pwsh scripts/security/install_hooks.ps1

# Update a baseline (after triaging in the matching triage.md)
pwsh scripts/security/run_<phase>.ps1 -UpdateBaseline
cp tests/security/<phase>/baseline.json tests/security/regression_baselines/<phase>_baseline.json
git commit -m "security-baseline: accept <reason>"
```
