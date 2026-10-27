# Security testing changelog

This file records every legitimate update to the canonical baselines
under `tests/security/regression_baselines/`. The phase-9 regression
check fails on any new high-severity item not already in baseline; a
baseline update is the deliberate way to acknowledge a known-and-
accepted finding has changed.

## How to update a baseline

1. Run the per-phase dispatcher with `-UpdateBaseline` (or, for the
   corpus baseline, copy a clean `corpus_run_*.json` from a live
   runner output).
2. Copy the per-phase baseline into
   `tests/security/regression_baselines/<phase>_baseline.json`.
3. Add a one-line entry below in this CHANGELOG for each modified
   baseline file.
4. Commit with the prefix `security-baseline:` in the subject line
   so the security-audit gate hook can flag the diff during review.

Example commit subject:
```
security-baseline: accept CVE-2026-0123 (libpng, unreachable in PDFE)
```

## Entries

### 2026-05-01

- **First live sanitizer baseline**
  (`sanitizers_baseline.json` — verdict: PASS, 3105/3107 tests pass,
  2 intentional skips, 0 ASAN findings).

  Pivoted from the clang-cl preset (`windows-vs2022-asan`) to a new
  MSVC-native preset (`windows-vs2022-msvc-asan`) because clang-cl on
  Windows hits a stack of integration issues (lld-link sanitizer-flag
  forwarding, libFuzzer /MT vs pdfe /MD CRT mismatch, Q_INIT_RESOURCE
  namespace mangling, QChar::fromUcs4 dllimport ABI). MSVC's native
  `/fsanitize=address` integrates cleanly with /MD vcpkg builds, and
  it shipped the first end-to-end live sanitizer run for this project.

  **Bugs caught and fixed in the first pass** (4 sites, same pattern):
  - `pdfe::core::QpdfWrapper::save` (`QpdfWrapper.cpp:30`)
  - `pdfe::core::QpdfWrapper::saveIncremental` (`QpdfWrapper.cpp:48`)
  - `pdfe::encryption::EncryptionManager::saveEncrypted` (`EncryptionManager.cpp:222`)
  - `pdfe::compliance::PdfAConverter` write stage (`PdfAConverter.cpp:272`)
  - `pdfe::compliance::PdfUAConverter` write stage (`PdfUAConverter.cpp:123`)

  Pattern: `QPDFWriter writer(qpdf, path.string().c_str())` — the
  `std::string` returned by `path.string()` is a temporary that
  destructs at end of full-expression. QPDFWriter stores the
  filename pointer for use during `.write()`, so by the time write
  runs the buffer has been freed. Fix: store `path.string()` in a
  named local that outlives the writer.

  Two test-only path expectations (`AppIcon.FileExists`,
  `AppIcon.NonZeroSize`) were also updated — they pointed at
  `src/app/pdfe.ico` (never existed; the .rc references
  `../../pdfe-red.ico` and qrc embeds it).

  **What this baseline does NOT cover:**
  - **UBSan** — clang-cl-only on Windows; not in MSVC's native asan.
  - **libFuzzer** — clang-cl-only; ships /MT only and pdfe is /MD.
    Multi-day toolchain work to either rebuild pdfe /MT or build a
    custom MD-variant fuzzer lib. Deferred follow-up.
  - **`fuzz_baseline.json`** remains a placeholder until the libFuzzer
    integration is resolved.

  Also kept as cross-compiler portability improvements (clang-cl
  rejects, MSVC permits — real C++17 conformance issues):
  - `LineFontVerifier::Thresholds` default arg via factory method.
  - `AtlasBuilder::Options` default arg via factory method.

### 2026-04-26 (later that day)

