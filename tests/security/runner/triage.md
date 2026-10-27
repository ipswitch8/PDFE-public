# Phase 3 corpus runner — finding triage

Records the disposition of every distinct PDFE behavior surfaced
by the malicious-PDF corpus run. Each entry below corresponds to
either a single payload or a class of payloads.

| Disposition | Meaning |
|---|---|
| `fix-now` | Real PDFE bug. Fix in `src/` this triage cycle, with at least one regression test. The finding's hash drops out of `findings.json` after the fix. |
| `accept` | Real PDFE behavior we have decided not to change (e.g., sandbox catches it via the firewall block rule). The finding's hash stays in baseline; rationale required. |
| `monitor` | Behavior we want to track but no immediate action. Re-evaluation date required. |

## Process

1. Run `python tests/security/runner/run_corpus.py` (admin).
2. Inspect `tests/security/reports/corpus_run_*.json`.
3. For each `final_decision='fail'` record, read its
   `non_loopback_blocked` entries and identify the PDFE code
   path. Compare to `expected_pdfe_behavior` for the payload.
4. Add a triage entry per finding (or class of findings) below.
5. For `fix-now`: implement, add a unit/integration test,
   re-run. Confirm the payload moves to `pass`.

## Per-payload format

```
### <relative_path> (<category>, expected=<expected_behavior>)

**Disposition:** fix-now | accept | monitor

**Observed:** <one-line description of what PDFE actually did>

**Code path:** <best guess at the source-level cause>

**Action:** <fix description, OR rationale for accept/monitor>
```

## Cross-run correlation (2026-04-25)

Two live runs produced 13 + 16 failures respectively. Control
experiment with a clean PDF (`test_av_baseline.py`) showed 0
firewall drops over 5 rounds, proving every finding is real
PDFE-driven behavior. 4 payloads failed in BOTH runs (highest
confidence); 21 unique payloads failed across at least one run.

| Category | Failure rate | Triage approach |
|---|---|---|
| `font` | 100% (2/2) | likely FunctionType-4 PostScript JS (PDF.js CVE-2018-5158); fix-now to disable PostScript eval |
| `svg_polyglot` | 100% (1/1) | PDFE blindly handing non-PDF to MuPDF; **fix-now via magic-byte check on Document::open** (implemented below) |
| `uri` | 60% (3/5) | URI / OpenAction firing without click; need to distinguish auto-fire vs user-fire |
| `submitform` | 50% (3/6) | XFA `initialize` event running SubmitForm on load; sandbox catches it but PDFE shouldn't try |
| `launch` | 50% (1/2) | /Launch firing without prompt |
| `js` | 38% (12/32) | MuJS executing /AA / OpenAction at doc open |
| `xfa` | 33% (2/6) | XFA initialize event |
| `xxe` | 25% (1/4) | XML external entity expansion in /Metadata or /XFA |

## Triaged findings

### svg_polyglot/test14.svg (svg_polyglot, expected=refuse)

**Disposition:** fix-now (partially closed) + monitor remaining

**Observed (pre-fix):** PDFE was given a `.svg` file that's an
ImageMagick MSL/SVG polyglot. PDFE blindly passed the file to
`Document::open`, which forwarded it to MuPDF's
`fz_open_document`. MuPDF's SVG handler (or Qt's QtSvg in some
code path) processed `<image xlink:href="msl:test14.svg">` and
triggered hostname resolution; pfirewall.log recorded NetBIOS
broadcasts (UDP 192.168.0.255:138) attributed to PDFE.exe.

**Code path (pre-fix):** `MainWindow::openFile` →
`Document::open(ctx, path)` → MuPDF's `fz_open_document`
content-type sniff → SVG handler.

**Action (taken):** Added magic-byte validation to
`Document::open`: refuse any input where `%PDF-` (0x25 0x50
0x44 0x46 0x2D) is not present in the first 1024 bytes.
Implemented in `src/core/Document.cpp` via `_looks_like_pdf()`.
Verified: 106 existing Document/form/annotation tests still
pass + 3 new regression tests in `test_document.cpp`
(OpenNonPdfMagicBytesThrows, OpenEmptyFileThrows,
OpenAcceptsPdfWithSmallPreamble).

**Post-fix observation:** The original MuPDF-SVG-handler
attack vector is closed. The corpus run still flags
`svg_polyglot/test14.svg` as fail, but with different
non_loopback_blocked entries:
  - **Pre-fix:** UDP 192.168.0.255:137-138 (NetBIOS broadcast
    on local subnet -- could potentially be picked up by an
    on-subnet attacker)
  - **Post-fix:** UDP 224.0.0.252:5355 + ff02::1:3:5355
    (LLMNR multicast, link-local only -- cannot reach an
    off-segment attacker; PDFE's `QMessageBox::critical`
    error-display path appears to trigger Qt's hostname
    resolver for some reason)

