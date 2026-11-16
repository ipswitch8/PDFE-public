# What is needed to produce a Linux version of PDFE

> **Status banner.** PDFE ships Windows-only today. A Linux build is a FUTURE
> target that has *not* been undertaken. This document is a grounded survey of
> the work required, with every claim cited to a file and line in the tree as it
> stands. Where something could not be determined from the code it is called out
> explicitly in **Open questions / unknowns** at the end.
>
> The deferral is not informal: `.github/workflows/linux-validation.yml:3-14`
> states the Linux and macOS lanes are "ASPIRATIONAL ... they attempt a genuine
> build and test run, they fail, and that failure is EXPECTED until the port is
> undertaken", and `:40-44` records that the port is deferred by explicit user
> direction under tracker issue **#42** (macOS `-ljpeg` link failure + Linux test
> failures). Treat #42 as the canonical tracker entry for this effort.

The headline finding is encouraging: **the C++ source is already overwhelmingly
cross-platform-structured.** Almost every Windows API touch is already wrapped in
`#ifdef _WIN32` / `#ifdef Q_OS_WIN`, usually with a POSIX `#else` branch, and
CMake already carries `linux-gcc` configure/build/test presets and a standing
(red) Linux CI lane. The port is mostly *finishing and validating* work already
scaffolded, not a from-scratch abstraction effort. The concentrated risk is in
(a) a handful of still-unguarded source sites, (b) the UI-automation test layer,
and (c) packaging/signing, which are wholly Windows-specific.

---

## 1. Current platform handling in the build

### 1.1 Root `CMakeLists.txt`

- Project is plain C++20, no language/platform assumptions at the top
  (`CMakeLists.txt:1-6`). Qt AUTOMOC/UIC/RCC on (`:8-11`).
- `find_package` block (`:18-40`) requests Qt6 (Widgets, Test, PrintSupport,
  Network, Svg, LinguistTools), `unofficial-libmupdf`, `unofficial-mujs`,
  `qpdf`, `freetype`, `harfbuzz`, `unofficial-sqlite3`, `Tesseract`, `pugixml`,
  `OpenSSL`. None of these finds is Windows-gated, so configure logic is portable.
- **ONNX Runtime is hard-pinned to a Windows x64 prebuilt**
  (`CMakeLists.txt:54` `onnxruntime-win-x64-1.20.1`, SHA-256 pinned at `:55-57`,
  `onnxruntime.lib`/`.dll` probe at `:59-100`). This is optional — absent simply
  disables the CNN font re-ranker (`:101-104`) — but a Linux build gets **no**
  ONNX re-ranker unless a Linux ONNX Runtime is wired in. `src/app/CMakeLists.txt:349-351`
  already anticipates this: "on Linux ONNX Runtime is found via system loader /
  rpath when installed via apt/package manager", but no CMake code yet locates it
  on Linux. See Open questions.
- **Everything under "Installation & Packaging" after `:293` is `if(WIN32)`-gated:**
  - Runtime DLL deployment (`:297-388`) — the entire explicit vcpkg DLL list and
    the special ICU/zlib/curl handling.
  - CNN model + tessdata install (`:400-409`) — note these two are *inside* the
    `if(WIN32)` block (`:297`…`:507`), so a Linux install tree currently installs
    **no tessdata and no ONNX model** at all. That is a packaging gap to port, not
    just a DLL-deploy gap.
  - `windeployqt` Qt-runtime deployment (`:411-506`) — Windows-tool-specific.
  - CPack NSIS generator + file-association `.nsh` includes (`:531-643`). The
    `CPACK_GENERATOR` is set to `"NSIS"` only inside `if(WIN32)` (`:532`); on Linux
    `CPACK_GENERATOR` is left unset, so `include(CPack)` at `:645` would default to
    TGZ/STGZ — i.e. no real Linux package is produced today.
- Portable packaging bits that already work cross-platform: `LICENSE` install
  (`:510`), `CORRESPONDING-SOURCE.txt` AGPL offer install (`:518`), and the CPACK
  metadata block (`:520-529`).

### 1.2 `src/app/CMakeLists.txt`

- `dbghelp` link is `if(WIN32)`-gated (`:30-32`); `WIN32_EXECUTABLE` property is
  `if(WIN32)`-gated (`:70-72`).
