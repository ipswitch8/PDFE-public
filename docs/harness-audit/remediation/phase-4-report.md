# Phase-4 Report — SECURITY-ORACLE Remediation

Replaces security "theatre" (no-throw / catch-all oracles) with **positive
behavioral security oracles** that prove attacks are *neutralized* and
trust/tamper are *enforced*. Targets: R-01, R-12, R-13, R-22.

**Product source changed:** yes — exactly one file, 11 additive lines
(`src/signatures/Pkcs7OpenSslBridge.cpp`). No security behavior weakened.
**Genuine security bug found:** no exploitable "attack-not-neutralized" bug.
Two *fidelity gaps* were surfaced and documented (DocMDP level enforcement;
TSA best-effort) — neither is an exploit.

Touched suites: **all green** — 316/0 across the signature/compliance/
encryption/LTV/OCSP/cert/trust net (JUnit XML: `tests=316 failures=0
errors=0 skipped=0`). No test weakened, deleted, or skipped; only added /
strengthened.

New shared helper: `tests/security_oracles.h` (header-only, test-only).

---

## R-01 — CVE / malware neutralization (`test_cve_replay.cpp`, `test_malicious_pdf_advanced.cpp`)

**Old weak oracle:** every CVE replay / hostile-JS test wrapped `Document::open`
in `try { … } catch(...) {}` and asserted only *no-throw* + a time bound. A test
named `ProcessSpawnAttemptBlocked` asserted nothing about a process being
blocked; a swallow-all `catch(...)` hid any throw.

**New real oracles:**

1. **Process-spawn observer (`ProcessSpawnGuard`, `security_oracles.h`).** Snapshots
   the live set of attack-process image names (`calc.exe`, `cmd.exe`,
   `powershell.exe`, `mshta.exe`, `wscript.exe`, `rundll32.exe`, …) via
   `CreateToolhelp32Snapshot` *before* the operation, then asserts none newly
   appeared. Wired into:
   - `verifySafeOpen` — **every** CVE replay now asserts *no child process is
     spawned while the malicious PDF is opened/rendered*.
   - `JsApiSurfaceTest::runHostileJs` — **every** hostile-JS test asserts the
     script spawned no process. `ProcessSpawnAttemptBlocked`'s name is now backed
     by an observation.