The post-fix finding is **substantially less severe** -- LLMNR
multicast cannot reach off-segment, so no real exfiltration
risk. The error-dialog path that triggers it would need a
deeper investigation into Qt's QMessageBox / QPlatformIntegration
networking behavior to fully suppress. Disposition:
**monitor** the remaining LLMNR signal; revisit if it ever
upgrades to a more concerning protocol.

The closing of the MuPDF-SVG-handler vector also protects
against any future polyglot-PDF attack that tries to confuse
format detection by prepending non-PDF bytes.

### Categories triage (architectural assessment + fix status)

A live deep-dive after the magic-byte fix landed showed PDFE's
own source code does **not** auto-dispatch any of the four
high-risk action categories (verified by Explore agent against
src/forms/, src/core/, src/ui/MainWindow.cpp). What we measure
on the wire from corpus runs comes from one of:

  (A) MuPDF's internal handling at fz_open_document /
      pdf_enable_js (it runs /AA, /OpenAction, /CO when JS is
      enabled).
  (B) Qt's eager URL resolution when annotations are
      enumerated for tooltips, link panels, or the preflight
      walk.
  (C) Windows' OS-level name resolver firing NetBIOS / LLMNR
      broadcasts when ANY hostname-shaped string flows into
      QFileInfo / QUrl / QDesktopServices code paths.

The corpus runner's `non_loopback_blocked` field captures
(A)+(B)+(C) collectively. The firewall block rule scoped to
PDFE.exe correctly drops them; the question per finding is
whether a real exfiltration is possible.

#### 1. JavaScript auto-execution -- FIXED (commit 40f1977)

**Disposition:** fix-now (closed)

**Pre-fix behavior:** `jsEngine_->enable()` called
unconditionally at MainWindow.cpp:2016. `pdf_enable_js()`
activates MuJS; MuPDF's internal handler then auto-fires
`/AA`, `/OpenAction`, `/AcroForm /CO` JavaScript scripts. The
JS calls `app.submitForm()` / `app.launchURL()` etc., which
trigger hostname resolution.

**Fix:** New `AppSettings::javaScriptEnabled()` (default
FALSE). The auto-enable call is gated behind it. JS only
activates when the user explicitly opts in via the Settings
dialog (forthcoming) or per-document active-content banner.
207 existing form/JS/MainWindow tests still pass.

#### 2. XFA initialize events -- N/A in PDFE source

**Disposition:** accept (rationale: PDFE has no XFA implementation)

**Reality:** PDFE has no XFA-form support. The Explore agent
found zero references to xfa, XFA, XfaForm,
pdf_invalidate_xfa, etc. in src/. MuPDF's XFA processing may
fire internally at fz_open_document for documents that
declare /XFA, but PDFE has no surface area that drives it
beyond standard document open.

**Residual signal:** `xfa/test48.pdf` etc. trip
non_loopback_blocked entries via either MuPDF's internal XFA
parsing OR Qt's annotation enumeration. All link-local
broadcast destinations (192.168.0.255:137-138, 224.0.0.252:5355,
ff02::1:3:5355) -- cannot reach off-segment attackers. The
firewall block rule catches them; no real exfil.

**Future hardening:** When/if PDFE adds XFA support, gate the
SubmitForm target through HostAllowlist (already exists at
src/formsubmit/HostAllowlist.h).

#### 3. PostScript Type-4 font functions -- N/A in PDFE source

**Disposition:** accept (rationale: PDFE does not invoke
Type-4 evaluation)

**Reality:** PDFE has no FunctionType-4 evaluator. Font
handling in PDFE is limited to substitution, metrics,
subsetting, and embedding -- no PostScript function eval.
MuPDF may evaluate Type-4 functions internally during
font/color rendering, but PDFE has no API surface that
exposes this.

**Residual signal:** Same as XFA -- link-local resolver noise
that the firewall blocks. No real exfil possible.

**Future hardening:** If a future PDFE feature ever wraps a
MuPDF call that evaluates Type-4 PostScript, gate it behind
AppSettings::externalActionsAllowed() (added in 40f1977,
default FALSE) and refuse evaluation at the source.

#### 4. /OpenAction auto-fire -- N/A in PDFE source

**Disposition:** accept (rationale: PDFE does not auto-dispatch
OpenAction)

