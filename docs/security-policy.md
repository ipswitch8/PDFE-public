# PDFE Security Policy

This document describes PDFE's security posture from an end-user
perspective: what PDFE does and doesn't do with attacker-controlled
PDFs, what's gated by default, and what you can opt into for trusted
documents.

For the *internal* security testing pipeline (corpus runner, SAST,
dependency CVE audit, sanitizers, secrets scan) see
`docs/security-testing.md`.

---

## Threat model

PDFE opens PDFs, which are an **attacker-controlled file format**.
A malicious PDF can embed:

- JavaScript that calls `app.submitForm()` / `app.launchURL()`
  to phone home with file contents
- `/Launch` actions that try to spawn external programs
- `/URI` actions that auto-open URLs at document load
- `/SubmitForm` actions targeting attacker-controlled URLs
- UNC paths (`\\attacker.com\share\...`) that trigger Windows
  hostname resolution before the user clicks
- XFA forms with `initialize` events that run at load
- Embedded files (steganography / payload delivery)
- Polyglot files (SVG/MSL/PostScript that pose as PDFs)

PDFE's job is to render the document and let you edit it without
exposing you to those attack vectors. It does this through three
layers:

1. **Refuse by default** for the highest-risk categories
2. **Prompt the user** before any irrevocable side-effect
3. **Sandbox boundary** as a backstop (relied on only by the
   internal test suite, not in production)

### The local-machine assumption (GitHub #42 / SEC-104)

Everything above concerns a hostile *document*. It says nothing about a hostile
*co-user*, and that omission was unstated rather than decided. It is stated here.

**PDFE assumes a single-user workstation.** Specifically, it assumes the account
running PDFE is the only account that can read the current user's
application-data directory.

That assumption is load-bearing for **autosave recovery**.
`AutosaveManager::defaultRecoveryDir()` resolves to
`QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)` and creates
it with `std::filesystem::create_directories()` — **no explicit security
descriptor**, so the directory inherits its parent's ACL. On a default Windows
install that parent is `%APPDATA%` — measured 2026-09-12 as
`C:\Users\<user>\AppData\Roaming\PDFE\PDFE\recovery`, ACL'd to the owning user
plus SYSTEM and Administrators, all three inherited. The protection is therefore
real.