- The executable itself is split: with `pdfe.rc` on Windows, without it elsewhere
  (`:47-58`) — so the Windows resource is already correctly excluded on Linux.
- `/Brepro`, `/PDBALTPATH`, `/INCREMENTAL:NO` link options are `if(MSVC)`-gated
  (`:143-154`) — no effect under gcc/clang.
- **Qt plugin / DLL POST_BUILD staging is `if(WIN32)`-gated** (`:243-347`), and
  the comment at `:241-242` explicitly says Linux `qt6-base-dev` installs plugins
  to `/usr/lib` so no staging is needed. ONNX DLL deploy is `PDFE_HAS_ONNXRUNTIME AND WIN32`
  (`:352`).
- Translations (`qt_add_lrelease`, `:170-193`) and `pdfe_preflight` target
  (`:199-217`) are platform-neutral. The fuzz-only sanitizer link-options on
  `pdfe_preflight` are gated `AND CMAKE_SYSTEM_NAME STREQUAL "Windows"` (`:208-209`).

### 1.3 `CMakePresets.json` — Linux presets already exist

- Configure presets `windows-vs2022`, `windows-vs2022-asan`,
  `windows-vs2022-package`, `windows-vs2022-msvc-asan` are all
  `condition: hostSystemName == Windows` (`:24-137`).
- **A `linux-gcc` configure preset already exists** (`:138-158`): Ninja
  Multi-Config, `VCPKG_TARGET_TRIPLET=x64-linux`, `gcc`/`g++`,
  `condition: hostSystemName == Linux`. A `macos-clang` preset mirrors it with
  `x64-osx` (`:159-179`).
- Matching build presets `linux-debug`/`linux-release` (`:218-229`) and test
  preset `linux-test` (`:311-319`) exist.
- **Caveat 1 — the Linux preset uses the stock `x64-linux` triplet**, not the
  overlay-repro triplet the Windows presets pin
  (`VCPKG_TARGET_TRIPLET=x64-windows-repro` + `VCPKG_OVERLAY_TRIPLETS` at
  `:42-44`). So the reproducible-build / cache-pinning machinery
  (`tools/vcpkg-triplets/x64-windows-repro.cmake`, the only triplet file present)
  does not cover Linux; there is **no `x64-linux-repro` triplet** in the repo.
- **Caveat 2 — the real Linux CI lane does NOT use this preset.**
  `linux-validation.yml:91-96` configures with a raw `cmake -B build-linux` using
  `clang-15`/`clang++-15` (not gcc) and `-DVCPKG_TARGET_TRIPLET=x64-linux`. So the
  preset's `gcc` toolchain and the CI lane's `clang-15` toolchain disagree; which
  compiler the port should target is an open decision.

### 1.4 vcpkg manifest — already partly Linux-aware

- `vcpkg.json` describes PDFE as a "Cross-platform PDF editor" (`:5`) and its deps
  (`:8-36`) carry no Windows `platform` constraint except **one**:
  `curl` is declared `"platform": "!windows"` with `openssl` feature (`:31-35`) —
  i.e. libcurl is pulled from vcpkg **only on non-Windows**. On Windows, libcurl
  is supplied another way (it appears in the deploy DLL list at
  `CMakeLists.txt:365`); the mechanism is not in this manifest. The net effect:
  the manifest *already expects* a non-Windows curl, which is a good sign for the
  TSA/HTTPS path on Linux.
- A Linux triplet is `x64-linux` (used by the preset `:149` and CI `:96`).

---

## 2. Windows-only source code — port / `#ifdef` status

Good news first: a tree-wide search shows the Windows API surface is almost
entirely already conditionally compiled. Representative already-guarded sites,
each confirmed to carry an `#ifdef _WIN32`/`Q_OS_WIN` (most with a POSIX `#else`):

