# Code signing & SmartScreen

PDFE release binaries are signed with **Azure Trusted Signing** (Microsoft's
managed code-signing service, formerly "Azure Code Signing"). Trusted Signing
issues short-lived, Microsoft-rooted Authenticode certificates and timestamps
each signature from Microsoft's RFC-3161 authority — so signed PDFE binaries
chain to a Microsoft-operated CA, which is the strongest baseline for Windows
SmartScreen and Defender.

> **Why signing matters:** an unsigned installer trips SmartScreen's
> "unrecognized app" warning on download/run. Signing removes the *unsigned*
> penalty and lets the publisher accrue reputation. Reputation then builds from
> two things: the signature (identity) and real-world download volume over time.

---

## What gets signed

| Artifact | Signed? | Why |
|---|---|---|
| `pdfe.exe`, `pdfe_preflight.exe` | ✅ before packaging | so the *installed* app is signed |
| `PDFE-<version>-win64.exe` (NSIS installer) | ✅ after packaging | the file users actually download and run |
| Third-party DLLs (Qt, MuPDF, OpenSSL, Tesseract, …) | ❌ left as-is | already vendor-signed; re-signing is unnecessary and undesirable |

### Hash-pinned redistributables (GitHub #72)

Most third-party DLLs arrive through vcpkg, which does its own integrity
checking. **ONNX Runtime does not.** It is too large to build from source via
vcpkg, so it is downloaded as a pre-built release archive into `external/`,
which is `.gitignore`d — meaning nothing in the repository recorded which bytes
were being shipped.

That mattered more than an ordinary dependency, because `onnxruntime.dll` is
`install()`ed into the package: it goes out **inside an installer we sign**. Our
signature asserts the contents came from us. Until this was pinned, the build
selected those bytes on an `EXISTS` probe alone — and on the self-hosted runner
`external/` survives between jobs, so anything that once wrote there would have
been picked up silently by every later build, including a release build.

`CMakeLists.txt` now verifies three SHA-256 digests at configure time:

| File | Role |
|---|---|
| `lib/onnxruntime.lib` | import library — linked |
| `lib/onnxruntime.dll` | **redistributed in the signed installer** |
| `lib/onnxruntime_providers_shared.dll` | **redistributed in the signed installer** |

A mismatch is a **`FATAL_ERROR`, not a downgrade to `PDFE_HAS_ONNXRUNTIME=OFF`**.
Downgrading would be the worse outcome: the build would go green having quietly
dropped the CNN font re-ranker because someone tampered with a DLL. Absent is
still a supported state — the feature is optional and simply turns off — but
present-but-wrong is refused.

Verified by tampering: flipping a single bit in `onnxruntime.dll` fails the
configure step with the expected/actual digests named.

---

## One-time Azure setup