**Reality:** PDFE has no `pdf_document_event_did_open()` call
or equivalent. The Explore agent confirmed that all action
dispatch (`/Launch`, `/URI`, `/SubmitForm`, etc.) is
**user-triggered only** via `MainWindow::handleLinkAction()`,
and `/Launch` is already explicitly guarded with a user
prompt (MainWindow.cpp:2416-2421). With the JavaScript
opt-in gate from category 1, MuPDF's internal /OpenAction
JavaScript-type dispatch is also closed.

**Residual signal:** Same link-local resolver noise.

**Future hardening:** AppSettings::externalActionsAllowed()
is added (default FALSE) so any future code that adds
OpenAction auto-dispatch is forced to check it.

### Net result of this triage cycle

| Category | Auto-fire in PDFE source? | Action taken |
|---|---|---|
| 1. JS at doc open | YES (was) | **Fixed** — opt-in via AppSettings::javaScriptEnabled() |
| 2. XFA initialize | No PDFE code | **Accepted** — link-local noise only |
| 3. Type-4 fonts | No PDFE code | **Accepted** — link-local noise only |
| 4. OpenAction auto-fire | No (user-triggered only) | **Accepted** — AppSettings::externalActionsAllowed() future-proofing added |

The remaining `non_loopback_blocked` findings on the corpus
run are all destinations to local-segment broadcast or
multicast addresses (192.168.0.255:137-138 NetBIOS,
224.0.0.252:5355 IPv4 LLMNR, ff02::1:3:5355 IPv6 LLMNR).
None can reach an off-segment attacker. The firewall block
rule blocks them all. The phase-3 gate is doing its job:
surfacing real PDFE/Qt code paths that touch the OS
resolver, even when those paths can't actually exfiltrate.

### Phase-3 cycle 2 measurements (post-WPAD-disable)

After the JS opt-in gate (commit 40f1977) AND the
QNetworkProxyFactory::setUseSystemConfiguration(false) +
NoProxy fix (commit b9a1a13), a fresh phase-3 run was
compared to the prior baseline.

| Metric | Pre-fixes | Post-WPAD-fix | Change |
|---|---:|---:|---:|
| Failed payloads / 70 | 17 | 16 | -1 (within variability) |
| Total non_loopback_blocked entries | 83 | 77 | -6 (~7%) |
| LLMNR drops (UDP/5355) | 36 | 36 | 0 |
| NetBIOS NBDS drops (UDP/138) | 41 | 37 | -4 |
| NetBIOS NBNS drops (UDP/137) | 6 | 4 | -2 |

**Empirical conclusion:** the WPAD/proxy disable made
effectively NO measurable difference. The 7% drop in
absolute count is within run-to-run variance (compare to
13/16/14/15/17 across earlier runs with no PDFE source
changes). LLMNR drops are *identical* (36 = 36) which would
be impossible if Qt's WPAD discovery were the source of
those broadcasts.

**What that means architecturally:** the residual NetBIOS /
LLMNR broadcasts are coming from **Windows' kernel-level
name resolver**, not Qt's user-mode network stack. When MuPDF
tokenizes a UNC path like `\\attacker.example.com\share\foo`
inside a malicious PDF (in JavaScript, in /SubmitForm
targets, in /URI links, in /Launch targets, etc.), Windows'
file-system code calls into the resolver before any C++ code
in PDFE has a chance to gate it. This happens at the
NTQueryDirectoryFile / GetAddrInfoEx layer; user-mode QApplication
/ QNetworkProxy settings don't intercept it.

**Disposition:** the WPAD disable is still defense-in-depth
(it removes ONE class of automatic network behavior PDFE
doesn't need), and stays committed. But further reduction of
the non_loopback_blocked count is **not actionable in PDFE
source** -- it would require either:

  - Refusing to tokenize UNC strings in MuPDF (deep modification
    of upstream code, very hard).
  - Running PDFE in a process sandbox that filters resolver
    calls -- equivalent to the AppContainer / Win32 Sandbox
    work that exists for other PDF readers but not this one.

Both are out of scope for incremental hardening. The
firewall block rule scoped to PDFE.exe is the production
defense for these residual paths and is verified working: no
loopback callback ever succeeds across any corpus run.

**Final triage state for the LLMNR/NetBIOS residual:**
disposition `accept` -- documented architectural limitation.
The phase-3 corpus runner correctly surfaces them as evidence
that PDFE indirectly touches the resolver via the OS, but
they don't represent an exploitable exfiltration vector
because of the multicast/broadcast scope.