> **Correction (GitHub #42 follow-up).** This paragraph previously said the
> parent was `%LOCALAPPDATA%`. It is not. Qt 6's `AppDataLocation` is the
> **roaming** profile directory; `AppLocalDataLocation` is the local one. The
> ACL happens to be the same three trustees either way, so the trustee reasoning
> below is unaffected — but the location was wrong, and it is the location that
> decides whether the "redirected or roaming AppData" caveat further down is an
> edge case or the default. On a domain-joined machine with roaming profiles it
> is the **default**, which makes the decision not to set an owner-only DACL
> more strongly justified, not less.

Until GitHub #42 that protection was **inherited and never asserted**: PDFE did
not check it and would not have noticed if it were wrong. It is now asserted —
see the control at the end of this section.

What is actually at stake: recovery files are **full copies of the document
being edited**, written without the user explicitly saving. So a weaker-than-
expected ACL is a confidentiality exposure of document content, not merely an
integrity problem.

**Does an ACL requirement follow? Deliberately not — with the reasoning shown,
because the answer is not obvious.**

*The case for setting an explicit DACL:* it would make the guarantee PDFE's own
rather than the platform's, and it would hold under the configurations where the
inherited ACL does not — a redirected or roaming `AppData` pointed at a network
share, or a terminal-server profile where the effective permissions are set by
policy rather than by the local default.

*Why it is not being done:* an explicit owner-only DACL on a roaming profile
directory is a good way to break profile synchronisation and backup tooling in
ways that surface as data loss rather than as a permission error — which trades a
confidentiality risk that requires a hostile local account for an availability
risk that does not require an attacker at all. PDFE is a desktop editor whose
threat model is the document, not the neighbouring user; a product that genuinely
needed to defend against a co-user would need far more than one directory ACL
(the temp files, the clipboard, the render cache, and the process itself are all
equally exposed), and doing one of those and not the others produces the
appearance of a boundary without the substance.

**So this is an accepted risk, scoped:** PDFE is not hardened against a hostile
local account on a shared machine. Anyone deploying it on multi-user or
terminal-server hardware, or with a redirected `AppData`, should confirm the
recovery directory's ACL themselves, or disable autosave.

The honest limitation of that position **as originally written**: it was an
assumption asserted in a document, with nothing in the product enforcing or
checking it. Recording it made it reviewable, which is strictly better than
leaving it implicit — but it was not a control and was explicitly not counted
as one.

**That gap is now closed (GitHub #42).**
`tests/test_autosave_recovery_dir_acl.cpp`
(`AutosaveRecoveryDirAcl.EffectiveAclMatchesTheDocumentedBaseline`) reads the
real DACL off `AutosaveManager::defaultRecoveryDir()` on every suite run and
fails if any ALLOW ace names a principal outside **{the owning user, SYSTEM,
Administrators}** — naming the offending trustee. It does **not** require the
owner-only DACL this section declined:
`AutosaveRecoveryDirAcl.TheBaselineToleratesAnInheritedNonProtectedDacl`
asserts the DACL is *inherited* (`SE_DACL_PROTECTED` clear) and still passes, so
the suite cannot quietly drift into mandating the hardening argued against
above. Two positive controls — directories granting `Everyone` and
`Authenticated Users` full control — require the predicate to be capable of
saying no, so a green result means "measured and clean", not "measured nothing".

What this does and does not change: the accepted risk is unchanged. PDFE still
does not defend against a hostile local account. What is no longer true is that
the assumption is unchecked — a deployment where the directory's ACL is weaker
than the baseline now fails the suite rather than passing silently.

---

## What PDFE refuses by default

These are silent, non-negotiable defenses. There is no setting
that enables them.

### Non-PDF file inputs

If you point PDFE at a file whose first 1024 bytes do not contain
the `%PDF-` magic bytes, PDFE refuses to open it. This blocks
SVG/MSL/PostScript/HTML polyglots that try to disguise themselves
as PDFs by file extension. Implementation:
`src/core/Document.cpp::_looks_like_pdf()`. Regression-tested in
`tests/test_document.cpp::OpenNonPdfMagicBytesThrows`.

### UNC paths and remote-fetch URI schemes

`Document::open()` lexically rejects any input that resolves to a
UNC path or a non-local URL scheme **before** any filesystem call.
The motivation is that the Windows kernel's NetBIOS-NS / LLMNR /
mDNS resolvers fire link-local broadcast packets the moment a UNC
hostname is canonicalized (even by a benign call like
`std::filesystem::exists`), and those broadcasts have shown up in
firewall logs attributed to `PDFE.exe`. Refusing the input at the
boundary closes the source.

Refused at `Document::open`:

- UNC: `\\server\share\…`, `//server/share/…`, `\\?\UNC\…`,
  `\\.\…`, mixed-slash variants, leading-whitespace variants
- file:// URIs that resolve to UNC: `file://host/…`,
  `file:////host/…`, `file:host/…`
- All non-local schemes: `http(s)://`, `ftp(s)://`, `smb://`,
  `cifs://`, `nfs://`, `webdav(s)://`, `dav(s)://`, `ssh://`,
  `sftp://`, `tftp://`, `ldap(s)://`

Refused at `/URI` link follow-ups (narrower policy — clickable web
links must keep working):

- All UNC and file://-to-UNC variants above
- File-share / network-FS schemes (smb, cifs, nfs, webdav, ftp, ssh,
  sftp, tftp, ldap, …) — these reach the kernel resolver via
  `ShellExecute` before any registered handler can sandbox them
- **Pass-through** for browser-safe schemes: `http(s)://`,
  `mailto:`, `tel:`, `sms:`, `news:`, `nntp://`, `gopher://` — these
  go to the user's registered handler, which has its own DNS path

Refused at `/Launch` link follow-ups: same strict policy as
`Document::open` (UNC + all non-local schemes refused). `/Launch`
is also gated by an unconditional user-prompt — see
"What PDFE prompts before doing".

Implementation: `src/core/PathSecurity.{h,cpp}` exposes
`is_unc_or_remote()` and `is_unsafe_link_target()`. Wired in
`Document::open` (`src/core/Document.cpp`) and
`MainWindow::dispatchLinkAction` (`src/ui/MainWindow.cpp`).

Regression coverage: 41 tests in `tests/test_path_security.cpp`
including a table-driven `LinkDispatcherPolicyTable` that asserts
the dispatcher's exact policy decision for representative URIs.

**Known scope gap (follow-up):** Five engine-level entry points
(`BatchEngine`, `OptimizeEngine`, `RedactionVerifier`,
`AlignmentInjector`, `HeaderFooterEngine`) call
`fz_open_document` / `pdf_open_document` directly with paths from
their respective UI dialogs. These don't currently route through
`is_unc_or_remote`. A user could be social-engineered into pointing
a Batch-Process dialog at `\\attacker\share\evil.pdf`. Tracked as
follow-up; not a primary attack vector since the user has to
manually paste the UNC.

### Windows process mitigation policies

PDFE applies these mitigation policies via `SetProcessMitigationPolicy`
at startup, before `QApplication` is constructed:

| Policy | Rationale |
|---|---|
| DEP (Permanent + DisableAtlThunkEmulation) | Pin DEP irrevocably. On x64 the OS reports `ERROR_NOT_SUPPORTED` because DEP is permanently on by default — that's success. |
| ASLR: ForceRelocateImages + BottomUpRandomization + HighEntropy + DisallowStrippedImages | Mandatory + bottom-up + 64-bit entropy. Defeats fixed-address ROP + heap-spray. |
| ImageLoadPolicy: NoRemoteImages + NoLowMandatoryLabelImages + PreferSystem32Images | Refuses DLLs from remote shares / low-IL paths and prefers System32 over the .exe directory. Closes the classic "drop a malicious comdlg32.dll next to PDFE.exe" hijack. |
| StrictHandleCheckPolicy | Surfaces invalid-HANDLE bugs as crashes instead of silent UB. |

CFG status is queried via `GetProcessMitigationPolicy` and the
result logged for forensic evidence in case of an exploit attempt
post-incident.

Intentionally NOT applied (each documented inline in
`src/app/ProcessMitigations.cpp`):

- `MicrosoftSignedOnly` — would block PDFE's own non-MS-signed
  plugin DLLs.
- `ProhibitDynamicCode` — Qt6 QML/JS engine and MuJS (PDF form
  JavaScript when opted in) need dynamic code generation.
- `ExtensionPointDisable` — breaks accessibility tools (NVDA,
  JAWS) in some configurations.
- `FontDisable` — breaks Qt font fallback for PDF rendering.

Two of those four objections — ACG and CIG — are about what the
**main** process loads, and neither applies to `pdfe_preflight`,
which links MuPDF and nothing else while being the process that
deliberately handles the most hostile input in the product. So
both are applied **there** instead of being written off
(`SEC-60`). The main process declines them; the preflight helper
does not.

Implementation: `src/app/ProcessMitigations.{h,cpp}`. Regression
coverage in `tests/test_process_mitigations.cpp`, re-derived
2026-09-08 with `--gtest_list_tests` rather than counted from the
source (this line previously said "5 tests" and had not moved
since the file gained four more):

- `ProcessMitigationsTest.*` — **6 tests** on Windows, which call
  `GetProcessMitigationPolicy` and assert each applied policy's
  bits are set in the running test process. One of the six,
  `AcgIsNotAppliedToTheMainProcess`, pins the *declined* side.
- `PreflightHardening.*` — **4 tests**, two here and two in
  `tests/test_preflight_open.cpp`. The load-bearing one is
  `TheHardenedHelperStillClassifiesDocumentsCorrectly`: asserting
  that ACG and CIG are on is not enough, because if the hardening
  broke that child, the parent reads its abnormal exit as "unsafe"
  and the failure would present to the user as *"PDFE cannot open
  any PDF"*.

**Not covered** (meta-audit `SEC-59`, GitHub #58): no architecture
test asserts that the set of intentionally-declined policies
matches a named list. Adding a fifth decline, or quietly dropping
one of the four, is a source-comment change that nothing reddens.

### Document-level OpenAction auto-fire

PDFE does not call `pdf_document_event_did_open()` or any
equivalent that auto-dispatches a PDF's catalog-level
`/OpenAction`. If a malicious PDF's `/OpenAction` references a
`/Launch`, `/URI`, `/SubmitForm`, `/ImportData`, or `/JavaScript`
action, that action does **not** run at open. (The user can still
click a link annotation that triggers the action; see "What PDFE
prompts before doing" below.)

### Encrypted with restricted permissions

PDFE honors the document's `/P` permission flags by default. You
can override (e.g., to print a copy-disabled doc you legally own)
via the per-document context menu — but the override is
deliberately hidden behind a click; nothing happens silently.

The security handler behind those flags — which cipher is written,
which permission bits the handler can actually express, and when a
document is weaker than it looks — is its own area; see
[Encryption and permission handler](#encryption-and-permission-handler)
below.

---

## What requires user opt-in

These categories are gated behind **Preferences → Security**
checkboxes. Default OFF.

### "Allow JavaScript in PDF forms"

When checked: PDFE calls `pdf_enable_js()` on the document at
open. Form fields can run JavaScript for keystroke validation,
calculation, format, focus/blur events, etc. Some malicious PDFs
use this to call `app.submitForm()` for exfiltration.

When unchecked (default): the JS engine is constructed but never
enabled. `/AA`, `/OpenAction`, and `/AcroForm /CO` JavaScript
scripts cannot run. Form calculations don't auto-update; the
fields still display correctly.

Setting key: `Security/javaScriptEnabled` in QSettings.

### "Allow external actions (Launch / URI / SubmitForm / ImportData)"

When checked: explicit user clicks on link annotations of these
types proceed without per-click confirmation (except `/Launch`,
which always prompts — see below).

When unchecked (default): clicks on link annotations of these
types are still allowed, but with a per-click confirmation dialog
that shows the destination URL/path. The user must click "Allow"
each time.

Setting key: `Security/externalActionsAllowed` in QSettings.

### "Allow JPEG 2000 (JPX) image rendering"

When unchecked (**default**): PDFE never hands JPX image data to
OpenJPEG. The decode gate lives inside MuPDF itself
(`vcpkg-overlay-ports/libmupdf/0001-pdfe-jpx-decode-gate.patch`) —
`fz_load_jpx` / `fz_load_jpx_info` return a placeholder *before*
entering openjpeg, so a malformed JPX stream is never parsed. The
render layer draws a visible "JPEG2000 image hidden for security —
enable in Preferences" overlay in place of each hidden image, so the
document is not silently altered.

When checked: JPX images decode normally, and PDFE shows a
**warn-once** confirmation the first time the box is ticked.

Why this one is default-deny. A malformed JPEG 2000 stream
heap-overflows **OpenJPEG 2.5.4 (the current upstream release)**,
producing `STATUS_HEAP_CORRUPTION` (0xC0000374) — memory corruption
with RCE potential. It is **not trappable** by `fz_try` / `fz_catch`,
so there is no in-process recovery. Found by the Tier-4 flaw corpus,
tracked as **#50**, root-caused in
`docs/audit/dca/0011-jpx-openjpeg-heap-overflow.md`. Note this is a
compile-time-enabled, run-time-gated control: `FZ_ENABLE_JPX` stays
ON, so removing the gate silently re-opens the hole. Guarded by
`ExternalFlawCorpusTest.JpxDecode_OpenRendersGracefully`
(`tests/test_external_flaw_corpus.cpp`, renders the former crasher with
the gate on) and the preference tests in `tests/test_app_settings.cpp`
and `tests/test_preferences_dialog.cpp`.

Setting key: `Security/jpxRenderingEnabled` in QSettings.

### "Allow JBIG2 image rendering"

When unchecked (**default**): PDFE never hands JBIG2 image data to
jbig2dec. A blank placeholder is substituted and the affected regions
are marked on the canvas, exactly as the JPX gate does.

When checked: JBIG2 images decode normally, after a warn-once
confirmation.

**Why this one is default-deny.** jbig2dec has a long history of
memory-safety CVEs, and `/JBIG2Decode` is reachable from any ordinary
document — it is the standard compression for scanned bilevel pages.

**JBIG2 needed two doors closed, not one.** Unlike JPX, `/JBIG2Decode`
is a *stream filter* as well as a whole-image codec, so the patch gates
both `source/fitz/filter-jbig2.c` (the filter path, including
`fz_load_jbig2_globals` — the globals stream is attacker data fed
straight to `jbig2_data_in`) and `source/fitz/load-jbig2.c` (the
standalone loader). Gating only the loader would have left the filter
path wide open.

**The placeholder emits `0xff`, not an empty stream, and that is
load-bearing.** MuPDF pads a short read with **zero**, and zero is
black — an empty placeholder would render a gated page as solid black
boxes rather than blank ones. `0xff` is white, and fully transparent
for an `/ImageMask`. It is bounded at 128 MiB so a `fz_read_all`
consumer terminates.

**The oracle differs from JPX's, deliberately.** A gated JPX image comes
back 1×1 instead of its declared size, so "decoded size != declared
size" is a sufficient oracle there. A gated JBIG2 image still comes back
at its declared dimensions, so that shape is unavailable. The patch
therefore exports `fz_pdfe_jbig2_placeholder_count()`, and the tests
assert both that the counter moved **and** that the pixels are ink-free.
`Jbig2Decode_RealDecodeIsDistinguishableFromThePlaceholder` pins the
contrast on a trusted authored fixture: gate on → ink, counter 0; gate
off → blank, counter up. A "did not crash" check could not distinguish a
held gate from a regressed one.

Setting key: `Security/jbig2RenderingEnabled` in QSettings.

---

## Resource limits

> **Partial section.** Follow-up phase **F13** owns gap `SEC-99` — the
> full resource-limit policy (page-count cap, image-dimension cap,
> decompression ratio, memory ceiling) is not written yet, and this
> section deliberately does not pretend otherwise. What is recorded
> here is the one resource-limit control that already **exists in
> code** and was previously documented only as a code comment
> (gap `SEC-108`).

### Preflight open timeout — 15 seconds, fail closed

`pdfe::core::isDocumentOpenSafe()` (`src/core/PreflightOpen.h`) opens
an untrusted document in a **disposable child process**
(`pdfe_preflight`) before the app opens it in-process. That child is
bounded by a **15 000 ms default timeout**.

This is a security control, not a UX nicety:

- A document that hangs the parser — a pathological loop, a
  decompression bomb, a recursive structure MuPDF's own guards miss —
  is a denial-of-service against the editor. The timeout converts an
  unbounded hang into a bounded refusal.
- On expiry the child is **killed** (`TerminateProcess` /
  `SIGKILL`) and the verdict is **unsafe** — fail *closed*. A timeout
  is never interpreted as "probably fine".
- The bound must stay finite and sane. Passing a negative value means
  `INFINITE`, which disables the control entirely; raising the default
  into the minutes makes the DoS survivable-but-still-effective.

Guarded by `PreflightOpen.TimeoutIsEnforcedAndFailsClosed` and
`PreflightOpen.DefaultTimeoutIsBoundedAndDocumented` in
`tests/test_preflight_open.cpp`, which assert both that the timeout is
actually honoured at run time and that the declared default stays
within `(0, 60 000] ms` and is still described here.

Known limitation, stated honestly: `isDocumentOpenSafe()` is called
from the GUI open path. Engine entry points (`BatchEngine` and
friends) do not route through it — that is gap `SEC-36`, open.

---

## Dependency bump runbook (MuPDF / QPDF)

PDFE carries **patched** overlay ports. A dependency bump that ignores
them silently removes a security control, so the bump procedure is
part of the security policy rather than a build note (gap `SEC-87`).

Two patches matter most today, and **both** are default-deny decoder
gates whose loss is invisible in the build:

- `vcpkg-overlay-ports/libmupdf/0001-pdfe-jpx-decode-gate.patch` — if it
  fails to apply or is dropped during a version bump, **PDFE goes back to
  feeding malformed JPEG 2000 straight into OpenJPEG**.
- `vcpkg-overlay-ports/libmupdf/0002-pdfe-jbig2-decode-gate.patch` — if it
  is lost, **PDFE goes back to feeding attacker-controlled JBIG2 straight
  into jbig2dec**, on the standard compression for scanned pages.

Neither loss changes anything observable about the build, which is why
this is a procedural stop rather than a note. `SupplyChainInventory.
MupdfOverlayStillAppliesTheJbig2DecodeGatePatch` guards the second one.

When bumping MuPDF or QPDF:

1. **Update `VERSION` and `SHA512`** in the overlay portfile
   (`vcpkg-overlay-ports/libmupdf/portfile.cmake`, and the QPDF
   equivalent if one exists at the time).
2. **Re-apply every entry in the `PATCHES` list against the new
   source.** A rejected hunk is a hard stop, not a warning to skip
   past. Re-base the patch; do not drop it.
3. **Bump `port-version`** in the overlay `vcpkg.json`. Without this
   vcpkg may serve the previously built binary and your patch never
   ships. (This has bitten before — see the jpx-optin pipeline notes.)
4. **Rebuild with `--fresh` and an explicit `VCPKG_ROOT`** so the
   binary cache cannot mask step 3.
5. **Check advisories for the new version** — `pwsh
   scripts/security/run_deps.ps1` — and triage anything CVSS ≥ 7.0 in
   `tests/security/deps/triage.md` before merging.
6. **Run the full `pdfe_tests` suite**, and specifically the JPX gate
   tests. A green suite with a silently-dropped patch is the failure
   mode this runbook exists to prevent.
7. **Record the bump** in the version-history table at the end of this
   document if it changes any stated control.

---

## What PDFE prompts before doing

These categories prompt regardless of settings — there is no way
to silence the prompt short of patching the source.

### `/Launch` actions

Whether triggered by the user clicking a link or (theoretically)
by a future `/OpenAction` dispatcher, every `/Launch` action
displays a confirmation dialog before any process is spawned. The
dialog shows:

- The target executable path
- Any command-line arguments embedded in the action
- A clear "Run" / "Cancel" choice

Implementation: `src/ui/MainWindow.cpp::handleLinkAction()`.

### Form submission to an unknown host

The first time a PDF tries to submit a form to a hostname not in
your `HostAllowlist`, PDFE prompts: *"Allow form submission to
`<host>`?"* with options to allow once, allow always (adds to
allowlist), or deny.

Implementation: `src/formsubmit/HostAllowlist.{h,cpp}` +
`src/formsubmit/FormSubmitter.cpp`.

---

## Encryption and permission handler

PDFE both *reads* and *writes* PDF security handlers, and the two
sides use different engines: **MuPDF reads** the `/Encrypt`
dictionary and authenticates passwords, **QPDF writes** it
(`QPDFWriter::setR2/R3/R4/R6` in
`src/encryption/EncryptionManager.cpp`). That split, plus the fact
that a PDF can *declare* protection it does not actually apply, is
what makes this its own area rather than a footnote to the
permission flags above.

### Threat statement

A user who encrypts a document, or who opens one that says it is
encrypted, acts on what the product tells them. Four ways that
statement can be false, all of them silent:

1. **Crypto downgrade.** An AES-256 (`/V 5 /R 6`) document is
   opened and re-saved and comes back out under a weaker handler —
   RC4 or AES-128. Nothing visible changes: same filename, same
   password, same "Encrypted" label. The document is simply easier
   to break than the day before.
2. **Empty user password.** The owner password is set, the *user*
   password is empty. The document opens with full read access and
   no prompt at all; the `/P` permission bits are advisory and most
   viewers do not enforce them. This is the most common "protected"
   PDF in the wild and it protects almost nothing.
3. **Encrypted wrapper, plaintext content.** A document with
   `/StmF /Identity` carries a real `/Encrypt` dictionary — so every
   "is this encrypted?" check answers yes — while every stream in
   the body (page content, images, embedded files) is stored in the
   clear and can be read out of the file with a text editor.
4. **Lossy permission mapping.** The API accepts eight permission
   bits. The RC4-40 (`/R 2`) handler defines four. A caller who
   denies accessibility, form-fill, assembly or high-quality print
   at R2 gets a file that permits all four — the restriction is
   discarded on the way to disk.

A fifth case is *unsupported by design*: PDFE does not implement the
public-key / certificate handler (`/Filter /Adobe.PubSec`). Such a
document must be refused cleanly, never presented as readable.

### Control

- **Handler identity is preserved across a re-save.**
  `EncryptionManager::saveEncrypted` decrypts to an in-memory buffer
  and re-applies the requested algorithm through QPDF; the AES-256
  path calls `setR6EncryptionParameters` and no other. The
  intermediate plaintext never touches disk and is zeroed by an RAII
  `SecureZeroMemory` guard (`IntermediateWiper`) on every scope exit,
  including the throw path.
- **The empty-user-password condition is detected and surfaced.**
  `EncryptionInfo::emptyUserPassword` is set when the empty string
  authenticates, and `MainWindow::onDocumentPermissions` prints an
  explicit warning in the Document Permissions dialog. PDFE does
  *not* refuse such documents — empty user passwords are legal and
  usually intended — it refuses to let them look stronger than they
  are.
- **The applied crypt filters are reported, not assumed.**
  `EncryptionInfo::streamCryptMethod` / `stringCryptMethod` carry the
  filter MuPDF actually resolved for `/StmF` and `/StrF`, and
  `streamsUnprotected` flags the `/Identity` case, with the same
  warning surfaced in the permissions dialog.
- **The cipher label is derived from `/V`, not guessed.**
  `getMethodString` reports `/V 5` as AES-256 for **both** `/R 5`
  (the deprecated Adobe Extension Level 3 handler) and `/R 6`, and
  reports `/V 1` as RC4 rather than as "None".
- **Permission loss is reported to the caller.**
  `EncryptionManager::previewPermissionMapping` and the
  `EncryptionSaveReport` out-parameter on `saveEncrypted` enumerate
  the bits the chosen handler cannot express and the subset of those
  the caller asked to restrict; the UI raises a warning dialog before
  the save is reported as successful.
- **Unsupported handlers fail closed.** A certificate-encrypted
  document is refused (clean throw or null document), with or
  without a password.

### Test pointer

`tests/test_encryption_handler_security.cpp` — registered in
`tests/CMakeLists.txt`, no test disabled or skipped. Every test in
it pairs its refusal/report assertion with a CONTROL assertion that
must still succeed, so none of them can pass by breaking encryption
for everyone.

| test | threat covered |
|---|---|
| `Aes256R6ResaveDoesNotEmitAWeakerHandler` | 1 — crypto downgrade |
| `EmptyUserPasswordIsDetectedAndSurfaced` | 2 — empty user password |
| `IdentityStreamFilterReportedAsUnprotected` | 3 — `/StmF /Identity` |
| `Aes256R5IsReportedAsAes256NotRc4` | cipher mislabel (`/V 5 /R 5`, `/V 1`) |
| `Rc4_40LossyPermissionMappingIsReported` | 4 — lossy R2 mapping |
| `NonAsciiPasswordRoundTripsAcrossEngines` | MuPDF-read / QPDF-write password preparation |
| `CertificateEncryptedDocumentIsRefusedCleanly` | unsupported public-key handler |
| `FailedEncryptionLeavesNoPlaintextOnDisk` | intermediate plaintext handling |

Adjacent, pre-existing coverage that this area also depends on:
`tests/test_encryption_preservation.cpp` (save/incremental-save keep
the encryption dictionary and revision),
`tests/test_permission_bits_roundtrip.cpp` (all eight permission bits
round-trip at R4 and R6), `tests/test_encryption_roundtrip.cpp`.

Fixtures: `tests/fixtures/flaw_corpus/encryption/` —
`identity_stmf.pdf` (built by `tests/fixtures/gen_identity_stmf.py`),
`aes256_r5.pdf`, `aes256_certificate.pdf`, `aes128.pdf`,
`standard40.pdf`.

**Known gap, stated plainly:** proving the intermediate plaintext
buffer is *actually* zeroed needs a memory-inspection harness this
project does not have. What is asserted today is the observable half
— nothing plaintext reaches disk on any path. See
`docs/audit/meta-audit-2026-07/followup/phase-3-dispositions.md`.

---

## Sandbox boundary (test-only)

The internal phase-3 corpus runner adds a Windows Firewall rule
scoped to `PDFE.exe` that blocks all outbound traffic except to
`127.0.0.1`. This is **not** a production defense — your real
PDFE installation does not install firewall rules. The sandbox
exists so the test suite can demonstrate that PDFE doesn't leak
attacker-controlled hostnames even when one of the layers above
has a bug.

Production users get layers 1-2-3 above. The sandbox is
test-suite-only.

See `docs/security-testing.md` for the testing pipeline.

---

## Reporting a security issue

If you find a way to make PDFE leak data, run an attacker-supplied
program, or render unprompted UI from an attacker-controlled PDF,
please:

1. Open a private security advisory at
   <https://github.com/ipswitch8/PDFE/security/advisories/new>
   (do not file a public issue).
2. Include: the PDF that reproduces, expected behavior, observed
   behavior, your PDFE version (Help → About).
3. We commit to acknowledging within 7 days and aiming for a fix
   within 30 days for critical severity.

Don't worry about the PDF being malware — phase-3's corpus runner
exists specifically to handle this kind of report safely. The
corpus already includes 70 representative attack patterns.

---

## What's NOT yet covered

Honest disclosure of where PDFE's security posture has known gaps:

- **OS-level name resolution noise (residual)**: PDFE rejects UNC
  paths and file-share URIs at the input boundary (see "What PDFE
  refuses by default" above), which closes the primary leak vector
  surfaced by phase-3. Residual broadcasts captured by the corpus
  runner's firewall sandbox include two distinct classes:
  - **Link-local multicast**: LLMNR (`224.0.0.252:5355`,
    `ff02::1:3:5355`) and mDNS. Cannot reach off-segment attackers.
  - **LAN-subnet broadcasts**: NetBIOS-NS / NetBIOS-DGM
    (`192.168.x.255:137-138`, similar /24 ranges). These reach
    every host on the same Ethernet segment (a LAN-subnet
    attacker, not just link-local). Mitigation in production
    typically falls to network segmentation, not the application.

  The corpus runner's firewall rule is currently machine-wide,
  not per-process, so some captured entries reflect ambient
  subnet traffic during the test window rather than PDFE-emitted
  packets. A follow-up to tighten the firewall scope to
  per-process is tracked in `tests/security/CHANGELOG.md`.

  Closing the genuinely PDFE-emitted residual broadcasts at their
  source inside Qt/MuPDF would require either an in-process WFP
  filter (kernel-mode, out of scope) or a multi-process broker
  (~60-120 dev-days, evaluated and deferred).
- **Engine-level entry-point gap**: `BatchEngine`, `OptimizeEngine`,
  `RedactionVerifier`, `AlignmentInjector`, and `HeaderFooterEngine`
  currently call `fz_open_document` / `pdf_open_document` directly
  on paths from their respective UI dialogs without routing through
  `is_unc_or_remote()`. A user manually pasting a UNC path into the
  Batch-Process dialog would still trigger the kernel resolver.
  Tracked as a follow-up.
- **MSan / TSan coverage**: PDFE's sanitizer test phase runs ASan
  + UBSan on Windows. MSan (uninitialized memory reads) and TSan
  (data races) require a Linux build that doesn't exist yet. See
  `docs/security-testing.md` § (e).
- **Coverity / commercial SAST**: PDFE uses cppcheck + clang-tidy
  open-source. No commercial Coverity license is available; if
  one becomes available, it'd be additive but isn't a gap that
  prevents shipping.
- **Autosave/crash-recovery copies rely on a single-user
  workstation assumption** (meta-audit `SEC-104`, GitHub #42):
  autosave writes a **complete PDF copy of the open document,
  unsaved edits included**, plus a JSON sidecar naming the
  original file, into
  `QStandardPaths::AppDataLocation` + `/recovery`
  (`AutosaveManager::defaultRecoveryDir`). That copy persists
  until a clean save or an explicit discard, so a document opened
  once from a share becomes a durable plaintext copy on the local
  profile.

  The only thing protecting it is the ACL Windows puts on the user
  profile. PDFE does **not** create that directory through
  `pdfe::core::createOwnerOnlyDirectory` — the primitive it uses
  for the font cache (`SEC-103`) and the plaintext search index —
  and it does not verify the resulting permissions at runtime.

  **This is an assumption, stated, not a control.** PDFE assumes a
  single-user Windows workstation on which the profile ACL holds.
  On a machine where `%APPDATA%` has been relocated or re-ACLed
  (roaming profiles, redirected folders, a shared build or kiosk
  account), recovery copies are readable by whoever can read that
  path, and PDFE will not notice or say so.

  No test asserts the recovery directory's permissions. The
  autosave suites (`test_autosave_recovery.cpp`,
  `test_autosave_sidecar_integrity.cpp`, `test_autosave_matrix.cpp`)
  assert capture and recovery behaviour only, so a regression that
  moved this directory somewhere world-readable would be caught by
  nothing. Adding that assertion is the open half of #42.
- **No active-content banner UI yet**: when a PDF contains
  JavaScript or external actions and the corresponding setting is
  off, PDFE silently doesn't run them. A future enhancement is a
  per-document banner: *"This PDF contains JavaScript. Currently
  blocked. [Allow once] [Always for trusted docs] [Stay safe]"*.

---

## Version history

| Version | Date | Change |
|---|---|---|
| 0.1.0 | 2026-04-25 | Initial security policy. JS opt-in gate (`Security/javaScriptEnabled`); external-actions gate (`Security/externalActionsAllowed`); magic-byte refusal at `Document::open`; `/Launch` prompt; HostAllowlist for form submission. |
| 0.1.1 | 2026-04-26 | UNC + remote-scheme rejection at input boundary (`Document::open` + `/URI` + `/Launch` link follow-ups via `pdfe::core::path_security::is_unc_or_remote` / `is_unsafe_link_target`). Windows process mitigation policies pinned at startup (DEP, ASLR variants, ImageLoadPolicy, StrictHandleCheck) with per-policy `GetProcessMitigationPolicy` regression assertions. Engine-level entry points (`BatchEngine`, `OptimizeEngine`, `RedactionVerifier`, `AlignmentInjector`, `HeaderFooterEngine`) tracked as deferred follow-up. |
| 0.1.2 | 2026-08-01 | New area: **Encryption and permission handler**. Crypto-downgrade, empty-user-password, `/StmF /Identity` and lossy-R2-permission-mapping threats stated, controlled and tested (`tests/test_encryption_handler_security.cpp`). `EncryptionInfo` gains `streamCryptMethod` / `stringCryptMethod` / `streamsUnprotected` / `stringsUnprotected` / `emptyUserPassword`; `saveEncrypted` gains an `EncryptionSaveReport` out-parameter; `getMethodString` corrected for `/V 5 /R 5` and `/V 1`. Added by meta-audit 2026-07 follow-up phase F3 (`PDF-24/25/26/27/29/30`). |
| 0.1.4 | 2026-09-08 | Two **honest-disclosure** additions, no behaviour change. The autosave/crash-recovery copy is recorded as resting on a **single-user-workstation assumption** rather than a control — it uses `QStandardPaths::AppDataLocation`, not `createOwnerOnlyDirectory`, and no test asserts its permissions (`SEC-104`, GitHub #42). The "Windows process mitigation policies" section states that ACG and CIG, declined for the main process, ARE applied to `pdfe_preflight` (`SEC-60`), corrects a stale "5 tests" regression-coverage count that had not moved in four test additions, and records that no architecture test pins the *set* of declined policies (`SEC-59`, GitHub #58). Added by pipeline `backlog-workable-p82` phase 18. |
| 0.1.3 | 2026-08-02 | **JPEG 2000 (JPX) decode is default-deny** and now documented as a user-facing opt-in (`Security/jpxRenderingEnabled`, warn-once on enable, placeholder overlay) — closes the "shipped control absent from the policy" gap `SEC-97`; the control itself landed 2026-07-31 with #50. New **Resource limits** section records the `isDocumentOpenSafe()` 15 s fail-closed preflight timeout as a security control (`SEC-108`; the remaining resource-limit policy is `SEC-99`, owned by F13). New **Dependency bump runbook** makes patch-loss on a MuPDF/QPDF bump a procedural stop (`SEC-87`). Added by meta-audit 2026-07 follow-up phase F9. |