| Area | File:line | Guard shape |
|---|---|---|
| Logging path | `src/app/LogHandler.cpp:21-25` | `#ifdef _WIN32 … #else … #endif` |
| Process mitigations (DEP/ASLR/CFG etc.) | `src/app/ProcessMitigations.cpp:3-5, 9-23, 27-78, 81-177` | guarded; `applyProcessMitigations()` is a no-op off-Windows (`src/main.cpp:38`) |
| Crash handler (MiniDumpWriteDump) | `src/app/CrashHandler.cpp:11-12` | `#ifdef _WIN32` |
| Power events (WM_POWERBROADCAST) | `src/app/PowerEvents.cpp:9-11,26-33,144-160` | guarded, with `#else` |
| Window manager | `src/app/WindowManager.cpp:36-42` | `#ifdef Q_OS_WIN … #else … #endif` |
| Core Document (temp/rename/handles) | `src/core/Document.cpp:36-48, 74-98, 387-400, 459-467, 1062-1066, 4055-4118, 4536-4543` | guarded, mostly with `#else` |
| Temp-file safety (ACLs, bcrypt) | `src/core/TempFileSafety.cpp` (≈20 guarded regions) | guarded, mostly with `#else` |
| Atomic file replace | `src/core/AtomicFileReplace.cpp:18-23` (`aclapi.h`,`bcrypt.h`) | under `_WIN32` |
| Path security | `src/core/PathSecurity.cpp:9-19, 208-269, 445-467` | guarded |
| Zone.Identifier / Mark-of-the-Web | `src/core/ZoneIdentifier.cpp:12-30,37-59`; `src/attachments/AttachmentManager.cpp:154-159` | guarded |
| Secure wipe | `src/core/SecureWipe.h:22-44` | guarded with `#else` |
| Sleep inhibitor | `src/core/SleepInhibitor.cpp:4-12,18-42,47-61` | guarded with `#else` |
| System font discovery | `src/core/SystemFontDiscovery.cpp:7-14,25-131,144-283` | guarded with `#else` |
| Preflight open (out-of-proc) | `src/core/PreflightOpen.cpp:8-29,53-89,114-170,182-313` | guarded; includes `#elif defined(__APPLE__)` (`:59`) |
| Font resolver | `src/fonts/FontResolver.cpp:13-16` | under `_WIN32` |
| Encryption manager | `src/encryption/EncryptionManager.cpp:21-24` | under `_WIN32` |
| Search index | `src/search/DocumentSearchIndex.cpp` (≈8 guarded regions, with `#else`) | guarded |
| Header/footer engine | `src/headerfooter/HeaderFooterEngine.cpp:69-73` | guarded with `#else` |
| Text-edit tool | `src/textedit/TextEditTool.cpp:38-50` | `#if defined(_WIN32) \|\| defined(__APPLE__) … #else` |
| Signature chain (CRL/OCSP/TSA/DocMDP) | `src/signatures/CrlParser.cpp:65-69`, `OcspResponseParser.cpp:77-81`, `TsaClient.cpp:15-18`, `DocMdpDirectWriteGuard.cpp:72-86` | guarded with `#else` |
| CNN font classifier | `src/tre/CNNFontClassifier.cpp:109-114,172-176` | guarded with `#else` |
| `main()` entry point | `src/main.cpp` | fully portable; mitigations are a documented no-op (`:35-39`) |

The accessibility helper (`src/ui/AccessibilityHelper.cpp:8-10`) and the
auto-saver (`src/core/AutosaveManager.cpp:27-28`) are also guarded.

### 2.1 Confirmed un-guarded sites that will break a Linux compile/link

These are the concrete porting defects found — the ones a first `cmake --build`
on Linux will hit:

1. **`src/ui/MainWindow.cpp:234` — `#include <io.h>` is NOT guarded.** `<io.h>` is
   a Windows/MSVC header with no Linux equivalent; the POSIX counterpart is
   `<unistd.h>`. The `windows.h` include just below it (`:240-248`) *is* guarded;
   the `io.h` include was left outside the guard.
2. **`src/ui/MainWindow.cpp:15782` — `::_close(fd)` is in platform-neutral code**
   (the clipboard-image staging path, not inside any `_WIN32` block — context
   `:15772-15784`). `_close` is the MSVC CRT name; POSIX is `::close`. This needs a
   guard or a portable wrapper. The surrounding `openWriteFd()` seam appears
   portable, so only the close call is the issue here.

   These two are in the same translation unit and are the clearest "this will not
   compile on Linux" findings. Because almost everything else is guarded, the
   realistic expectation is a *small* set of such sites surfaced iteratively by
   the compiler (exactly what the aspirational Linux CI lane is scaffolded to
   catch), not a large porting backlog. A full compile on an actual Linux host is
   the only way to enumerate the complete list — see Open questions.

