# Code-signing / no-auto-update posture — risk acceptance (R3-16)

<!-- SIGNING-STATUS: SIGNED-MANUAL-ONLY -->
<!-- SIGNING-STATUS-COMMIT: 92e1328 -->

> **Filename note.** This file is still called `unsigned-binary-risk-acceptance.md`
> because the audit registers, the status register and `src/app/VersionInfo.h`
> cite it by that path. The *name* is historical; the content below is the
> current posture. Do not read the filename as a status claim.

**Status:** Release artifacts **are Authenticode-signed** (since `92e1328`,
2026-07-18). The residual accepted risk is narrower than the original finding:
signing is a **manual release step, not an automated gate**, and there is still
no auto-update channel.
**Finding:** R3-16 — originally raised when the shipped binary/installer was not
Authenticode-signed and there was no auto-update or in-app version-check channel.
**Owner:** PDFE maintainers.
**Last reviewed:** 2026-08 (meta-audit follow-up F7 — WIN-92/WIN-94/WIN-95).

---

## 1. Current posture (the facts)

**Signing status: SIGNED, but manual-only.**

- Release artifacts — `pdfe.exe`, `pdfe_preflight.exe` and the NSIS installer
  `PDFE-<version>-win64.exe` — are signed with **Azure Trusted Signing** under
  the publisher identity **Itwerx Corp**. The chain is Microsoft-rooted
  (Itwerx Corp → Microsoft ID Verified CS EOC CA 04 → Microsoft ID Verified Code
  Signing PCA 2021 → Microsoft Identity Verification Root CA 2020) and every
  signature carries an **RFC-3161 countersignature** from Microsoft's timestamp
  authority. This landed in commit `92e1328` ("release(sign): code-sign v0.1.0
  as Itwerx Corp (Azure Trusted Signing)", 2026-07-18) together with the
  `CompanyName`/`CPACK_PACKAGE_VENDOR` metadata alignment.
- **Verified 2026-08-01** on `build/vs2022/installer/PDFE-0.1.0-win64.exe`:
  `Get-AuthenticodeSignature` reports `Status = Valid`,
  signer `CN=Itwerx Corp, O=Itwerx Corp, L=Seattle, S=Washington, C=US`,
  timestamper `CN=Microsoft Public RSA Time Stamping Authority`.
- **Signing is NOT automated.** No workflow under `.github/workflows/` invokes
  any signing tool — `signtool`, `Sign-Artifacts.ps1` or
  `Build-Release-Signed.ps1` — and none verifies a signature either. A release is
  signed only because a human ran
  `scripts/sign/Build-Release-Signed.ps1`; skipping that step ships an unsigned
  artifact green. This is not hypothetical — the stale copy of the v0.1.0
  installer at the repository root is unsigned, while the CPack-produced one
  under `build/vs2022/installer/` is signed.
- There is **no auto-update mechanism**. The version-check *core* has been added
  (see §4); the network fetch of the "latest available" version is still future
  work.

The signing tooling:

- `docs/code-signing.md` — Azure Trusted Signing setup and rationale.
- `scripts/sign/Build-Release-Signed.ps1` — end-to-end signed-release build
  (signs binaries before packaging, signs the installer after, verifies every
  signature, prints the installer SHA-256).
- `scripts/sign/Submit-SmartScreen.ps1` — SmartScreen reputation submission.
- `scripts/vm-install-residue-harness.ps1` — the verification side: its
  signature stage runs `Get-AuthenticodeSignature` over the shipped binaries and
  the installer and fails unless `Status` is `Valid` **and** an RFC-3161
  countersignature is present. `-SignatureOnly` runs that stage alone, read-only,
  with no install and no elevation.

The remaining gap is **automation**, not a certificate.

## 2. Concrete risks being accepted

*(Re-scoped 2026-08 now that signing is in place. The tamper-evidence and MITM
rows are no longer accepted risks for a correctly-signed release — they are
accepted only for the failure mode where the manual signing step is skipped.)*

| Risk | Impact | Likelihood |
|---|---|---|
| **Signing is a manual step with no gate** — a release can be published unsigned because nobody ran `Build-Release-Signed.ps1` | Reintroduces every row below for that release, silently and invisibly | Medium — no CI check enforces it; the unsigned repo-root copy of the v0.1.0 installer shows the step is skippable |
| **SmartScreen / Defender "unrecognized app" warning** on download and first run | Users see a prompt; some abandon the install | Medium — the *unsigned* penalty is gone, but Itwerx Corp is a new publisher and reputation accrues with download volume |
| **No tamper-evidence on an unsigned release** — a modified `pdfe.exe`/installer could not be distinguished from the genuine one by the OS | A supply-chain or MITM attacker could substitute a trojaned build with no identity signal | Low — only reachable through the manual-step failure above |
| **MITM on download of an unsigned release** — transport (HTTPS) and the published hash would be the only protection | A compromised mirror or hijacked link could serve a malicious binary | Low over HTTPS + published hash |
| **No auto-update** — users are not notified of, and are not automatically moved to, patched releases | Known-vulnerable versions linger in the field | Medium (mitigated by the version-check core in §4 once a feed is wired) |

## 3. Mitigations in place

- **Authenticode signing with an RFC-3161 countersignature** on every release
  artifact, Microsoft-rooted via Azure Trusted Signing (§1). This is the primary
  mitigation and it is in place.
- **A verification harness** that fails on `Status -ne 'Valid'` or a missing
  countersignature (`scripts/vm-install-residue-harness.ps1`, signature stage),
  runnable read-only as `-SignatureOnly`.
- **Published SHA-256** for every release artifact. `Build-Release-Signed.ps1`
  prints the installer SHA-256; the release notes/download page publish it so a
  user can verify the download out-of-band.
- **HTTPS-only distribution** of release artifacts (GitHub Releases), which
  provides transport integrity and authenticity of the download endpoint.
- **Hash pinning of build/security tooling** (R2-27): `scripts/security/
  install_tools.ps1` verifies downloaded tool binaries against pinned SHA-256
  values rather than trusting whatever the endpoint serves, reducing
  supply-chain exposure in the build environment itself.
- **Process-hardening of the app** (unrelated to signing but reduces the blast
  radius of a hostile document): out-of-process preflight open, process
  mitigations (`src/app/ProcessMitigations.cpp`).
- **Version-check core** (§4): the comparison logic an update channel needs,
  landed and unit-tested, so wiring a release-feed fetch later is incremental.

## 4. Partial technical mitigation — version-check core

`src/app/VersionInfo.{h,cpp}` (in `pdfe_app`) provides:

- `currentVersion()` — the running build's version, sourced from the single
  source of truth `project(pdfe VERSION 0.1.0 ...)` (top-level `CMakeLists.txt`),
  threaded to C++ via the `PDFE_VERSION_STRING` compile definition with a
  literal fallback held in lock-step.
- `compareVersions(current, latest)` → `UpdateStatus{UpToDate, UpdateAvailable,
  Newer}` — pure semantic-version comparison (zero-padded segment counts,
  leading-zero-insensitive, `v`-prefix and pre-release/build-metadata tolerant,
  non-numeric-guarded).
- `checkForUpdate(latestFromChannel)` — wraps the above for a supplied "latest"
  string.

This is the decision core of an update/version-check channel. Deliberately
**out of scope here** (documented future work): fetching the "latest available"
version from a signed release feed over HTTPS, the update-notification UI, and
any download/apply mechanism. Keeping the fetch out of `VersionInfo` keeps the
logic pure and fully unit-testable with no network dependency
(`tests/test_version_update.cpp`).

## 5. Accepted-risk rationale

For the current pre-1.0, low-volume, open-source (AGPL-3.0) distribution:

- Signing itself is **no longer** an accepted risk: an identity was provisioned
  and every release artifact is signed and RFC-3161 countersigned (§1).
- What *is* accepted is that signing runs as a **manual release step** rather
  than an enforced gate. At the current cadence (releases are cut by hand, one
  so far) a human checklist step is proportionate; the harness in §3 makes the
  check one command rather than a judgement call.
- The dominant remaining user-visible cost is the SmartScreen prompt for a new
  publisher, which is a reputation/UX issue rather than a code-execution
  vulnerability. Users obtain the binary over HTTPS from a known GitHub Releases
  page and can verify the published SHA-256.

The residual risk accepted here is therefore *"a release could be shipped
unsigned because the manual step was skipped"*, mitigated by the verification
harness and revisited at the trigger points below.

## 6. Path to closing the risk

1. **Automate the signing gate** — the outstanding item. Wire
   `scripts/sign/Build-Release-Signed.ps1` into the release workflow and add a
   release-gate step running the harness signature stage
   (`scripts/vm-install-residue-harness.ps1 -SignatureOnly`) so an unsigned or
   un-countersigned artifact fails the release rather than shipping green.
   Until that exists, this document records the status as
   **SIGNED-MANUAL-ONLY** and the doc test in
   `tests/test_signing_and_install_docs.cpp` holds the claim to reality by
   failing if `.github/workflows/` ever starts (or stops) signing.
2. **Update channel** — stand up a signed release feed (e.g. a JSON manifest of
   the latest version + artifact hashes served over HTTPS), fetch it, and feed
   the result into `checkForUpdate()`; surface `UpdateAvailable` in-app.

## 7. Re-evaluation triggers

Revisit this acceptance if any of the following occur:

- A `1.0` / broadly-promoted public release is planned.
- Download volume grows enough that SmartScreen friction materially affects
  adoption.
- Distribution moves to any channel that is not HTTPS-with-published-hash.
- Any tamper or substitution incident is reported.
- The signing certificate/profile changes, expires, or the Trusted Signing
  account is re-provisioned.

## 8. Status history

| Date | Commit | Status | Note |
|---|---|---|---|
| 2026-07 (v0.1.0 planning) | — | not signed | Original R3-16 finding: no Authenticode identity provisioned. |
| 2026-07-18 | `92e1328` | **signed** | Azure Trusted Signing, publisher Itwerx Corp, Microsoft-rooted chain, RFC-3161 timestamped; v0.1.0 release asset replaced with the signed installer. |
| 2026-07-24 | `7903f37` | signed (doc stale) | This document was created in that commit still describing the unsigned posture — six days after signing landed. That contradiction is gap `WIN-92`/`WIN-98`. |
| 2026-08-01 | this change | **signed, manual-only** | Document and `src/app/VersionInfo.h` reconciled with reality; the manual-vs-automated nuance recorded; `tests/test_signing_and_install_docs.cpp` added so the two files cannot drift apart again. |
