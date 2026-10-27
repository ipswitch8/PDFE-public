// -*- coding: utf-8 -*-
//
// Meta-audit 2026-07 follow-up, phase F13 — supply-chain inventory integrity
// (SEC-76 / SEC-77 / SEC-78 / SEC-79 / SEC-83 / SEC-86).
//
// THE FINDING. tests/security/reports/deps/sbom.spdx.json listed 191 packages
// yet OMITTED four of the eleven direct dependencies declared in vcpkg.json —
// qtsvg, tesseract, pugixml and curl — and the file was .gitignore'd, so
// nothing could review it, diff it across releases, or assert on it. An SBOM
// that silently drops a direct dependency answers "not affected" to a CVE
// triage for a component the product actually ships. That is the most
// dangerous kind of wrong: it looks like coverage.
//
// WHAT F13 DID. The SBOM is now COMMITTED (an exception in .gitignore) and
// repaired; scripts/security/run_deps.ps1 gained a hard completeness gate so a
// future regeneration fails closed instead of dropping a dependency again;
// scripts/security/sbom_direct_deps.py is the committed reconciliation check
// that emits the difference report. This file is the standing assertion over
// all of it, plus the two overlay-port pins (SEC-83 / SEC-86) that keep the
// #50 JPX decode-gate patch from being silently lost in a version bump.
//
// Everything here is text-over-source: no build, no network, no admin.
#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace {

std::filesystem::path projectRootDir()
{
    return std::filesystem::path(TEST_FIXTURES_DIR).parent_path().parent_path();
}

std::string slurp(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return "";
    return std::string(std::istreambuf_iterator<char>(file),
                       std::istreambuf_iterator<char>());
}

bool has(const std::string& hay, const std::string& needle)
{
    return hay.find(needle) != std::string::npos;
}