3. **`markOfTheWeb()` / Zone.Identifier is a silent no-op on Linux**
   (`src/attachments/AttachmentManager.cpp:152-160`): the body is entirely under
   `#if defined(_WIN32)` with no `#else`, so extracted attachments get no
   provenance marking on Linux. Compiles fine (the function is a no-op), but it is
   a *behavioural* gap. The Linux-equivalent provenance mechanism is the
   `user.xdg.origin.url` extended attribute, not implemented.

### 2.2 Windows-specific behaviours with no Linux equivalent yet

- **Native file dialog seam** (`src/main.cpp:41-54`): production uses the native
  Win32 `IFileOpenDialog`; the Qt dialog is a test-only override. On Linux Qt's
  own file dialog (GTK/portal) is the native path — functionally fine, but the
  comment's reasoning is Windows-specific.
- **File associations** are done entirely through the NSIS `.nsh` registry writes
  (`CMakeLists.txt:583-643`, `packaging/file_assoc*.nsh`). The Linux equivalent is
  a `.desktop` file + MIME `application/pdf` association via `xdg-mime` /
  `update-desktop-database` — none of which exists in the tree.
- **SmartScreen reputation submission** (`scripts/sign/Submit-SmartScreen.ps1`)
  and **Authenticode / Azure Trusted Signing** (§4) are Windows-only by nature.
- **Single-instance guard** uses `QLocalServer`/`QLocalSocket`
  (`src/app/SingleInstanceGuard.cpp`) — these are cross-platform Qt, so the
  single-instance / same-file data-loss protection should port without change.

---

## 3. Dependencies on Linux

All third-party deps are vcpkg ports with `x64-linux` triplet support. Status and
caveats drawn from the repo:

| Dependency | Manifest ref | Linux outlook |
|---|---|---|
| Qt6 (Widgets, Test, PrintSupport, Network, Svg, LinguistTools) | `vcpkg.json:9-16`, `CMakeLists.txt:19,31` | Builds on `x64-linux`. **Runtime needs the `xcb` platform plugin + an X server**; CI runs tests under `xvfb-run` with `QT_QPA_PLATFORM=xcb` (`linux-validation.yml:101-104,126`) because "vcpkg's Qt6 build only ships the xcb platform plugin" (`:105-106`). Wayland is not covered. |
| MuPDF (static) | `vcpkg.json:17-20` (`libmupdf 1.26.10#1`) | Cross-platform; AGPL (see §3.1). Note the reproducible-link concern (`src/app/CMakeLists.txt:83-142`) is MSVC-specific and does not apply. |
| QPDF (shared) | `vcpkg.json:21` | Cross-platform. |
| freetype / harfbuzz | `vcpkg.json:26-27` | Cross-platform; on Linux typically also available as system `-dev` packages. |
| sqlite3 | `vcpkg.json:27` (via `unofficial-sqlite3`) | Cross-platform. |
| Tesseract (+leptonica) | `vcpkg.json:28` | Builds on Linux; large dependency chain (`CMakeLists.txt:339` lists its Windows DLL deps). tessdata install is currently Windows-only (§1.1) and must be ported. |
| pugixml | `vcpkg.json:29` | Cross-platform. |
| OpenSSL | `CMakeLists.txt:40` | Cross-platform; on Linux typically the system libssl/libcrypto. |
| libcurl | `vcpkg.json:31-35` | **Already declared `!windows`-only in the manifest** with the `openssl` feature — i.e. the manifest expects vcpkg to supply curl on Linux. Used by `TsaClient` for TSA/HTTPS. |
| MuJS | `vcpkg.json` via `unofficial-mujs`; overlay at `tools/vcpkg-overlays/mujs` | Cross-platform. The overlay exists to repin a drifted codeberg 1.3.8 tarball hash (`linux-validation.yml:62-67`); the Linux CI lane sets `VCPKG_OVERLAY_PORTS` to use it. A `gumbo` overlay also exists (`tools/vcpkg-overlays/gumbo`). |
| ONNX Runtime | `CMakeLists.txt:54-104` | **Windows-prebuilt only.** Optional (re-ranker disables cleanly when absent). A Linux build gets no CNN re-ranker until a Linux ONNX Runtime is located; `src/app/CMakeLists.txt:349-351` assumes an apt/system install but no CMake locates it. |
| Rust/Cargo (search index) | `linux-validation.yml:85` adds `~/.cargo/bin` to PATH | The search index has a Rust/Tantivy component (`tests/test_tantivy_search.cpp`, `test_search_engine_tantivy_parity.cpp`); a Rust toolchain is a Linux build prerequisite. |

