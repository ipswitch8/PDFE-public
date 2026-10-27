# PDFE Security Review Cadence

<!-- tracking-banner -->
> **STATUS: HISTORICAL EVIDENCE — this is not a work list.**
>
> **GitHub Issues (`ipswitch8/PDFE`) is the canonical tracker for anything still
> open.** This document records what was found or decided on the day it was
> written. Items here may have been fixed, superseded, or deliberately declined
> since. Where this document and a GitHub issue disagree, **the issue wins**.
>
> See [`docs/TRACKING.md`](../../docs/TRACKING.md).

---


**Purpose:** Establish a recurring schedule for security activities so
that drift, new CVEs, and dependency vulnerabilities are caught early.

---

## Quarterly (every 3 months)

### Q1: Threat Model Review
- **Owner:** Security lead + Architecture lead
- **Output:** Updated `tests/docs/threat_model.md` (if it exists) or
  new threat model doc
- **Activities:**
  - Walk through STRIDE per major component (Document, Renderer, Forms,
    Signatures, JS engine, Save pipeline)
  - Identify new attack surface from features added in the prior
    quarter
  - Update mitigation status: which CVEs apply, which are covered
    by existing tests, which need new tests

### Q2: Dependency CVE Scan
- **Owner:** Build engineer
- **Tools:** `vcpkg audit`, `npm audit` (if any JS tooling),
  `pip-audit` for Python test tooling, GitHub Dependabot alerts
- **Output:** Tickets filed for each Critical/High CVE in:
  - MuPDF
  - OpenSSL
  - Qt
  - FreeType
  - HarfBuzz
  - libpng / libjpeg-turbo / libtiff / libopenjp2
  - zlib
  - SQLite
  - pugixml
  - ONNX Runtime
  - Tesseract / Leptonica
- **Action:** Patch within SLA — Critical: 7 days, High: 30 days,
  Medium: 90 days

### Q3: Fuzz Coverage Review
- **Owner:** Test engineer
- **Activities:**
  - Review nightly fuzz campaign findings (artifacts uploaded by
    `.github/workflows/fuzz.yml`)
  - Triage any new crashes; file tickets
  - Minimize corpus (remove redundant inputs, keep coverage)
  - Expand harnesses if new file types or parsers added

### Q4: External Security Activity Review
- **Owner:** Security lead
- **Activities:**
  - Review NVD/CVE for new PDF-related vulnerabilities in the
    quarter
  - Add CVE replays to `tests/test_cve_replay.cpp` for new public
    PoCs that map to PDFE's surface
  - Cross-reference with `tests/docs/quality_gaps_plan.md`

---

## Annually

### External Penetration Test
- **Cadence:** Once per year (preferably Q3 before annual planning)
- **Scope:** Black-box and grey-box testing of:
  - Open arbitrary PDF (parser exploits)
  - Signature verification (cryptographic and chain spoofing)
  - JavaScript sandbox (escape attempts)
  - Network operations (TSA, OCSP, embedded resources)
  - Redaction (residual content recovery)
- **Output:** Penetration test report; tickets for findings;
  remediation tracked through next quarter's reviews

### Sanitizer Build Refresh
- **Cadence:** Once per year (e.g., Q1)
- **Activities:**
  - Update clang/sanitizer version in `.github/workflows/sanitizer.yml`
  - Run with `-fsanitize=memory` (MSan) in addition to ASan+UBSan
    on a long-running campaign (24+ hours)
  - Add `-fsanitize=thread` (TSan) run if any concurrency changes

### Compliance / Regulatory Review
- **Activities:**
  - FIPS 140 status of cryptographic primitives (signature, encryption)
  - GDPR/PII handling review (does PDFE log paths, content, or
    metadata that could contain PII?)
  - Section 508 / WCAG accessibility audit

---

## Triggered (event-based, not scheduled)

### After Each Major Feature Merge
- Threat model delta review by the feature author
- Fuzz harness extension if the feature opens new parser code
- Update `tests/docs/manual_qa_checklist.md` if user-facing surface
  changed

### After Each Disclosed PDF CVE
- Within 7 days: assess whether PDFE is affected (which library,
  which code path, which versions)
- Within 30 days: ship a fix or document why PDFE isn't vulnerable
  (different code path, alternative library, mitigations in place)
- Add CVE-specific test to `tests/test_cve_replay.cpp` so it remains
  in the regression set