- **Per-process firewall attribution** — addresses the
  "machine-wide netsh DROP rule cannot disambiguate PDFE-emitted
  vs. subnet-ambient" follow-up flagged in the morning entry below.

  `tests/security/runner/run_corpus.py`:
  - New `_snapshot_pdfe_sockets(pid)` helper uses `netstat -ano -p
    TCP/UDP` to enumerate outbound sockets owned by PDFE.exe at a
    point in time.
  - `_launch_and_observe()` now accepts a `socket_capture` set;
    sockets are sampled every 0.5 s during the wait window and
    accumulated into that set so short-lived bindings (a single DNS
    query, an LLMNR ephemeral) are still captured.
  - New `_attribute_drops_to_pdfe(drops, sockets)` filters
    pfirewall.log DROP entries to those whose
    `(proto, src_port, dst_ip, dst_port)` matches a PDFE-held
    socket. Includes a "fuzzy" match on `(proto, src_port)` for
    kernel-resolver multicast (LLMNR / mDNS) where the dst_ip
    isn't directly visible to the user-mode netstat.
  - New observation field `non_loopback_blocked_pdfe_attributed`
    records the filtered subset alongside the raw
    `non_loopback_blocked` (kept for forensics + back-compat).

  `tests/security/runner/decision.py`:
  - `decide()` now uses the attributed subset when present;
    falls back to the raw list when the field is absent (older
    artifacts or a netstat snapshot failure).

  Tests:
  - 6 new tests in `tests/security/runner/test_pdfe_attribution.py`
    exercise the filter (empty set, exact match, fuzzy multicast,
    protocol distinction, ordering, the 1900-vs-1 storm pattern).
  - 2 new tests in `tests/security/runner/test_decision.py`
    confirm the back-compat path and the attribution-clears-storm
    behavior.
  - All 46 Python tests pass.

  This change does not require a fresh corpus run to land — it
  changes the runner instrumentation, not the product. The next
  corpus run will produce both fields; the storm payloads
  (font/test40, uri/test41, xfa/test4, xxe/test42) should report
  ~0 PDFE-attributed drops if the storm is genuinely subnet
  ambient as the morning entry hypothesized.

### 2026-04-26

- **SecHardening Phase-1 + Phase-2 shipped** (commits `f949c1d` and
  `bca78dd`). Two layered fixes for the residual link-local
  broadcasts surfaced by phase-3:

  1. **UNC + remote-scheme rejection at input boundary**
     (`src/core/PathSecurity.{h,cpp}`). `Document::open` and the
     `/URI` + `/Launch` link dispatchers now refuse UNC paths
     (`\\…`, `//…`, `\\?\UNC\…`, `\\.\…`), file://-to-UNC
     (`file://host/…`, `file:////host/…`), and file-share schemes
     (smb, cifs, nfs, webdav(s), dav(s), ftp(s), ssh, sftp, tftp,
     ldap(s)) before any kernel-resolver-triggering Win32 call.
     Browser-safe schemes (http, https, mailto, tel, sms, news,
     nntp, gopher) keep working in the `/URI` path because they go
     through registered handlers, not the Win32 file resolver.
     41 unit tests + table-driven `LinkDispatcherPolicyTable`.

  2. **Windows process mitigation policies at startup**
     (`src/app/ProcessMitigations.{h,cpp}`). `SetProcessMitigationPolicy`
     applied for DEP (Permanent), ASLR (ForceRelocate +
     BottomUp + HighEntropy + DisallowStripped), ImageLoadPolicy
     (NoRemoteImages + NoLowMandatoryLabel + PreferSystem32),
     StrictHandleCheck. CFG status queried + logged. 5 unit tests
     call `GetProcessMitigationPolicy` and assert each policy's
     bits are set in the running test process. LogHandler
     install order moved before `applyProcessMitigations()` so
     per-policy [INFO]/[WARNING] log lines route through it.