### 3.1 Licensing notes already in the repo

- Project license is **AGPL-3.0-only** (`vcpkg.json:6`, `pdfe.rc:23`). The AGPL
  corresponding-source obligation is already wired into the install tree
  (`CMakeLists.txt:510-518`, installs `LICENSE` + `CORRESPONDING-SOURCE.txt`) and
  is platform-neutral, so it carries over to any Linux package unchanged. MuPDF is
  the AGPL anchor dependency (per project memory and `vcpkg.json:17`).

---

## 4. Packaging + signing

### 4.1 Current Windows story (what must be replaced, not reused)

- **CPack NSIS installer** — `CPACK_GENERATOR "NSIS"` is set only inside
  `if(WIN32)` (`CMakeLists.txt:531-643`), with Start-menu/desktop shortcuts
  (`:576-581`), file-association `.nsh` includes (`:629-642`), admin elevation
  (documented at `:543-573`), and uninstall-before-install (`:541`).
- **Azure Trusted Signing (Authenticode)** — `scripts/sign/Build-Release-Signed.ps1`
  (end-to-end signed-release pipeline, `:1-38`), `Sign-Artifacts.ps1`,
  `Submit-SmartScreen.ps1`, plus `trusted-signing-metadata.json`. All PowerShell +
  Azure, inherently Windows. The script signs only PDFE's own binaries and leaves
  vendor DLLs untouched (`Build-Release-Signed.ps1:15-16`).
