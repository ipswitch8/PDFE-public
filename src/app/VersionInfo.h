// -*- coding: utf-8 -*-
//
// VersionInfo — single source of truth for the running build's version plus a
// pure, network-free semantic-version comparison used by the (future) update /
// version-check channel.
//
// R3-16 (code-signing / no-auto-update posture). Signing status, kept in lock-
// step with docs/security/unsigned-binary-risk-acceptance.md (whose filename is
// historical) and enforced by tests/test_signing_and_install_docs.cpp:
//
// SIGNING-STATUS: SIGNED-MANUAL-ONLY
// SIGNING-STATUS-COMMIT: 92e1328
//
// Release artifacts (pdfe.exe, pdfe_preflight.exe, PDFE-<version>-win64.exe) ARE
// Authenticode-signed as "Itwerx Corp" via Azure Trusted Signing, Microsoft-
// rooted and RFC-3161 countersigned, since commit 92e1328 (2026-07-18). Signing
// is a MANUAL release step (scripts/sign/Build-Release-Signed.ps1) with no CI
// gate, and there is still no auto-update channel; both are recorded as accepted
// risks in the document above.
// This header partially *addresses* the "no version-check channel" half by
// providing the comparison core of an update channel. The network fetch of the
// "latest available" string is deliberately NOT implemented here, so the logic
// stays pure and unit-testable with no I/O.
//
// UPDATED 2026-09-06 (GitHub #127): the fetch is no longer absent from the
// product — it lives in src/app/UpdateChecker.cpp, which supplies the "latest"
// string this header compares against. It remains deliberately NOT implemented
// *here* for the reason above: this file must stay free of I/O.
//
// That fetch is a NOTIFY-ONLY check and is off by default. It never downloads
// or executes anything, which is what keeps it outside the UC-1..UC-8 update-
// channel requirements; the boundary is recorded normatively as UC-9 in
// docs/code-signing.md. PDFE still has no update channel.
//
// Version source of truth
// -----------------------
// The canonical version lives in the top-level CMakeLists.txt:
//     project(pdfe VERSION 0.1.0 ...)
// which also drives CPACK_PACKAGE_VERSION_* and the Win32 VERSIONINFO block in
// src/app/pdfe.rc. To thread that same value into C++ WITHOUT a second literal,
// add to src/app/CMakeLists.txt (see the file's report / registration note):
//     target_compile_definitions(pdfe_app PUBLIC
//         PDFE_VERSION_STRING="${PROJECT_VERSION}")
// When that define is present, currentVersion() returns exactly PROJECT_VERSION.
// The literal fallback below is kept in lock-step with project() so the library
// still reports the correct version even before the define is wired, and the
// unit test cross-checks the two against the PDFE_VERSION_STRING macro.

#pragma once

#include <string>

namespace pdfe::app {

// Result of comparing the running build's version against a candidate "latest"
// version obtained from an update channel.
enum class UpdateStatus {
    UpToDate,         // current == latest
    UpdateAvailable,  // current <  latest  (a newer release exists)
    Newer             // current >  latest  (running a pre-release / dev build)
};

// The version of this build. Backed by the PDFE_VERSION_STRING compile
// definition (threaded from CMake's PROJECT_VERSION) with a literal fallback
// kept equal to project(pdfe VERSION ...).
std::string currentVersion();

// Pure semantic-version comparison. No I/O, no globals.
//
// Semantics:
//   * Dot-separated numeric segments compared most-significant first.
//   * Unequal segment counts are handled by zero-padding the shorter one
//     ("1.2" == "1.2.0").
//   * Leading zeros are irrelevant ("1.02" == "1.2").
//   * An optional leading 'v'/'V' is ignored ("v1.0.0" == "1.0.0").
//   * A pre-release / build-metadata suffix ('-' or '+') terminates the core
//     version and is ignored for this comparison ("1.0.0-beta" == "1.0.0").
//   * Non-numeric junk in a segment is guarded: the leading digit run is used
//     ("1a" -> 1) and a digit-less segment counts as 0. Never throws.
UpdateStatus compareVersions(const std::string& current, const std::string& latest);

// Convenience wrapper: compare THIS build against a "latest" string supplied by
// a caller (e.g. a future update channel / release-feed fetcher).
UpdateStatus checkForUpdate(const std::string& latestFromChannel);

}  // namespace pdfe::app