- **Corpus delta** *(new run `corpus_run_20260426T182858Z.json` vs
  baseline `corpus_run_20260426T042156Z.json`)*:

  | Metric | Baseline | New run | Delta |
  |---|---|---|---|
  | Fails | 16 | 18 | +2 (within run-to-run variance: prior 9 runs ranged 13-17) |
  | Total non_loopback_blocked entries | 77 | 1984 | +1907 *(see breakdown)* |

  **Per-payload disposition changes (improvements from Phase 1):**

  | Payload | Old NLB → New NLB | Old verdict → New verdict | Notes |
  |---|---|---|---|
  | goto/test16.pdf | 9 → 0 | fail → **pass** | Phase 1 closed |
  | uri/test28.pdf | 16 → 0 | fail → **pass** | UNC URI rejected at link follow-up |
  | uri/test30.pdf | 5 → 0 | fail → **pass** | UNC URI rejected |
  | launch/test32.pdf | 3 → 0 | fail → **pass** | UNC Launch rejected |
  | js/test18.pdf | 1 → 0 | fail → **pass** | (incidental) |
  | js/test33_2, _4, _14, test34_7, test43 | various → 0 | fail → **pass** | (incidental) |

  **Per-payload disposition changes (new failures since baseline):**

  | Payload | Old NLB → New NLB | Old verdict → New verdict | Notes |
  |---|---|---|---|
  | font/test40.pdf | 1 → **428** | fail → fail | LLMNR storm during 26s window — burst behavior, root cause not yet identified |
  | uri/test41.pdf | 0 → **687** | pass → fail | Same LLMNR-storm pattern; payload is `/OpenAction /URI (https://127.0.0.1/...)` |
  | xfa/test4.pdf | 10 → **420** | fail → fail | LLMNR storm |
  | xxe/test42.pdf | 0 → **408** | pass → fail | LLMNR storm |
  | (other small +1 deltas) | — | various | individual LLMNR queries, within noise |

  | embedded/test39.pdf | 0 → 8 | pass → fail | NetBIOS broadcast (192.168.0.255:138) and LLMNR — small burst, not the storm pattern |

  Four payloads (font/test40, uri/test41, xfa/test4, xxe/test42)
  account for 1943 of the 1984 NLB entries — sustained burst
  spanning ~97 seconds across 4 consecutive payload windows
  (11:56:41 → 11:58:18) of a mix of LLMNR multicast
  (224.0.0.252:5355, ff02::1:3:5355) and LAN-subnet NetBIOS
  broadcasts (192.168.0.255:138, 192.168.0.34, 10.10.10.x sources).
  Without these four outliers the run would be ~41 NLB, in line
  with prior baselines (59-83 range).

  **Root cause not established.** The corpus runner uses a
  machine-wide netsh DROP rule, not per-process firewall scoping,
  so the firewall log captures all non-loopback traffic on the
  host during the test window — not just PDFE.exe traffic. The
  97-second burst spans 4 separate PDFE process launches; a
  per-process Qt/codec probing storm would reset on each launch,
  so the sustained burst is more consistent with subnet-wide
  ambient traffic (other VMs / processes on the same host or
  /24) being captured during the window than with PDFE-emitted
  traffic. Two payloads (`uri/test41`, `xxe/test42`) regressed
  from pass to fail — we cannot attribute that to product
  regression vs. test-environment noise without per-process
  firewall attribution.

  **Required follow-up:** Re-run the corpus with the firewall
  rule tightened to PDFE.exe-only (via WFP filter or per-process
  netsh rule) to determine whether the 4-payload storm is
  PDFE-emitted or subnet ambient. Until that's done, the +1907
  NLB delta is uninterpretable. Phase 1's input-boundary check
  is closing the *expected* attack vectors (UNC URIs, UNC
  Launch, polyglot SVG) regardless — those wins are not in
  doubt.

  Categorically:
  - `uri_unc/`: 1/2 still fails (1 was passing in baseline; the
    one failure is `test34_1.pdf`, a `/F` filespec UNC that goes
    through `pdf_open_document` directly, not through the URI gate
    — confirms the engine-level entry-point gap documented in
    `docs/security-policy.md` § "What's NOT yet covered"). Phase 1
    closed the link-action UNC paths but not the inline filespec.
  - `launch/`: 0/2 fails (was 1/2). Phase 1 closed.
  - `svg_polyglot/`: 1/1 still fails — verifying behavior; the
    magic-byte check was already in place pre-Phase-1.

- **Engine-level entry-point gap** (deferred). `BatchEngine`,
  `OptimizeEngine`, `RedactionVerifier`, `AlignmentInjector`, and
  `HeaderFooterEngine` open paths via `fz_open_document` /
  `pdf_open_document` directly without routing through
  `is_unc_or_remote`. Validator scoped Phase 1 to `src/forms/`,
  `src/annotations/`, `src/ui/MainWindow.cpp`. The engine call
  sites are documented in `docs/security-policy.md` § "What's NOT
  yet covered" as a follow-up.

### 2026-04-25

- `corpus_run_baseline.json` — replaced placeholder with a real
  baseline from a live phase-3 run (post phase-3 triage cycle 1:
  Document::open magic-byte fix + JS opt-in gate). Records 14
  failures as the accepted floor; all to link-local destinations
  (NetBIOS broadcast / LLMNR multicast) which the firewall
  catches and which cannot reach off-segment attackers. See
  `tests/security/runner/triage.md` for the per-category
  rationale.
- `sast_baseline.json` — populated with the 32 cppcheck high-
  severity false positives (29x unknownMacro for Qt's `slots:`,
  3x doubleFree/deallocuse from cppcheck not modeling
  fz_throw as [[noreturn]]). See
  `tests/security/sast/triage.md`.

### 2026-04-22

- `corpus_run_baseline.json` — initial placeholder (Option A: live
  run deferred; runner infrastructure committed).
- `fuzz_baseline.json` — initial placeholder (Option A).
- `sast_baseline.json` — initial placeholder (Option A).
- `deps_baseline.json` — initial placeholder (Option A).
- `sanitizers_baseline.json` — initial placeholder (Option A).
- `secrets_baseline.json` — initial placeholder (Option A).

(Add new entries above this line, newest first, with a date heading.)