Done once by an Azure admin (see Microsoft's Trusted Signing quickstart):

1. **Register the resource provider:** `az provider register -n Microsoft.CodeSigning`
2. **Create a Trusted Signing account** (pick a region → that region is your
   endpoint host, e.g. `eastus` → `https://eus.codesigning.azure.net/`).
3. **Complete identity validation** (public or private) for the account.
4. **Create a certificate profile** (`Public Trust` for public software).
5. **Grant the signing identity** (your user, or a CI service principal) the
   **"Artifact Signing Certificate Profile Signer"** role on the account
   (Microsoft renamed the older "Trusted Signing Certificate Profile Signer").

You then have three values the scripts need:

| Value | Example | Env var |
|---|---|---|
| Endpoint | `https://eus.codesigning.azure.net/` | `TRUSTED_SIGNING_ENDPOINT` |
| Account name | `pdfe-signing` | `TRUSTED_SIGNING_ACCOUNT` |
| Certificate profile | `pdfe-public` | `TRUSTED_SIGNING_PROFILE` |

None of these are secrets, but the filled-in local config is `.gitignore`d
anyway. **Auth** is separate and never stored in the repo.

---

## Authentication

Signing uses Azure `DefaultAzureCredential`, resolved in this order:

- **Local dev box:** `az login` (interactive). *(As of this writing the cached
  token on the build host has expired — run `az login` before signing.)*
- **CI:** a service principal via env vars
  `AZURE_TENANT_ID` / `AZURE_CLIENT_ID` / `AZURE_CLIENT_SECRET`
  (the SP needs the *Certificate Profile Signer* role above).
- **Azure-hosted runners:** managed identity.

---

## How to build a signed release

```powershell
az login                                             # or set AZURE_* SP env vars in CI
$env:TRUSTED_SIGNING_ENDPOINT = "https://eus.codesigning.azure.net/"
$env:TRUSTED_SIGNING_ACCOUNT  = "pdfe-signing"
$env:TRUSTED_SIGNING_PROFILE  = "pdfe-public"

# one command: build -> sign exes -> pack -> sign installer -> verify + SHA-256
./scripts/sign/Build-Release-Signed.ps1
```

**Which build tree this signs (GitHub #182).** `Build-Release-Signed.ps1`
defaults to `build/vs2022-package` — configured by
`cmake --preset windows-vs2022-package`, which sets `PDFE_TESTING=OFF` and so
compiles out the test-only introspection seams (`PDFE_TEST_PIPES`). It
configures that tree for you if it is missing. It is **not** `build/vs2022`:
that is the tree `pdfe_tests.exe` uses and it has the seams compiled in. The
script reads `PDFE_TESTING:BOOL=` out of the target tree's `CMakeCache.txt`
before packaging and refuses to continue unless it is `OFF`, so passing
`-BuildDir build/vs2022` is a hard stop rather than a quiet downgrade. Every
signed release before this change, v0.2.0 included, was built from the ON tree.

To sign arbitrary files directly:

```powershell
./scripts/sign/Sign-Artifacts.ps1 -Files build/vs2022-package/installer/PDFE-<version>-win64.exe
```

Both scripts auto-install the [`sign`](https://github.com/dotnet/sign) .NET
global tool (requires the .NET SDK, already present) and verify the result with
`signtool verify /pa /v`.

### Alternative: signtool + Trusted Signing dlib

If you prefer the classic path instead of the `sign` tool:

1. `dotnet tool install --global sign --prerelease` **or** restore the
   `Microsoft.Trusted.Signing.Client` NuGet package to get
   `Azure.CodeSigning.Dlib.dll`.
2. Copy `scripts/sign/trusted-signing-metadata.example.json` →
   `trusted-signing-metadata.json` and fill in Endpoint / account / profile.
3. ```
   signtool sign /v /fd SHA256 /tr http://timestamp.acs.microsoft.com /td SHA256 ^
     /dlib "<path>\Azure.CodeSigning.Dlib.dll" ^
     /dmdf trusted-signing-metadata.json ^
     "PDFE-<version>-win64.exe"
   ```

---

## SmartScreen submission

Script: `scripts/sign/Submit-SmartScreen.ps1`.

> **Reality check.** Microsoft's file-submission portal
> (<https://www.microsoft.com/en-us/wdsi/filesubmission>) is **interactive**
> (Microsoft-account sign-in + web upload) and is a **dispute / allow-list**
> channel — "this file is being *incorrectly* flagged" — not a general
> "pre-approve my installer" API. There is no unauthenticated public submission
> endpoint, so this step cannot be fully headless.

Recommended flow:

1. Ship the **signed** installer (GitHub release). Let it accrue downloads.
2. **If** a signed release is still SmartScreen-warned in the wild, run:
   ```powershell
   ./scripts/sign/Submit-SmartScreen.ps1 -Installer build/vs2022-package/installer/PDFE-<version>-win64.exe
   ```
   It verifies the signature, writes a `*.smartscreen-submission.json` with the
   SHA-256 + signer details, prints a copy-paste block, and opens the portal.
3. On the portal choose **"Software developer"**, upload the installer (or give
   the public release-asset URL), and reference the details from the script.

The durable fix is the signature + volume; the portal is the escalation path.

---

## Update channel security requirements (normative)

<!-- UPDATE-CHANNEL-STATUS: NOT-IMPLEMENTED -->
<!-- UPDATE-CHANNEL-DECISION: DOCUMENTED-NON-FEATURE -->
<!-- VERSION-NOTIFICATION-STATUS: OPT-IN-NOTIFY-ONLY -->

**Current state — a documented non-feature, not a regression.**
PDFE has no update channel.
`src/app/VersionInfo.h` provides only the *pure* comparison core
(`compareVersions`, `checkForUpdate`) with no I/O; the "Check for Updates" menu
action in `src/ui/MainWindow.cpp` is a `QDesktopServices::openUrl` to the
releases page. Nothing in the product fetches, parses or applies a manifest, and
nothing downloads or executes a payload.

> **AMENDED 2026-09-06 (GitHub #127).** PDFE now has an **opt-in, notify-only
> version check** — and still has **no update channel**. The two are different
> things and the distinction is load-bearing, so it is stated normatively as
> **UC-9** below rather than left to a reader's judgement.
>
> What changed: `src/app/UpdateChecker.cpp` performs one HTTPS GET of the
> releases feed, reads a version string, and displays a notification. It is
> **off by default** (`AppSettings::updateCheckEnabled()`, default `false`), so
> a stock install still makes no such request.
>
> What did NOT change: nothing fetches, parses or applies a **manifest**, and
> nothing downloads or executes a **payload**. UC-1..UC-8 govern applying an
> update, because applying one executes attacker-reachable bytes. A
> notification executes nothing. The worst a hostile or spoofed response can do
> is display a wrong version number.
>
> The "Check for Updates" menu action is deliberately **still** a
> `QDesktopServices::openUrl` to the releases page. Installing remains an
> explicit human act performed through the browser, which is what keeps
> UC-1..UC-8 out of scope. Any change to that — any in-app download, any
> launched installer — makes them apply in full and is a different piece of
> work.
>
> Users were previously not notified of releases at all, including security
> releases. They now can be, if they choose to be.

**Why this section exists (gap WIN-91).** PDFE's release artifacts are
Authenticode-signed. Bolting an *unauthenticated* updater onto a signed
application would hand any network or DNS attacker a remote-code-execution
channel with the application's own privileges — a strictly worse security
posture than having no updater at all. The requirements below are therefore
recorded **before** the component exists, when they are cheapest to satisfy, and
are asserted by `tests/test_update_channel_docs.cpp` so they cannot be quietly
dropped when someone eventually builds the updater.

These are requirements on any future implementation. They are normative: an
update channel that does not satisfy every one of them must not ship.

| id | requirement |
|---|---|
| **UC-1** | **The update manifest MUST be signature-verified before any field of it is acted upon.** The manifest carries a detached signature (or is itself Authenticode-signed) over its exact bytes, verified against a publisher key pinned in the binary. An unsigned, malformed, or unverifiable manifest MUST be discarded without parsing its version, URL or digest fields for action. |
| **UC-2** | **The downloaded payload MUST be verified twice**: its SHA-256 MUST equal the digest recorded in the *signed* manifest, **and** it MUST pass Authenticode verification (`Status -eq 'Valid'` plus a non-null RFC-3161 `TimeStamperCertificate`, the same two conditions `scripts/vm-install-residue-harness.ps1` enforces for releases). Both checks MUST complete before the payload is executed. |
| **UC-3** | **Transport security is not authentication.** The fetch MUST use HTTPS with full certificate-chain validation, but a successful TLS handshake MUST NOT be accepted as evidence that the manifest is authentic. UC-1 is not waived for any host, including a first-party one. |
| **UC-4** | **Rollback and downgrade protection.** A manifest advertising a version less than or equal to the running version MUST NOT trigger an update, and a manifest older than the newest one already seen (monotonic sequence number or signed timestamp) MUST be rejected, so a captured older-but-validly-signed manifest cannot be replayed to push a known-vulnerable build. |
| **UC-5** | **Key rotation and revocation.** At least two publisher keys MUST be pinned so a compromised key can be retired without bricking the channel, and revocation state (CRL/OCSP) MUST be consulted when verifying the payload signature. |
| **UC-6** | **Fail closed.** Any verification failure — signature, digest, chain, revocation, version ordering — MUST abort the update, surface a user-visible error, and record a log entry. There MUST be no "install anyway" path, no fallback to an unverified copy, and no silent retry against a different host. |
| **UC-7** | **No silent apply.** Applying an update MUST require explicit user consent and MUST NOT silently elevate. The download and verification steps may run unattended; execution of the payload may not. |
| **UC-8** | **The verification code MUST be unit-testable without the network.** Signature/digest/version-ordering verification MUST be a pure function over (manifest bytes, signature bytes, trusted keys, current version), mirroring the way `VersionInfo`'s comparison core is already testable, so the fail-closed behaviour of UC-6 can be proven by test rather than by inspection. |

| **UC-9** | **A version *notification* is not an update *channel*, and MUST stay on the safe side of that line.** The opt-in check (`src/app/UpdateChecker.cpp`, GitHub #127) MAY read a version string from the releases feed and display it. It MUST NOT download a payload, write an executable, launch an installer, or act on any other field of the response — in particular it MUST NOT follow a URL taken from the response body. It MUST be off by default. Its parsing MUST be a pure function over the response bytes, testable with no network (as UC-8 requires of the channel proper), and MUST fail closed: a response it cannot parse yields "no answer", never "no update" and never a notification. While these hold, UC-1..UC-8 do not apply, because nothing attacker-influenced is ever executed; the moment any of them is violated, an update channel has appeared and UC-1..UC-8 apply in full. |

**Scope note.** Recording these requirements does **not** implement WIN-90. The
channel remains unimplemented; this section is the security contract any future
implementation inherits. UC-9 is the exception that proves it: it exists to say
precisely what the opt-in notifier is allowed to be, so that "we already do an
update check" can never be read as "the channel is built".

---

## Autosave write latency under antimalware

Meta-audit gap-register row **WIN-89**, tracked as **GitHub #106**.

### The interaction

Signing and on-access antimalware compound each other on the autosave path.

`AutosaveManager::writeSidecar()` persists its recovery metadata through
`QSaveFile`, which is deliberately atomic: it writes a temporary file and then
**renames** it into place, so a concurrent scan can never observe a
half-written sidecar. Both of those steps are exactly what a real-time
antimalware engine hooks — a freshly created file is inspected before the
handle is released, and the rename presents the file again under its final
name. The sidecar is only a few hundred bytes of JSON, so its cost is entirely
filesystem metadata and scanner time; payload size is irrelevant.

Code signing adds to this rather than relieving it. On first touch of a signed
binary (and of files it creates, under some reputation-based policies) the
engine may additionally verify the publisher chain, which can involve a
revocation check. A cold or unreachable CRL/OCSP endpoint turns that into a
network-bound wait on a code path the user expects to be invisible.

The user-visible symptom is a UI stall on the autosave tick with **nothing in
any log** — which is what makes it expensive to diagnose rather than merely
slow.

### What has been done about it

A **write-duration probe**, not a fix. `writeSidecar()` times its whole
`QSaveFile` open/write/commit — `commit()` included, because the rename is
where a scanner blocks — and emits **one** `qWarning` per process if the
duration reaches `AutosaveManager::kSidecarWriteWarnMs` (**250 ms**).

The threshold's rationale is written next to the constant in
`src/core/AutosaveManager.h`: an unobstructed sidecar write is low
single-digit milliseconds, so 250 ms is roughly two orders of magnitude above
expectation — beyond scheduler jitter, a busy disk or a virtualised runner, and
still well below the ~1 s mark at which a stall becomes user-perceptible. It is
a diagnostic floor for "something is inspecting our writes", not a performance
budget. Nothing branches on it except the warning; a delayed autosave still
**succeeds**.

It warns once per process, not once per write, because a scanner that delays
one write delays every subsequent one and autosave runs on a timer — warning
per occurrence would emit indefinitely and bury the first, most useful report.
The latch is process-wide rather than per-instance because the condition is a
property of the machine, not of one open document.

Tests: `tests/test_autosave_write_latency_probe.cpp`. The slowness is
**injected** through `AutosaveManager::setSidecarWriteProbeHook()` rather than
waited for, so the oracle runs unconditionally on every machine instead of
being a coin flip on disk speed.

### What has NOT been verified — this is not a clean bill of health

**No live antimalware-toggle verification has been performed.** Everything
above is reasoning about a documented mechanism plus a probe exercised with an
*injected* delay. Specifically, none of the following has been measured:

- autosave latency with Windows Defender real-time protection **ON** over a
  non-excluded scratch directory, versus the same run with it off;
- whether the 250 ms threshold is correctly placed against real scanner
  latency on this hardware — it is currently an argued value, not a measured
  one;
- whether the probe fires in the field at all, as opposed to under injection;
- any contribution from publisher-chain verification or a cold revocation
  endpoint, which is hypothesised above and not demonstrated.

That verification is **blocked pending [GitHub #105](https://github.com/ipswitch8/PDFE/issues/105)**
(WIN-86: "a CI lane, or a documented manual pass, with Defender real-time
protection ON over a non-excluded scratch dir, running open/edit/save/redact/
autosave"). #105 is labelled `needs-external` — toggling real-time protection
on the self-hosted runner is an administrative action outside the scope of the
pipeline that added this probe, and no environment presently exists in which
the measurement can be taken honestly.

Until #105 lands, treat the probe as **instrumentation awaiting calibration**.
Do not cite this section as evidence that the signing/AV autosave interaction
has been characterised, quantified, or resolved — it has been *documented and
instrumented*, which is a strictly weaker claim.

---

## Files

| File | Purpose |
|---|---|
| `scripts/sign/Sign-Artifacts.ps1` | Sign + verify given files via Trusted Signing |
| `scripts/sign/Build-Release-Signed.ps1` | Full signed-release pipeline (build → sign → pack → sign → verify) |
| `scripts/sign/Submit-SmartScreen.ps1` | Prepare + open a SmartScreen/WDSI submission |
| `scripts/sign/trusted-signing-metadata.example.json` | Template for the signtool+dlib path |