- Packaging is forced from the `windows-vs2022-package` tree with
  `PDFE_TESTING=OFF` (`CMakePresets.json:85-108`, `Build-Release-Signed.ps1:21-38`)
  so the test introspection seams are statically dead. **This posture discipline
  must be preserved on Linux**: whatever packaging path is built must also
  configure from a `PDFE_TESTING=OFF` tree, or the shipped Linux binary carries
  the `PDFE_TEST_PIPES` seam (the exact defect GitHub #182 fixed on Windows).

### 4.2 Linux-equivalent story (to be built)

- **Package format** — options, none currently implemented:
  - *AppImage* — closest analogue to the self-contained NSIS installer; bundles Qt
    + deps, no system install. Good fit given the app already deploys its own Qt
    plugins on Windows.
  - *.deb / .rpm* via `CPACK_GENERATOR "DEB"`/`"RPM"` — integrates with system
    package managers; would declare Qt/MuPDF/etc. as dependencies rather than
    bundling.
  - *Flatpak* — strong sandboxing/desktop-integration story; most work to set up.
- **Desktop integration** — a `.desktop` entry, an icon in the hicolor theme
  (the source `pdfe-red.ico` would need a PNG/SVG rendition), and an `xdg-mime`
  `application/pdf` association replacing the NSIS registry writes
  (`CMakeLists.txt:583-643`).
- **Install-tree gaps to port first** (§1.1): tessdata and the ONNX model install
  rules live inside `if(WIN32)` today and must be re-expressed for Linux, or OCR
  and the font re-ranker silently lose their data files in the package.
- **Signing** — Authenticode/Trusted-Signing/SmartScreen **do not apply**. The
  Linux analogues are GPG-signed packages/repos (`.deb`/`.rpm` signing,
  `debsign`/`rpm --addsign`), and/or sigstore/cosign for artifacts. AppImages are
  conventionally signed with an embedded GPG signature. None exists yet.

---

## 5. Testing on Linux

### 5.1 GoogleTest suite — largely portable, already CI-scaffolded

- The primary suite (`tests/test_*.cpp`, thousands of tests) is cross-platform C++
  and is what the Linux CI lane builds and runs
  (`linux-validation.yml:98-171`). The lane runs the binary directly under
  `xvfb-run` and asserts a *non-vacuous* result (tests>0, failures=0, errors=0)
  from the JUnit XML (`:135-171`), plus a **cross-platform core-parity suite**
  (`:173-203`) driven by `tools/gate/parity_filter.py` + `tools/gate/parity_suite.txt`
  (189 entries) — a shared ~200+-test contract that must pass identically on
  Windows, Linux, and macOS.
- **Windows-only test code is already `if(WIN32)`-gated** in
  `tests/CMakeLists.txt`:
  - `:1482-1492` links `iphlpapi ws2_32 psapi crypt32` and builds the tests that
    use them (process-working-set / live Windows cert-store tests) only on Windows.
  - `:1595-1624` (and `:1717`) stage Qt plugin DLLs only on Windows (unnecessary on
    Linux per `src/app/CMakeLists.txt:241-242`).
- Tests that are inherently Windows and will need `if(WIN32)` scoping or Linux
  analogues if not already gated: `test_installer.cpp` (NSIS installer),
  `test_signing_and_install_docs.cpp`, the `pdfe.rc` version-string test
  (`test_app_resources.cpp` / VersionInfo), and anything asserting registry
  file-association or the launch mutex. These should be audited during the port;
  the existing `if(WIN32)` blocks show the established pattern.

### 5.2 UI-automation layer — Windows-only, needs a Linux substitute

Per `CLAUDE.md`, the native UI-automation stack is **pywinauto / WinAppDriver /
FlaUI / PyAutoGUI**, all Windows-UIA/Win32 based, plus the Win32
`EnumWindows`-based modal helper (`tests/helpers/modal_dialog.py`) and the
`PDFE_TEST_PIPES` UIA-workaround seam (`CMakeLists.txt:127-163`). None of this
runs on Linux. A Linux UI-automation substitute would be:

- **AT-SPI2 + dogtail / pyatspi** — the Linux accessibility-bus analogue of UIA;
  Qt exposes widgets via `QAccessible` (already used — `AccessibilityHelper.cpp`,
  the `PdfCanvasAccessible` factory in `main.cpp:105`), so the `objectName` /
  `accessibleName` discipline the project already follows maps onto AT-SPI.
- **xdotool / ydotool** (or `pyautogui` on X) for coordinate/keyboard gestures and
  the PDF-canvas raster interactions.
- **Xvfb** (already used by the gtest lane) or a headless Wayland compositor for a
  virtual display.

This is the single largest *net-new* test effort in the port: the entire
`tests/test_ui_*.py` layer and its helpers (`tests/helpers/modal_dialog.py`,
`tests/helpers/canvas_geometry.py`) would need a parallel AT-SPI/xdotool
implementation, because the UIA seam and the Win32 window-enumeration helper have
no Linux meaning.

---

## 6. CI

- CI is **self-hosted and manual**. Windows lanes run on a self-hosted runner;
  the Linux and macOS lanes are `workflow_dispatch`-only and target an **ephemeral
  Hetzner VM** provisioned on demand by `tests/scripts/hetzner_ephemeral_runner.ps1`
  and destroyed afterward (`linux-validation.yml:25-58`). There is no runner on a
  timer, which is why the lane is dispatch-only (`:31-38`).
- The Linux lane (`linux-validation.yml`) is a **ready-made build/test scaffold**:
  checkout → vcpkg (`x64-linux`, GHA binary cache) → `cmake -G Ninja` with
  `clang-15` → `ninja` → `xvfb-run pdfe_tests` → non-vacuous XML gate → parity
  suite. **It is permanently red today** and the file itself warns
  (`:8-14`) that a red run here is expected and must NOT be read as a regression,
  but also that a permanently-red lane can *mask* a real Linux breakage — so
  "establish a green baseline here" is defined as the first task of the actual
  port.
- A heavier `hetzner-sanitizer.yml` (MSan/ASan on Linux) is gated behind the basic
  lane going green (`:22-23`) and behind tracker #42 (`:46-49`); MSan has no
  Windows equivalent, so the Linux lane is also the only route to closing those
  sanitizer gaps.
- Requirements for a working Linux lane beyond what exists: a persistent (or
  reliably provisioned) Linux runner with Qt xcb/X deps, a Rust toolchain
  (already added to PATH at `:85`), and resolution of the gcc-vs-clang-15
  toolchain question (§1.3).

---

## 7. Effort / risk summary

### Biggest blockers (ranked)

1. **UI-automation rewrite (largest effort).** The pywinauto/WinAppDriver/FlaUI/
   PyAutoGUI layer plus the UIA-workaround seam and Win32 modal helper are wholly
   Windows-specific; a full AT-SPI/dogtail + xdotool + Xvfb substitute is net-new
   work. The gtest suite ports largely for free, but the end-to-end UI matrix does
   not.
2. **Packaging + signing from scratch.** NSIS/CPack-NSIS, Azure Trusted Signing,
   SmartScreen, and the registry-based file associations have zero Linux reuse.
   Need AppImage/.deb/Flatpak + `.desktop`/MIME + GPG/sigstore, *and* the
   `PDFE_TESTING=OFF` packaging-tree discipline must be reproduced so the Linux
   binary doesn't ship the test seam (GitHub #182).
3. **Install-tree data files (tessdata, ONNX model) are Windows-gated** and would
   be dropped from a Linux package, silently disabling OCR language data and the
   CNN re-ranker (`CMakeLists.txt:297-409`).
4. **ONNX Runtime has no Linux provider** (`CMakeLists.txt:54-104`). Lower risk
   because it degrades gracefully to the heuristic matcher, but it is a real
   feature loss until wired.
5. **Unknown long tail of unguarded source sites.** Two are confirmed
   (`MainWindow.cpp:234` `<io.h>`, `:15782` `::_close`); the true count is only
   knowable by compiling on Linux. Risk is "iterative compiler whack-a-mole,"
   bounded by how thoroughly the `#ifdef _WIN32` discipline was already applied —
   which, from the survey, is very thorough.

### Quick wins

- The CMake scaffolding is **already there**: `linux-gcc` preset, `linux-*` build/
  test presets, `x64-linux` manifest support, a standing Linux CI lane, and
  `!windows` curl handling. First configure+build is a low-friction starting point.
- Most source is already guarded with POSIX `#else` branches, so a large fraction
  of the port is "already done, just never compiled."
- Qt accessibility is already in use (`QAccessible`, `objectName` discipline),
  which is the same substrate AT-SPI needs — the UI-automation rewrite starts from
  an accessible tree, not from scratch.
- `SingleInstanceGuard` (QLocalServer/QLocalSocket) and `main()` are already
  portable.

### Open questions / unknowns (need a human decision or a real Linux build)

1. **Toolchain: gcc or clang?** The `linux-gcc` preset says `gcc`
   (`CMakePresets.json:150-151`) but the CI lane uses `clang-15`
   (`linux-validation.yml:89-90`). Pick one as the supported Linux compiler.
2. **Complete list of unguarded source sites is unknown** without an actual Linux
   compile. Only `MainWindow.cpp:234` and `:15782` are confirmed here; a real
   `cmake --build` on `x64-linux` is the only way to enumerate the rest.
3. **ONNX Runtime on Linux** — is the CNN re-ranker in scope for the Linux build
   at all? If yes, a Linux ONNX Runtime must be located (apt/system vs. a pinned
   prebuilt mirroring the Windows SHA-pin approach). No CMake code locates it today
   despite the comment at `src/app/CMakeLists.txt:349-351`.
4. **Target package format** — AppImage vs .deb/.rpm vs Flatpak is unresolved;
   each implies different desktop-integration and signing work.
5. **Display server** — the gtest lane assumes xcb/Xvfb (vcpkg Qt ships only the
   xcb plugin, `linux-validation.yml:105-106`). Whether the shipped app must also
   support Wayland natively is undecided.
6. **Distro / glibc baseline** — no target distribution or minimum glibc is stated
   anywhere in the tree; needed to scope the vcpkg build host and package
   compatibility.
7. **Reproducible-build parity** — the repro-link / cache-pin machinery is
   Windows-only (`x64-windows-repro` triplet; `/Brepro` at
   `src/app/CMakeLists.txt:143-154`). Whether Linux needs an equivalent
   `x64-linux-repro` triplet is an open decision.
8. **Mark-of-the-Web / provenance on Linux** — `markOfTheWeb` is a silent no-op
   off-Windows (`AttachmentManager.cpp:152-160`); decide whether the
   `user.xdg.origin.url` xattr equivalent is required for parity.

---

*Every claim above is cited to the tree as of this writing; nothing here has been
verified by an actual Linux build, which does not yet exist. The confirmed
compile-breakers and the dependency/packaging gaps are the load-bearing findings;
the unguarded-source long tail is bounded but not enumerated.*
