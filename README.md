# PDFE

**A native, open-source, Acrobat-Pro-class PDF editor for Windows.**

[![License: AGPL-3.0](https://img.shields.io/badge/License-AGPL--3.0--only-blue.svg)](LICENSE)
[![Language: C++20](https://img.shields.io/badge/C%2B%2B-20-00599C.svg)](https://en.cppreference.com/w/cpp/20)
[![UI: Qt 6](https://img.shields.io/badge/Qt-6.10-41CD52.svg)](https://www.qt.io/)
[![Platform: Windows](https://img.shields.io/badge/Platform-Windows-0078D6.svg)](#platform-support)
[![Release](https://img.shields.io/badge/release-0.3.0-blue.svg)](https://github.com/ipswitch8/PDFE/releases/latest)

PDFE is a desktop PDF editor built in modern C++20 on Qt 6, backed by
[MuPDF](https://mupdf.com/) (rendering, annotations, forms, redaction, content
editing) and [QPDF](https://qpdf.sourceforge.io/) (incremental saves,
linearization, signature byte-ranges). It aims to cover the editing,
annotation, forms, signing, OCR, redaction, compliance, and page-management
workflows that make up the vast majority of real-world Acrobat Pro usage — as
a **local-only, no-telemetry, source-available** application.

Its standout capability is a **Text Reconstruction Engine (TRE)** that edits
text on *scanned / image-only* PDFs by combining OCR, metric-compatible font
substitution, glyph-atlas synthesis, and content re-injection — going further
than the "recognize text then edit" flow in most commercial tools.

---

## Download

> **No installer is published on this repository at present.** The
> [latest release](https://github.com/ipswitch8/PDFE/releases/latest) carries
> only the SBOM (`PDFE-0.3.0-sbom.spdx.json`).
>
> The signed `PDFE-0.3.0-win64.exe` was built and code-signed, but it was
> deliberately **not re-uploaded** when this repository was recreated in
> October 2026 to drop pre-rewrite history: a signed binary is not something to
> copy between repositories on the assumption it is still the right artifact.
> It remains on the retired predecessor, which is **private and not publicly
> reachable**, so that is a statement of where the file is rather than a usable
> download link. A published installer has to come from a fresh signed release.
>
> Exact asset locations, per tag, are tracked in
> [`docs/ci/release-asset-pointer.md`](docs/ci/release-asset-pointer.md) — the
> live pointer, kept current as releases change.

The installer described below is what a release provides **once one is
published**; the mechanics are unchanged and are documented here so the
packaging contract stays reviewable in the meantime.

- **Windows x64 installer** (`.exe`, NSIS) — download, run, and PDFE installs with
  Start-menu and file-association entries. Bundles the Qt runtime, MuPDF, and the
  default OCR language + font model, so there are no separate prerequisites to install.
- **Administrator rights are required to install.** The installer is
  **per-machine only** — it writes `HKLM` app-registration, `App Paths` and
  file-association keys, so it must run elevated. There is deliberately **no
  per-user (HKCU-scoped) install mode**; the disposition and its rationale are
  recorded in
  [`docs/installer-residue-and-hkcu-intent.md`](docs/installer-residue-and-hkcu-intent.md).
- Prefer to build it yourself? See [Quick start](#quick-start).

> **Version 0.3.0.** PDFE is feature-rich and heavily tested (6,610 automated
> tests), but early in its release lifecycle. See
> [Project status](#project-status) for an honest read on maturity and scope.
> The 0.3.0 installer **was code-signed as Itwerx Corp** (Azure Trusted
> Signing) — it is not published here; see [Download](#download) above.
> Because the publisher is brand new, SmartScreen reputation is still accruing,
> so a warning may still appear until enough downloads establish it.

---

## Comparison chart

How PDFE compares to six well-known commercial PDF editors, by capability. The
chart is split into sections so the product columns stay in view as you scroll.

**Legend:** ✓ full · ~ partial / narrower / edition-dependent · 🔜 planned (coming soon) · ✗ not available · ⊘ out of scope by design

> **Fairness note.** The competitor columns reflect the *generally documented*
> capabilities of each product's **top / "Pro" edition** as of early 2026, to
> the best of our knowledge. Feature sets change between versions and editions,
> vendors don't publish exhaustive matrices, and some capabilities are bundled
> as separate services. Treat these columns as a good-faith orientation, not a
> substitute for each vendor's own current spec. Corrections via PR/issue are
> welcome. Product names are trademarks of their respective owners.

### Licensing & platform

| Capability | **PDFE** | Acrobat Pro | Foxit | Nitro | PDF Expert | pdfFiller | PDF-XChange |
|---|:--:|:--:|:--:|:--:|:--:|:--:|:--:|
| Cost model | Free / source | Subscription | Subscription | Paid license | Paid | Subscription | Free tier + paid |
| Open source | ✓ (AGPL-3.0) | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ |
| Runs fully local (no cloud upload) | ✓ | ✓ | ✓ | ✓ | ✓ | ✗ | ✓ |
| Native Windows desktop | ✓ | ✓ | ✓ | ✓ | ✗ | ~ (web) | ✓ |
| macOS / iOS | 🔜 | ✓ | ✓ | ~ | ✓ | ~ (web) | ✗ |
| Linux | 🔜 | ✗ | ✗ | ✗ | ✗ | ~ (web) | ✗ |

### Viewing & editing

| Capability | **PDFE** | Acrobat Pro | Foxit | Nitro | PDF Expert | pdfFiller | PDF-XChange |
|---|:--:|:--:|:--:|:--:|:--:|:--:|:--:|
| View / navigate / zoom | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| Edit existing text | ✓ | ✓ | ✓ | ✓ | ✓ | ~ | ✓ |
| **Edit *scanned* text (font reconstruction)** | ✓ | ~ | ~ | ~ | ✗ | ✗ | ~ |
| OCR | ✓ | ✓ | ✓ | ✓ | ~ | ~ | ✓ |
| Annotations & markup | ✓ | ✓ | ✓ | ✓ | ✓ | ~ | ✓ |
| Measurement tools | ✓ | ✓ | ✓ | ~ | ✗ | ✗ | ✓ |
| Full-text search | ✓ | ✓ | ✓ | ✓ | ✓ | ~ | ✓ |
| Document compare (diff) | ✓ | ✓ | ✓ | ~ | ✗ | ✗ | ✗ |

### Forms

| Capability | **PDFE** | Acrobat Pro | Foxit | Nitro | PDF Expert | pdfFiller | PDF-XChange |
|---|:--:|:--:|:--:|:--:|:--:|:--:|:--:|
| Fill forms | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| Design / create forms | ✓ | ✓ | ✓ | ✓ | ~ | ✓ | ✓ |
| Form JavaScript | ✓ | ✓ | ✓ | ~ | ✗ | ~ | ✓ |

### Security & signatures

| Capability | **PDFE** | Acrobat Pro | Foxit | Nitro | PDF Expert | pdfFiller | PDF-XChange |
|---|:--:|:--:|:--:|:--:|:--:|:--:|:--:|
| Password encryption + permissions | ✓ | ✓ | ✓ | ✓ | ~ | ~ | ✓ |
| Certificate (public-key) encryption | 🔜 | ✓ | ✓ | ~ | ✗ | ✗ | ~ |
| **Redaction (true removal)** | ✓ | ✓ | ✓ | ✓ | ✗ | ✗ | ~ |
| Digital signatures (PKI) | ✓ | ✓ | ✓ | ✓ | ~ | ~ | ✓ |
| Long-term validation (LTV data: `/DSS`, `/VRI`) | ~ | ✓ | ✓ | ~ | ✗ | ✗ | ~ |

### Compliance & accessibility

| Capability | **PDFE** | Acrobat Pro | Foxit | Nitro | PDF Expert | pdfFiller | PDF-XChange |
|---|:--:|:--:|:--:|:--:|:--:|:--:|:--:|
| PDF/A convert + validate | ✓ | ✓ | ✓ | ~ | ✗ | ✗ | ~ |
| **PDF/UA + auto-tagging** | ✓ | ✓ | ~ | ✗ | ✗ | ✗ | ✗ |
| Preflight | ✓ | ✓ | ~ | ✗ | ✗ | ✗ | ~ |

### Organization & optimization

| Capability | **PDFE** | Acrobat Pro | Foxit | Nitro | PDF Expert | pdfFiller | PDF-XChange |
|---|:--:|:--:|:--:|:--:|:--:|:--:|:--:|
| Page organization (insert/extract/reorder/rotate) | ✓ | ✓ | ✓ | ✓ | ✓ | ~ | ✓ |
| Crop / headers-footers / Bates / watermark | ✓ | ✓ | ✓ | ✓ | ~ | ~ | ✓ |
| Optimize / compress | ✓ | ✓ | ✓ | ✓ | ~ | ✗ | ✓ |

### Creation, export & automation

| Capability | **PDFE** | Acrobat Pro | Foxit | Nitro | PDF Expert | pdfFiller | PDF-XChange |
|---|:--:|:--:|:--:|:--:|:--:|:--:|:--:|
| Create PDF from images | ✓ | ✓ | ✓ | ✓ | ~ | ✓ | ✓ |
| Create PDF from Office documents | ✗ | ✓ | ✓ | ✓ | ~ | ~ | ✓ |
| Create PDF from scanner | 🔜 | ✓ | ✓ | ✓ | ✗ | ✗ | ✓ |
| Export to Word / Excel | ~¹ | ✓ | ✓ | ✓ | ~ | ~ | ✓ |
| Batch processing | ✓ | ✓ | ✓ | ~ | ✗ | ~ | ~ |
| Action wizard / macros | ✗ | ✓ | ✓ | ✗ | ✗ | ✗ | ~ |
| Command-line interface | ✓ | ✗ | ~ | ✗ | ✗ | ✗ | ~ |
| Cloud storage / shared review / web e-sign | ⊘ | ✓ | ✓ | ✓ | ~ | ✓ | ✗ |
| Plugin SDK | ⊘ | ✓ | ✓ | ~ | ✗ | ✗ | ~ |

<sub>¹ The export-to-Word/Excel engines exist and are tested; a menu-wired **UI is coming soon** (see [Roadmap](#roadmap--coming-soon)).</sub>

---

## Roadmap — coming soon

These capabilities are actively planned (marked 🔜 in the chart above):

- **macOS and Linux support** — the codebase is written with cross-platform intent and the build system already carries `macos-clang` / `linux-gcc` presets; first-class packaging and testing on those platforms is next up.
- **UI for Office (Word / Excel) export** — the DOCX / XLSX export engines are implemented and tested today; wiring them into the File menu is coming soon.
- **Scanner input** — "Create PDF from scanner" (TWAIN/WIA acquisition) to complement the existing "Create PDF from images" wizard.
- **Certificate (public-key) encryption** — encrypting a document to a list of recipient certificates, alongside today's password-based encryption.

Beyond these, farther-out and explicitly-out-of-scope items are listed under
[Where PDFE stands out — and where it doesn't](#where-pdfe-stands-out--and-where-it-doesnt).

---

## Quick start

Prefer a pre-built binary? See [Download](#download). To build from source, PDFE
uses **CMake + [vcpkg](https://vcpkg.io/)** (manifest mode). vcpkg fetches and
builds every dependency (Qt 6, MuPDF, QPDF, FreeType, HarfBuzz, Tesseract,
SQLite, pugixml, OpenSSL) automatically.

### Prerequisites

| Requirement | Notes |
|---|---|
| **Windows 10 / 11** or **Server 2019+** (x64) | Primary supported platform |
| **Visual Studio 2022** | MSVC v143 toolset with C++20 support ("Desktop development with C++") |
| **CMake ≥ 3.25** | The VS 2022-bundled CMake works |
| **vcpkg** | Cloned and bootstrapped; `VCPKG_ROOT` set to its path |
| **Git** | To clone the repo |

> **First build is long.** vcpkg compiles Qt 6 and MuPDF from source on the
> first configure — budget **30–90+ minutes** and several GB of disk. Later
> builds are incremental.

### Build & run

```powershell
# 1. Clone
git clone https://github.com/ipswitch8/PDFE.git
cd PDFE

# 2. Point CMake at your vcpkg (portable route uses the VCPKG_ROOT env var)
$env:VCPKG_ROOT = "C:\path\to\vcpkg"

# 3. Configure (Ninja Multi-Config, uses $VCPKG_ROOT)
cmake --preset default

# 4. Build a Release binary
cmake --build --preset release

# 5. Run
.\build\Release\pdfe.exe
```

**Using the Visual Studio generator instead?** The repo also ships a
`windows-vs2022` configure preset (`cmake --preset windows-vs2022` →
`cmake --build --preset release-vs2022`, output in
`build\vs2022\bin\Release\pdfe.exe`). Note: that preset currently hardcodes a
local vcpkg toolchain path — a rough edge at this early version — so a fresh
clone should either use the portable `default` preset above or edit
`CMAKE_TOOLCHAIN_FILE` in `CMakePresets.json` to point at your vcpkg.

### Build the installer

An installer must be packaged from the **`windows-vs2022-package` configure
tree**, never from `build\vs2022`. That tree is the ordinary Windows build with
`PDFE_TESTING=OFF`, which compiles out the test-only introspection seams
(`PDFE_TEST_PIPES` — the dialog-inventory and tool-activation pipes) so a
shipped binary carries no control channel a local process could drive. The flag
is configure-scoped (`pdfe_ui` is a static library linked into both `pdfe.exe`
and `pdfe_tests.exe`), so it genuinely needs a second tree — see GitHub #182.

```powershell
cmake --preset windows-vs2022-package                        # configure, PDFE_TESTING=OFF
cmake --build build\vs2022-package --config Release --target pdfe pdfe_preflight
cpack -C Release -G NSIS --config build\vs2022-package\CPackConfig.cmake
# → installer written to build\vs2022-package\installer\PDFE-<version>-win64.exe
```
(Requires [NSIS](https://nsis.sourceforge.io/) on the machine.)

**Signed release build.** Release binaries are signed with **Azure Trusted
Signing**. To build a fully signed installer (build → sign the app → package →
sign the installer → verify) in one step:

```powershell
az login
$env:TRUSTED_SIGNING_ENDPOINT = "https://<region>.codesigning.azure.net/"
$env:TRUSTED_SIGNING_ACCOUNT  = "<account>"
$env:TRUSTED_SIGNING_PROFILE  = "<profile>"
./scripts/sign/Build-Release-Signed.ps1
```

See [`docs/code-signing.md`](docs/code-signing.md) for the full setup, CI service-principal auth, and the SmartScreen submission flow.

### Optional: CNN font re-ranker (ONNX Runtime)

The visual font-identification pipeline can use an optional CNN re-ranker. It
is **off by default** and only compiled in (`PDFE_HAS_ONNXRUNTIME`) when
pre-built ONNX Runtime binaries are present at
`external/onnxruntime-win-x64-1.20.1/`. Without them, PDFE falls back to
PANOSE + visual-correlation font matching — no functionality is lost, only the
neural re-rank.

### Run the tests (optional)

```powershell
cmake --build --preset debug            # build the Debug test binary
ctest --preset test                     # or run build\Debug\pdfe_tests.exe directly
```

---

## Features

Everything below is implemented and wired into the application UI unless
explicitly marked otherwise. Where a capability exists in the engine but isn't
yet exposed in a menu, it's flagged — PDFE tries not to promise more than it
delivers.

### Document lifecycle
- **Open / view / save / close** with a tile-based renderer (cache + prefetch + thumbnails)
- **Save As** (full rewrite) and **incremental save** that preserves signed byte-prefixes
- **Linearization** (web-optimized output) via QPDF
- **Merge / Combine Files** — 3-step wizard, accepts mixed PDF + image inputs
- **Extract pages** to a new PDF
- **Create PDF from images** — multi-file wizard, one page per image
- **Print** and print preview

> **Coming soon:** creating PDFs from a **scanner** (TWAIN/WIA). Creating PDFs
> from **Office/HTML sources** is not currently planned (PDFE exports *to* Office
> formats rather than importing them). There is **no XFA** form support.

### Viewing & navigation
- Multi-page view, zoom, pan, fit-width / fit-page / actual-size
- **View rotation** (display-only) and **page rotation** (persisted `/Rotate`)
- **Thumbnails**, **Bookmarks/outline** (full CRUD + persistence), **Layers (OCG)** panels
- **Full-text search** — [Tantivy](https://github.com/quickwit-oss/tantivy)-backed (Rust), plus **Find + Replace**
- **Link tool** — URI, GoTo (internal), Named actions, and Launch (external, with a mandatory security prompt on every click — no auto-execute, by design)
- **Page labels** — Decimal / Roman / Alpha styles + prefix + start value
- **Navigation history** — browser-style Back/Forward (Alt+Left / Alt+Right)

### Annotations & markup
- Sticky notes, highlight, underline, strikethrough
- Ink / freehand, shapes (rectangle, circle, line, polygon), free text / text box
- **Stamps** — static (Draft/Approved/Reviewed), dynamic tokens (`{date}`, `{author}`), and custom image stamps (PNG/JPG/BMP)
- **Comment panel** with author/date attribution and **reply threading** (`/IRT`)
- **Measurement tools** — distance, perimeter, area
- **File-attachment annotations** — on-page clip icon with embedded payload
- **Alt text / ActualText** on annotations (PDF/UA accessibility)

### Forms
- All **7 field types** — Text, Checkbox, Radio, Combo, List, Push, Signature
- **Interactive designer** — drag-to-place palette, field-properties panel
- **Tab-order editor** and **calculation-order (`/CO`) editor** with drag-reorder
- **Per-field JavaScript** (keystroke / validate / calculate / format / focus / blur) via the MuJS engine, plus **document-level events** (willSave / didSave / willPrint / didPrint / willClose)
- **FDF / XFDF** import & export
- **Form submit** — URL-encoded and HTTP POST (with a host allowlist)
- **Flatten** to static content and **Reset** (fields with `/DV`)

### Security & signatures
- **Password encryption** — RC4, AES-128, AES-256, with permission flags (print/copy/modify/annotate)
- **Redaction** — true content removal with a verifier pass
- **Digital signatures** — self-signed & PKCS#7, CA-chain signing, **certification signatures** (`/DocMDP`, 3 permission levels), **invisible signatures**, and **RFC 3161 timestamps**
- **Long-term validation data** — embeds a `/DSS` dict (Certs + VRI); OCSP/CRL parsing via OpenSSL; validation data is fetched **only on explicit user request** over strict TLS; embedded certificates can never self-elevate to trust roots. **PDFE does not implement PAdES** — its signatures are ISO 32000-1 `adbe.pkcs7.detached`, and no ETSI baseline level is claimed; see [docs/signature-profile-support.md](docs/signature-profile-support.md).
- **Malformed-PDF sandboxing** — every document is first opened in a disposable `pdfe_preflight` child process; if it crashes, times out, or reports a parse error, the editor refuses the file cleanly (contains recursion/DoS parser attacks that are un-trappable in-process)

> **Coming soon:** **certificate (public-key) encryption** — encrypting to
> recipient certificates, alongside today's password-based encryption.

### Text editing & reconstruction
- **Edit existing text** using the document's existing fonts
- **Metric-compatible font substitution** with a scoring algorithm (for missing/embedded fonts)
- **Rich-text formatting** — family, size, bold, italic, underline, strikethrough, color
- **Alignment** (Left / Center / Right / Justify) with persistence into the content stream
- **Text Reconstruction Engine (TRE)** — edit text on **scanned / image-only** pages: OCR → region reconstruction → glyph-atlas synthesis → content re-injection
- **OCR** via Tesseract, including **multi-language** (`eng+fra`, …) selectable in preferences
- **Visual font identification** — PANOSE classification + visual correlation, with an *optional* CNN re-ranker (see quick start)

> **Line breaking** in the reflow engine is a **greedy** line-filler, not
> Knuth-Plass optimal breaking. **Spell-check / dictionary** are not implemented.

### Accessibility & compliance
- **PDF/A** conversion — 1b / 2b / 2u / 2a / 3b
- **PDF/UA** conversion + **auto-tagging** and structure-tree build
- **Preflight** — profile picker, severity-grouped issue tree, text report, and **one-click Fix All** for auto-fixable issues
- **Alt text / ActualText** editing dialog
- **ICC color profiles** (OutputIntent), font-embedding compliance checks
- **Light / Dark / System** theme (high-contrast friendly)
- Every interactive widget carries a stable `objectName` + `accessibleName` for UI automation and assistive tech

> Structure-tree editing is **auto-build + read** only — there is no manual
> tag-tree or visual reading-order editor yet.

### Image handling
- Insert / delete / replace / rotate / **crop** / resize images (via CTM)
- **EXIF stripping**, **grayscale conversion** (whole-document recolor to DeviceGray)
- **Batch export** of pages to PNG/JPG
- Image compression / downsampling via Optimize; Lanczos-3 text-bitmap resampling; LCD subpixel text rendering

### Document organization
- Page **insert (blank / from file)**, **delete**, **extract**, **reorder**, **rotate**, **replace**
- **Crop** pages (interactive `/CropBox` tool, undoable)
- **Headers & footers** with idempotent token substitution, **Bates numbering** (`{bates:PREFIX:START[:WIDTH]}`)
- **Watermarks** (text + image, page-range aware)
- **Optimize PDF** (compress / subset / garbage-collect)
- **Metadata** (Info dict) and **XMP** editing
- **Attachments panel** (list / add / extract / delete), **Portfolio** (package) detection

### Export / interchange
- **Plain text** export (UTF-8 BOM, form-feed page separators) — *UI-wired*
- **Word (DOCX + RTF)**, **Excel (XLSX, with table detection)**, and per-page **image** export exist in the engine and are tested; a menu-wired UI is **coming soon** — treat these as library-level today, not click-to-use features
- **Page-as-image** batch export (PNG/JPG) — *UI-wired*

### Automation & tooling
- **Batch processing** — watermark, flatten, PDF/A, set-permissions across many files
- **Document comparison (diff)** — split-pane view with change navigation
- **Command-line interface**
- **Undo / redo** across all operations

---

## Where PDFE stands out — and where it doesn't

### Genuine strengths
- **Source-available (AGPL-3.0) and free** — the only open-source option in this comparison. Nothing is a black box; every parity claim is backed by a test.
- **Local-only & no telemetry** — documents never leave your machine. (pdfFiller, by contrast, is cloud-mandatory.)
- **Scanned-text editing (TRE)** — OCR + font reconstruction + glyph synthesis to edit image-only PDFs, deeper than the stock "recognize then edit" path in commercial tools.
- **Font identification pipeline** — PANOSE + visual correlation (+ optional CNN) that most tools don't expose at all.
- **Malformed-PDF sandboxing** — documents are vetted in a disposable child process before the editor touches them.
- **Test-backed** — 6,610 GoogleTest cases plus a native pywinauto/WinAppDriver/FlaUI/PyAutoGUI UI suite.

### Coming soon (see [Roadmap](#roadmap--coming-soon))
macOS/Linux support · Office-export UI · scanner input · certificate (public-key) encryption.

### Not planned / out of scope
- **PDF creation from Office/HTML sources** — a different product category; PDFE handles the export direction only.
- **Cloud / collaboration / web-based e-signature** — a deliberate local-only stance.
- **Action-wizard macros**, **spell-check**, and a **manual tag-tree / reading-order editor**.
- **Plugin SDK** — an explicit non-goal.

---

## Architecture & tech stack

| Layer | Technology |
|---|---|
| Language | C++20 |
| UI | Qt 6 (Widgets) |
| Document model / rendering / editing | MuPDF 1.26.10 (static, AGPL) |
| Incremental save / linearization / signatures byte-range | QPDF |
| Text shaping / fonts | FreeType, HarfBuzz |
| OCR | Tesseract |
| Form JavaScript | MuJS |
| Full-text search | Tantivy (Rust) |
| Crypto (signatures, LTV) | OpenSSL |
| Optional visual font CNN | ONNX Runtime 1.20.1 (off by default) |
| Build | CMake ≥ 3.25 + vcpkg (manifest mode) |

Source is organized by subsystem under `src/` — `core/` (document model,
undo, search), `ui/` (Qt MainWindow + ~50 dialogs/panels), `rendering/`,
`annotations/`, `tre/` (Text Reconstruction Engine), `fonts/`, `forms/`,
`signatures/`, `redaction/`, `encryption/`, `compliance/`, `optimize/`,
`image/`, `diff/`, `layers/`, and more.

---

## Testing

- **GoogleTest** — the primary suite: **6,610 registered tests** (`pdfe_tests.exe`), unit + headless-integration, re-derived from `--gtest_list_tests` on 2026-10-22. Last full run recorded 2026-08-25 against the then-current 5,898: 5,882 passed, 0 failed, 16 skipped — with every skip itemised and explained, in [`docs/audit/release-readiness/gtest-run-summary.md`](docs/audit/release-readiness/gtest-run-summary.md). That run's figures are left as the historical record they are; the registered count above is current. The skips are environment-gated (absent external corpus, OCR tessdata, perf/sandbox gates), not disabled tests — there are zero `DISABLED_`-prefixed cases. Re-derive the count with `pdfe_tests.exe --gtest_list_tests`; the static `TEST*` macro count is lower because `TEST_P` suites expand at run time.
- **Qt Test** — widget/signal tests.
- **Native UI automation** — pywinauto / WinAppDriver / FlaUI / PyAutoGUI drive the real window, menus, dialogs, and rendered output. (This is a native desktop app — **not** Selenium.)
- **Opt-in security harness** — libFuzzer/AFL++ fuzzing and ASan/UBSan sanitizer builds under `tests/security/`, gated off by default.

---

## Platform support

PDFE is a **native Windows desktop application** and that is the only
first-class, tested, packaged target today. **macOS and Linux support are on the
near-term [roadmap](#roadmap--coming-soon)** — the build system already carries
`linux-gcc` and `macos-clang` presets and the dependencies are portable, but the
current platform-specific code (process mitigations, crash handling, resource
manifests) is Windows-only and there is no Linux/macOS packaging or routine
testing yet.

---

## Reporting and tracking defects

**[GitHub Issues](https://github.com/ipswitch8/PDFE/issues) is the canonical
tracker for anything still open.** If you are looking for what is known-broken,
that is the list.

The `docs/` tree also contains a large body of audit registers and gap
documents. Those are **historical evidence, not work lists** — they record what
an audit found on the day it ran. Each carries a banner saying so.
[`docs/TRACKING.md`](docs/TRACKING.md) explains the split, and which few files
are operationally live rather than historical.

## Project status

PDFE is **version 0.3.0** — ambitious in scope, extensively tested, but early
in its release lifecycle:

- **No installer is currently published here.** A signed Windows x64 installer
  for 0.3.0 exists and is **code-signed as Itwerx Corp via Azure Trusted
  Signing**, but it was not carried over when this repository was recreated in
  October 2026 — see [Download](#download). SmartScreen reputation for a new
  publisher builds with download volume, so a warning may still appear early on
  once one is published.
- The feature set above is real and test-covered, but some capabilities are
  engine-level (Word/Excel export) or intentionally narrow (Launch-link policy,
  LTV data embedding without any ETSI baseline profile — see
  [docs/signature-profile-support.md](docs/signature-profile-support.md)).
- APIs, presets, and internal structure may still change.

For everyday viewing, annotating, editing, forms, signing, OCR, redaction,
compliance, and page work, PDFE is already capable. If you're evaluating it
against a commercial tool for production use today, read the
[comparison chart](#comparison-chart) and the
[not-planned list](#not-planned--out-of-scope) carefully.

---

## License

PDFE is licensed under the **GNU Affero General Public License v3.0 only
(AGPL-3.0-only)** — see [LICENSE](LICENSE).

This is a deliberate consequence of building on MuPDF (AGPL). The AGPL's network
clause means that if you run a modified PDFE as a network service, you must make
your modified source available to its users. For most desktop use this simply
means: the source is open, and derivative works stay open.

---

## Contributing

Issues and pull requests are welcome — including corrections to the comparison
chart above. Because every shipped feature in PDFE carries test coverage,
contributions that add or change behavior are expected to come with tests
(GoogleTest for logic, the native UI-automation layer for UI flows).

---

*PDFE is not affiliated with, endorsed by, or derived from Adobe, Foxit, Nitro,
Readdle, pdfFiller, or Tracker Software / PDF-XChange. All product names are
trademarks of their respective owners and are used here for comparison only.*