// Drop whole-line '#' comments so a commented-out directive cannot satisfy an
// active-config assertion (the mutation-survivor hardening idiom used across
// the CI static-config guards).
std::string stripHashComments(const std::string& src)
{
    std::string out;
    std::string cur;
    for (char c : src) {
        if (c == '\n') {
            const size_t p = cur.find_first_not_of(" \t");
            if (p == std::string::npos || cur[p] != '#') { out += cur; out += '\n'; }
            cur.clear();
        } else if (c != '\r') {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) {
        const size_t p = cur.find_first_not_of(" \t");
        if (p == std::string::npos || cur[p] != '#') out += cur;
    }
    return out;
}

// Direct dependency names declared in vcpkg.json's "dependencies" array,
// handling both the bare-string form ("qtsvg") and the object form
// ({"name": "libmupdf", "version>=": ...}). A deliberately small scan rather
// than a JSON dependency: this test reads the AUTHORED manifest as a human
// reviewer would.
std::vector<std::string> vcpkgDirectDependencies(const std::string& manifest)
{
    std::vector<std::string> names;
    const size_t depsAt = manifest.find("\"dependencies\"");
    if (depsAt == std::string::npos) return names;
    const size_t open = manifest.find('[', depsAt);
    if (open == std::string::npos) return names;

    int bracket = 0;      // [] nesting inside the dependencies array
    int brace = 0;        // {} nesting: we are inside an object dependency
    bool expectName = false;
    for (size_t i = open; i < manifest.size(); ++i) {
        const char c = manifest[i];
        if (c == '[') { ++bracket; continue; }
        if (c == ']') { if (--bracket == 0) break; continue; }
        if (c == '{') { ++brace; continue; }
        if (c == '}') { --brace; continue; }
        if (c != '"') continue;

        const size_t close = manifest.find('"', i + 1);
        if (close == std::string::npos) break;
        const std::string token = manifest.substr(i + 1, close - i - 1);
        i = close;

        if (expectName) { names.push_back(token); expectName = false; continue; }
        if (brace == 0 && bracket == 1) {
            // A bare string element of the dependencies array IS a dependency.
            names.push_back(token);
            continue;
        }
        if (brace > 0 && token == "name") { expectName = true; continue; }
        // Any other token inside an object (keys and their values: version>=,
        // features, platform, host, default-features, feature names) is not a
        // dependency name.
    }
    return names;
}

// name -> versionInfo for every SPDX "packages" entry.
std::map<std::string, std::string> spdxPackages(const std::string& sbom)
{
    std::map<std::string, std::string> out;
    size_t pos = 0;
    while (true) {
        const size_t nameAt = sbom.find("\"name\":", pos);
        if (nameAt == std::string::npos) break;
        const size_t nq = sbom.find('"', nameAt + 7);
        if (nq == std::string::npos) break;
        const size_t nqe = sbom.find('"', nq + 1);
        if (nqe == std::string::npos) break;
        const std::string name = sbom.substr(nq + 1, nqe - nq - 1);
        pos = nqe + 1;

        // versionInfo belongs to the same JSON object; look in a bounded
        // window around the name so a neighbouring package cannot donate one.
        std::string version;
        const size_t winStart = (nameAt > 500) ? nameAt - 500 : 0;
        const size_t winEnd = std::min(sbom.size(), nqe + 500);
        const std::string window = sbom.substr(winStart, winEnd - winStart);
        const size_t vAt = window.find("\"versionInfo\":");
        if (vAt != std::string::npos) {
            const size_t vq = window.find('"', vAt + 14);
            if (vq != std::string::npos) {
                const size_t vqe = window.find('"', vq + 1);
                if (vqe != std::string::npos)
                    version = window.substr(vq + 1, vqe - vq - 1);
            }
        }
        out[name] = version;
    }
    return out;
}

std::string joinList(const std::vector<std::string>& v)
{
    std::string out;
    for (const auto& s : v) { if (!out.empty()) out += ", "; out += s; }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// SEC-76 / SEC-78 / SEC-79 — the committed SBOM must enumerate every direct
// dependency, each with a version readable from the tree.
// ---------------------------------------------------------------------------
TEST(SupplyChainInventory, SbomEnumeratesEveryDirectVcpkgDependencyWithAVersion)
{
    const std::string manifest = slurp(projectRootDir() / "vcpkg.json");
    ASSERT_FALSE(manifest.empty()) << "vcpkg.json must be readable";

    const std::filesystem::path sbomPath = projectRootDir() / "tests" /
        "security" / "reports" / "deps" / "sbom.spdx.json";
    const std::string sbom = slurp(sbomPath);
    ASSERT_FALSE(sbom.empty())
        << "tests/security/reports/deps/sbom.spdx.json must be COMMITTED and "
           "readable (SEC-78). An ignored SBOM cannot be reviewed, diffed "
           "across releases, or asserted on — which is exactly how it came to "
           "omit four direct dependencies unnoticed.";

    const std::vector<std::string> declared = vcpkgDirectDependencies(manifest);
    ASSERT_GE(declared.size(), 10u)
        << "the vcpkg.json direct-dependency scan produced only "
        << declared.size() << " names (" << joinList(declared) << "). The "
           "manifest shape changed under this check and the assertion below "
           "would pass vacuously.";

    const std::map<std::string, std::string> packages = spdxPackages(sbom);
    ASSERT_GE(packages.size(), 50u)
        << "the SBOM package scan produced only " << packages.size()
        << " names; the document shape changed under this check";

    std::vector<std::string> missing;
    std::vector<std::string> versionless;
    for (const std::string& dep : declared) {
        const auto it = packages.find(dep);
        if (it == packages.end()) { missing.push_back(dep); continue; }
        if (it->second.empty() || it->second.rfind("unknown", 0) == 0)
            versionless.push_back(dep + "=\"" + it->second + "\"");
    }

    EXPECT_TRUE(missing.empty())
        << "the SBOM omits " << missing.size() << " DIRECT dependency/ies: "
        << joinList(missing)
        << "\nA CVE triage run against this SBOM would report 'not affected' "
           "for a component PDFE ships (SEC-76). Regenerate with "
           "scripts/security/run_deps.ps1 (which now fails closed on exactly "
           "this) or repair with: python scripts/security/sbom_direct_deps.py "
           "--fill <vcpkg installed/vcpkg/status> --check";
    EXPECT_TRUE(versionless.empty())
        << "these direct dependencies appear in the SBOM without a usable "
           "version: " << joinList(versionless) << " (SEC-79)";
}

// ---------------------------------------------------------------------------
// SEC-77 — the GENERATOR must fail closed. Without this the assertion above
// can be satisfied once by hand and silently regress on the next regeneration.
// ---------------------------------------------------------------------------
TEST(SupplyChainInventory, DepsScriptFailsClosedOnAnIncompleteSbom)
{
    const std::string ps = stripHashComments(
        slurp(projectRootDir() / "scripts" / "security" / "run_deps.ps1"));
    ASSERT_FALSE(ps.empty()) << "scripts/security/run_deps.ps1 must be readable";

    EXPECT_TRUE(has(ps, "$missingDirect"))
        << "run_deps.ps1 must compute the declared-but-missing direct "
           "dependency set BEFORE writing the SBOM (SEC-77)";
    EXPECT_TRUE(has(ps, "exit 2"))
        << "run_deps.ps1 must EXIT NON-ZERO when a direct dependency is absent "
           "from the SBOM. A warning is not a gate — the previous version "
           "warned about missing transitives and shipped the SBOM anyway.";

    const std::string checker = slurp(
        projectRootDir() / "scripts" / "security" / "sbom_direct_deps.py");
    EXPECT_FALSE(checker.empty())
        << "scripts/security/sbom_direct_deps.py must be committed: it is the "
           "check that compares vcpkg.json against the SBOM and emits the "
           "difference report referenced by the phase-13 gate artifacts";
    EXPECT_TRUE(has(checker, "missing_from_sbom"))
        << "the checker must name which direct dependencies are missing";
    EXPECT_TRUE(has(checker, "present_but_versionless"))
        << "the checker must also catch a dependency listed WITHOUT a version";
}

// ---------------------------------------------------------------------------
// SEC-83 — the MuPDF overlay's JPX decode-gate patch is a SECURITY CONTROL
// (issue #50 / DCA-0011: an OpenJPEG heap overflow, PDFE's only RCE-class
// defect). Losing it in a version bump re-opens a heap-corruption path and
// nothing about the build looks different.
// ---------------------------------------------------------------------------
TEST(SupplyChainInventory, MupdfOverlayStillAppliesTheJpxDecodeGatePatch)
{
    const std::filesystem::path portDir =
        projectRootDir() / "vcpkg-overlay-ports" / "libmupdf";
    const std::string rawPortfile = slurp(portDir / "portfile.cmake");
    ASSERT_FALSE(rawPortfile.empty())
        << "the libmupdf overlay portfile must exist";
    const std::string portfile = stripHashComments(rawPortfile);

    EXPECT_TRUE(has(portfile, "0001-pdfe-jpx-decode-gate.patch"))
        << "vcpkg-overlay-ports/libmupdf/portfile.cmake no longer applies "
           "0001-pdfe-jpx-decode-gate.patch (and not merely in a comment). "
           "That patch gates JPEG2000 decoding OFF by default BEFORE OpenJPEG "
           "is entered (#50 / DCA-0011). Dropping it re-accepts a "
           "heap-corruption / RCE path on untrusted input. Re-base the patch; "
           "do not remove it. (SEC-83)";

    EXPECT_TRUE(std::filesystem::exists(portDir / "0001-pdfe-jpx-decode-gate.patch"))
        << "the JPX decode-gate patch FILE is missing from the overlay port; a "
           "portfile referencing an absent patch fails the port build, but the "
           "reference alone is not evidence the control exists";

    EXPECT_TRUE(has(rawPortfile, "SECURITY CONTROL"))
        << "the portfile must keep the 'THIS PATCH IS A SECURITY CONTROL' note "
           "beside the patch: an unexplained filename is what gets deleted "
           "during a routine version bump";
}

// ---------------------------------------------------------------------------
// SEC-56 (GitHub #56) — the second decode-gate patch, same standing as the JPX
// one above: jbig2dec has a long history of memory-safety CVEs and MuPDF's
// fz_try/fz_catch cannot trap the heap corruption they produce.
// ---------------------------------------------------------------------------
TEST(SupplyChainInventory, MupdfOverlayStillAppliesTheJbig2DecodeGatePatch)
{
    const std::filesystem::path portDir =
        projectRootDir() / "vcpkg-overlay-ports" / "libmupdf";
    const std::string rawPortfile = slurp(portDir / "portfile.cmake");
    ASSERT_FALSE(rawPortfile.empty())
        << "the libmupdf overlay portfile must exist";
    const std::string portfile = stripHashComments(rawPortfile);

    EXPECT_TRUE(has(portfile, "0002-pdfe-jbig2-decode-gate.patch"))
        << "vcpkg-overlay-ports/libmupdf/portfile.cmake no longer applies "
           "0002-pdfe-jbig2-decode-gate.patch (and not merely in a comment). "
           "That patch gates JBIG2 decoding OFF by default BEFORE jbig2dec is "
           "entered (SEC-56 / #56). Dropping it re-accepts a heap-corruption "
           "path on untrusted input. Re-base the patch; do not remove it.";

    EXPECT_TRUE(std::filesystem::exists(portDir / "0002-pdfe-jbig2-decode-gate.patch"))
        << "the JBIG2 decode-gate patch FILE is missing from the overlay port; a "
           "portfile referencing an absent patch fails the port build, but the "
           "reference alone is not evidence the control exists";

    // The patch must still gate BOTH doors into jbig2dec — the PDF stream
    // filter and the standalone loader. A rebase that keeps only one of the two
    // files leaves a fully open path while every other check here stays green.
    const std::string patch = slurp(portDir / "0002-pdfe-jbig2-decode-gate.patch");
    ASSERT_FALSE(patch.empty());
    EXPECT_TRUE(has(patch, "source/fitz/filter-jbig2.c"))
        << "the patch must gate the PDF /JBIG2Decode stream filter";
    EXPECT_TRUE(has(patch, "source/fitz/load-jbig2.c"))
        << "the patch must gate the standalone JBIG2 image loader too";
}

// ---------------------------------------------------------------------------
// SEC-86 — vcpkg.json's libmupdf constraint and the overlay port's own version
// must not drift. If the manifest demands a version the overlay no longer
// provides, vcpkg resolves to the REGISTRY port and silently loses the patch
// asserted above.
// ---------------------------------------------------------------------------
TEST(SupplyChainInventory, MupdfManifestConstraintMatchesTheOverlayPortVersion)
{
    const std::string manifest = slurp(projectRootDir() / "vcpkg.json");
    const std::string overlay = slurp(projectRootDir() / "vcpkg-overlay-ports" /
                                      "libmupdf" / "vcpkg.json");
    ASSERT_FALSE(manifest.empty());
    ASSERT_FALSE(overlay.empty());

    const size_t vAt = manifest.find("\"version>=\"");
    ASSERT_NE(vAt, std::string::npos)
        << "vcpkg.json must pin libmupdf with a version>= constraint";
    const size_t q1 = manifest.find('"', manifest.find(':', vAt));
    ASSERT_NE(q1, std::string::npos);
    const size_t q2 = manifest.find('"', q1 + 1);
    ASSERT_NE(q2, std::string::npos);
    const std::string constraint = manifest.substr(q1 + 1, q2 - q1 - 1);

    const std::string constraintBase = constraint.substr(0, constraint.find('#'));
    long constraintPortVersion = 0;
    if (constraint.find('#') != std::string::npos)
        constraintPortVersion = std::stol(constraint.substr(constraint.find('#') + 1));

    const size_t ovAt = overlay.find("\"version\"");
    ASSERT_NE(ovAt, std::string::npos);
    const size_t oq1 = overlay.find('"', overlay.find(':', ovAt));
    ASSERT_NE(oq1, std::string::npos);
    const size_t oq2 = overlay.find('"', oq1 + 1);
    ASSERT_NE(oq2, std::string::npos);
    const std::string overlayVersion = overlay.substr(oq1 + 1, oq2 - oq1 - 1);

    long overlayPortVersion = 0;
    const size_t pvAt = overlay.find("\"port-version\"");
    if (pvAt != std::string::npos) {
        size_t d = overlay.find(':', pvAt) + 1;
        while (d < overlay.size() && std::isspace(static_cast<unsigned char>(overlay[d]))) ++d;
        size_t e = d;
        while (e < overlay.size() && std::isdigit(static_cast<unsigned char>(overlay[e]))) ++e;
        if (e > d) overlayPortVersion = std::stol(overlay.substr(d, e - d));
    }

    EXPECT_EQ(constraintBase, overlayVersion)
        << "vcpkg.json requires libmupdf " << constraintBase << " but the "
           "overlay port — the one carrying the JPX decode-gate security patch "
           "— provides " << overlayVersion << ". A mismatch makes vcpkg resolve "
           "to the REGISTRY port and silently drops the patch. (SEC-86)";
    EXPECT_GE(overlayPortVersion, constraintPortVersion)
        << "the overlay port-version (" << overlayPortVersion << ") is BELOW "
           "the constraint in vcpkg.json (#" << constraintPortVersion << "), so "
           "the overlay cannot satisfy the manifest and the registry port wins";
}