2. **Swallow-all narrowed.** `verifySafeOpen`'s `catch(...)` → `catch(const
   std::exception&)`. A MuPDF/PDFE controlled rejection surfaces as a
   `std::exception` (the safe path); a non-`std` throw is no longer silently
   absorbed — it propagates and fails the test.
3. **JS-sandbox positive oracle** reused (js_sandbox_probe fixture,
   `fireFormatEvent`). The `/AA` + `/OpenAction` JavaScript CVEs
   (CVE-2021-30560, CVE-2024-4367) and the `XmlHttpRequestRejected` /
   `ProcessSpawnAttemptBlocked` tests now assert the escape APIs a hostile
   script would need (`System`, `Net`, `SOAP`, `Collab`, `app.openDoc`,
   `this.submitForm`, `this.getURL`, `util.readFileIntoStream`) are `undefined`
   — proving that *even if* the embedded action ran, it could not reach the
   OS/network/filesystem.

**Proof the oracle catches its threat:** the `ProcessSpawnGuard` was empirically
validated with a temporary test that `CreateProcessA`-spawns `cmd.exe` inside the
guarded window — `newlySpawned()` returned `"cmd.exe"` (test passed), proving it
is not a null oracle. Temporary test removed after verification. The sandbox
probe already fails if any escape API becomes a `function` (existing
`JsSandboxRealOracle` design).

---

## R-12 — signature trust-chain (`test_signature_roundtrip.cpp`)

**Old weak oracle:** the suite only asserted `digestValid` and *documented* the
certificate status ("self-signed → whatever MuPDF reports"). It never proved the
trusted-vs-untrusted distinction, and the verifier ignored the app's own
`TrustAnchorManager`, so no cert could ever report `Okay`.

**Product test-seam (the one src change):** `openssl_check_certificate` in
`Pkcs7OpenSslBridge.cpp` now calls `TrustAnchorManager::instance().loadIntoStore(store)`
*in addition to* `X509_STORE_set_default_paths(store)`. This mirrors what
`LtvValidator` already does. It is **purely additive**: the system default store
is still loaded, so nothing that verified before stops verifying; when the anchor
list is empty (the default) behavior is byte-for-byte unchanged. It only lets a
cert the user has **explicitly** trusted (Acrobat-style "trusted identities")
chain successfully — making the trusted/untrusted distinction real. This does not
weaken verification (it never *removes* a trust requirement); it closes a real
gap where the app managed trust anchors that the verifier ignored.

**New real oracles (both poles):**
- `UntrustedSelfSignedCertReportsNonOkay` — no anchor ⇒ `certificateValid=false`,
  `certificateStatus != Okay`, `!isFullyValid()`.
- `TrustedAnchorCertReportsOkay` — import the signer's own leaf cert into
  `TrustAnchorManager` ⇒ `certificateStatus == Okay`, `certificateValid=true`.
- `P1CertificationFlagsDisallowedContentChange` — certify at P1 (NoChanges),
  establish the `/DocMDP /P 1` context, then a content-byte edit is **flagged by
  the verifier** (`digestValid=false`, `!isFullyValid()`) — behavioral, not a
  byte-grep.

**Proof:** the two poles are mutually exclusive by construction — if the seam were
a no-op, `TrustedAnchorCertReportsOkay` fails (cert stays non-Okay); if trust were
always granted, `UntrustedSelfSignedCertReportsNonOkay` fails. Both pass ⇒ the
distinction is real and driven by the anchor.

**Fidelity gap surfaced (documented, not a vuln):** MuPDF reports a *fresh* P1
certification as `modifiedAfterSigning=true` (the `/Perms /DocMDP` dict is an
incremental update after the byte range), and PDFE does **not** differentially
enforce DocMDP P1/P2/P3 levels — it reports any-incremental-change uniformly and
gates disallowed *content* edits via the digest. The DocMDP oracle therefore uses
the robust digest axis. This level-fidelity gap is a certification-UX limitation,
not an exploit.

---

## R-13 — signature tamper / revocation (`test_signature_security_phase2.cpp`, `test_ocsp_crl_validation.cpp`)

**Live bug fixed (test):** `test_signature_security_phase2.cpp:310` had
`f.write(&b == 0 ? "\x01" : "\x00", 1);`. C++ parses `&b == 0` as *"is the address
of a stack local null?"* — always false — so it unconditionally wrote `"\x00"`.
When the target byte was already `0x00`, the "tamper" was a **no-op** and the
oracle then "verified" an untampered file (false pass). Replaced with a guaranteed
XOR-flip (`b ^ 0xFF`, asserted `!= b`) and strengthened the assertion to
`!digestValid && !isFullyValid()`.

**Revocation-freshness de-masked (new oracle):** every prior OCSP test ran with
`useTrustAnchors=false`, so the chain was always `UntrustedChain` and "not
LtvValidated" could mean *untrusted* OR *stale* — freshness enforcement was
masked. `StaleOcspRejectedEvenWhenChainIsTrusted` **explicitly trusts** the signer
(adds its cert to `TrustAnchorManager`), then proves the *only* difference between
a fresh-GOOD OCSP (→ `LtvValidated`) and a stale-GOOD OCSP (→ *not*
`LtvValidated`) is freshness. Revocation-independent-of-trust is already covered by
`StaleRevokedOcspStillHonoured` (Revoked honored with `useTrustAnchors=false`).

**Proof:** with trust held constant (signer trusted), the fresh case reaches
`LtvValidated` and the stale case does not — isolating the freshness check as the
discriminator. If `OCSP_check_validity` staleness rejection were removed, the
stale case would also reach `LtvValidated` and the test fails.

**Residuals (documented, not delivered):**
- *RFC-3161 TSA validation* (`test_tsa_real_endpoint`) still `NOTE`s the OID rather
  than hard-asserting. A hard assertion cannot be added without introducing
  network flakiness: the product treats TSA as **best-effort / non-fatal by
  design** (`Pkcs7OpenSslBridge::embedTimestampToken`), so the token is legitimately
  absent when the DigiCert endpoint is unreachable/firewalled. Making presence a
  hard assert would fail offline runs. Recommended follow-up: a *test-only* offline
  RFC-3161 responder fixture so the timestamp token can be asserted deterministically.
- *Forged-GOOD OCSP from an unauthorized responder* (OCSP signed by a rogue key):
  not added this phase. Recommended follow-up: generate a rogue self-signed
  responder key, sign a GOOD OCSP for the real cert's `certId`, and assert
  `verifyWith(useTrustAnchors=true)` does **not** reach `LtvValidated`.

---

## R-22 — prohibited-feature scanner (`test_prohibited_feature_scanner.cpp`)

**Old weak oracle:** only proved a *clean* doc passes — a scanner that
unconditionally returned "no prohibited feature" (the null oracle) passed every
test.

**New positive-detection oracles:**
- `HasEncryption_DetectsEncryptedDoc` — reuse committed `test_encrypted_aes256.pdf`
  (opened with password) ⇒ `hasEncryption()==true` + `scanAll` emits rule `6.1.3`.
- `HasJavaScript_DetectsNamesJavaScript` + `HasJavaScript_DetectsOpenActionJs` —
  inline PDFs with `/Names /JavaScript` and `/OpenAction /S /JavaScript` ⇒
  `hasJavaScript()==true` + rule `6.6.1`.
- `HasExternalReferences_DetectsGoToR` — inline PDF with a `/GoToR` annotation
  action ⇒ `hasExternalReferences()==true` + rule `6.6.3`.
- `PdfaidReadFromNodeNotSubstring` — an XMP packet with a **decoy**
  `pdfaid:part 3 conformance A` string inside `dc:description` preceding the real
  `pdfaid:part="1"/conformance="B"` attributes. `detectPdfALevel` must return
  **PDF/A-1b** (node read), not PDF/A-3a (substring read).

**Proof:** each detection test asserts `true`/a specific rule on a *known-positive*
fixture — if the detection logic regressed to "always false", these fail by
construction. The pdfaid test fails iff the reader regresses from node-based to
substring-based (the decoy would then win → PDF/A-3a ≠ asserted PDF/A-1b). Verified
green; the clean-doc negative tests remain green (no false positives).

---

## Fixtures

No **new committed** fixtures were added, so no `CMakeLists.txt` fixture-floor or
`fixtures/README.md` entries are required:
- Encryption reuses the existing `test_encrypted_aes256.pdf`.
- JS / external-ref / pdfaid-decoy PDFs are generated **inline** into the temp dir.
- Trust anchors are derived at runtime from the existing `test_signer.pfx`.
- The JS-sandbox oracle reuses the already-registered `js_sandbox_probe.pdf`.

## Product-source change (git status -- src/)

```
 M src/signatures/Pkcs7OpenSslBridge.cpp   (+11 lines, additive)
```
`openssl_check_certificate` additionally loads user `TrustAnchorManager` anchors
into the verification `X509_STORE`. Empty anchor list (default) ⇒ unchanged
behavior; system default store still loaded ⇒ nothing that verified before stops
verifying. Regression-checked: 316/0 across the full signature/compliance/
encryption/LTV/OCSP/cert/trust suite net.