### After Each Production Incident
- Root cause analysis
- "Did our tests catch this? If not, why?" review
- Add regression test that would have caught it

---

## Metrics to Track

Quarterly snapshots in `tests/docs/security_metrics.md`:

| Metric | Target | Source |
|---|---|---|
| Total security tests passing | ≥95% of suite | gtest run |
| CVE replay corpus size | Growing each quarter | `tests/test_cve_replay.cpp` |
| Fuzz hours per harness per quarter | ≥40 hr | GitHub Actions logs |
| Dependency CVEs unresolved past SLA | 0 | vcpkg audit + tracking |
| Days since last external pen test | <365 | Calendar |
| Manual QA checklist runs per release | 100% | Release docs |

---

## Sign-Off Template (each quarter)

| Quarter | Review type | Date | Lead | Findings | Tickets |
|---|---|---|---|---|---|
| 2026-Q3 | Threat model | | | | |
| 2026-Q4 | Dep CVE scan | | | | |
| 2027-Q1 | Fuzz coverage | | | | |
| 2027-Q2 | External activity | | | | |

---

## OWASP Attack-Category Coverage Matrix (GAP-X7)

Maps each OWASP-relevant attack category applicable to a PDF editor to at
least one specific automated test. All cited suites PASSED in the gtest run
`build/.../gtest_phase7_final.xml` (2026-06-14, 3926 tests, the only 2
failures were the unrelated ZoomComboExists flake and the now-fixed
OpenSslTlsPresent deploy test — zero failures among security suites). The
"latest sanitizer run" column is refreshed by the **scheduled**
`windows-sanitizer.yml` run on master HEAD — `schedule: '0 18 * * 0,2,4'`
(Sun/Tue/Thu 18:00 UTC, max 3-day gap) plus `workflow_dispatch`. That lane has
had **no `push` trigger since 2026-07-20**; this text said "post-push" until
F32 corrected it, so the refresh is bounded by the 3-day cadence, not by the
next commit.

| OWASP-relevant category | Covering test(s) | File | Status |
|---|---|---|---|
| Malformed / hostile input parsing | `MaliciousPdfTest.*`, `MaliciousPdfAdvancedTest.*`, CVE replay corpus | `tests/test_malicious_pdf*.cpp`, `tests/test_cve_replay.cpp` | PASS |
| Path traversal (JS `openDoc`, attachment extract) | `JsSandboxSecurityTest.OpenDocPathTraversalBlocked`, `AttachmentSanitize.*` | `tests/test_form_javascript_xss.cpp`, attachment tests | PASS |
| Embedded JavaScript escape / sandbox bypass | `JsSandboxSecurityTest.{LaunchURLBlockedInKeystroke, SystemOsNotAccessible, FieldValueInjectionSafe, WillSaveHostileScriptSafe, InfiniteLoopInCalculateDoesNotHang}` | `tests/test_form_javascript_xss.cpp` | PASS |
| Encryption bypass / weak-crypto | `EncryptionRoundTripTest.*`, `EncryptionManagerTest.*`, `RedactionSecurityTest.*` | `tests/test_encryption*.cpp`, `tests/test_redaction*.cpp` | PASS |
| Signature forgery / verification bypass | `SignatureSecurityTest.*`, `CertEnforcementTest.*`, `Pkcs7BridgeTest.*`, `SignatureVerifyResultTest.*` | `tests/test_signature*.cpp`, `tests/test_pkcs7*.cpp` | PASS |
| Redaction completeness (data-leak after redaction) | `RedactionCompletenessTest.*`, `RedactionVerifierTest.*`, `RedactionRoundTripTest.*` | `tests/test_redaction*.cpp` | PASS |
| Unicode / non-ASCII path handling | `UnicodeFilename*` (see PDFE-LIMITATION-UNICODE-PATH RESOLVED) | `tests/test_security_audit.cpp` | PASS |

**GAP-X7 status:** category→test mapping consolidated here (was previously
scattered across test_security_audit.cpp / test_malicious_pdf*.cpp /
test_form_javascript_xss.cpp / tests/security/). Each category has ≥1 passing
test. Verification against the current-HEAD sanitizer run is performed by the
**scheduled** `windows-sanitizer.yml` gate (Phase 8 criterion 2) —
`schedule: '0 18 * * 0,2,4'` + `workflow_dispatch`, no `push` trigger since
2026-07-20. Worst case, the matrix is verified against a HEAD up to 3 days old.
