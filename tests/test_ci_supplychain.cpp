// test_ci_supplychain.cpp
//
// Phase 8 (CI honesty + build portability + supply-chain) static-config guards.
// These tests parse the AUTHORED CI / build / installer sources from the source
// tree (no build, no network, no admin) and assert the R2-24 / R2-26 / R2-27
// hardening is present and structurally correct.
//
//   R2-24  linux-validation.yml invokes the built pdfe_tests binary DIRECTLY
//          (not the vacuous-green `ctest` discovery path) and FAILS on a
//          zero/partial run (asserts tests>0, failures=0, errors=0 from the XML).
//   R2-26  CMakePresets.json uses $env{VCPKG_ROOT} for every vcpkg toolchain
//          file -- no hardcoded per-developer absolute vcpkg path remains.
//   R2-27  scripts/security/install_tools.ps1 verifies the SHA256 of every
//          fetched scanner binary against a pinned digest (Get-FileHash).
//
// Source files are located relative to the project root, itself derived from the
// TEST_FIXTURES_DIR compile define (see tests/test_installer.cpp).

#include <gtest/gtest.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace {

std::filesystem::path getProjectRoot() {
    return std::filesystem::path(TEST_FIXTURES_DIR).parent_path().parent_path();
}

std::string readFileContents(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return "";
    return std::string(std::istreambuf_iterator<char>(file),
                       std::istreambuf_iterator<char>());
}

std::vector<std::string> splitLines(const std::string& contents) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : contents) {
        if (c == '\n') { lines.push_back(cur); cur.clear(); }
        else if (c != '\r') { cur.push_back(c); }
    }
    lines.push_back(cur);  // trailing partial line (may be empty)
    return lines;
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// Drop whole-line comments (first non-space char is '#') so a commented-out
// directive cannot satisfy an ACTIVE-config substring assertion. Both YAML and
// PowerShell use '#'. Mutation-survivor hardening: commenting out the fix must
// re-red the oracle, not silently pass.
std::string stripHashComments(const std::string& src) {
    std::string out;
    for (const std::string& line : splitLines(src)) {
        size_t p = line.find_first_not_of(" \t");
        if (p != std::string::npos && line[p] == '#') continue;
        out += line;
        out += '\n';
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// R2-26 : CMakePresets.json vcpkg toolchain portability
// ---------------------------------------------------------------------------

// Every CMAKE_TOOLCHAIN_FILE value that points at vcpkg.cmake must resolve via
// $env{VCPKG_ROOT}. A hardcoded per-developer absolute path (e.g.
// C:/Users/<name>/source/vcpkg/...) is non-portable and must not appear. This
// is the core R2-26 oracle: it walks EVERY line naming vcpkg.cmake and requires
// the env-var form.
TEST(CiSupplyChain, PresetsUseVcpkgRootForEveryToolchainFile) {
    std::string presets = readFileContents(getProjectRoot() / "CMakePresets.json");
    ASSERT_FALSE(presets.empty()) << "CMakePresets.json must be readable";

    int toolchainLines = 0;
    for (const std::string& line : splitLines(presets)) {
        if (!contains(line, "vcpkg.cmake")) continue;
        ++toolchainLines;
        EXPECT_TRUE(contains(line, "$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"))
            << "Non-portable vcpkg toolchain path (must use $env{VCPKG_ROOT}): " << line;
    }
    // The file historically wires several presets (default, windows-vs2022,
    // two asan, linux, macos) -- ensure the oracle actually inspected some.
    EXPECT_GE(toolchainLines, 3)
        << "expected multiple vcpkg toolchain-file lines in CMakePresets.json";
}

// No hardcoded per-developer absolute vcpkg path may survive anywhere in the
// presets file (belt-and-suspenders against a path that isn't on a
// "vcpkg.cmake" line, e.g. a stray VCPKG_ROOT default).
TEST(CiSupplyChain, PresetsHaveNoHardcodedDeveloperVcpkgPath) {
    std::string presets = readFileContents(getProjectRoot() / "CMakePresets.json");
    ASSERT_FALSE(presets.empty());

    EXPECT_FALSE(contains(presets, "C:/Users/dev/source/vcpkg"))
        << "hardcoded per-developer vcpkg path must be removed from CMakePresets.json";
    // Generalized: any drive-letter path that ends in a vcpkg toolchain file.
    EXPECT_FALSE(contains(presets, "/vcpkg/scripts/buildsystems/vcpkg.cmake") &&
                 (contains(presets, ":/Users/") || contains(presets, ":/users/")))
        << "a Windows drive-letter absolute vcpkg toolchain path remains (non-portable)";
}

// ---------------------------------------------------------------------------
// R2-24 : Linux validation leg runs the gtest binary directly, non-vacuously
// ---------------------------------------------------------------------------

// The Linux leg must invoke the built pdfe_tests binary DIRECTLY and emit a
// JUnit XML -- not rely on `ctest`, which exits 0 even when zero tests are
// discovered.
TEST(CiSupplyChain, LinuxValidationInvokesGtestBinaryDirectly) {
    std::string wf = readFileContents(
        getProjectRoot() / ".github" / "workflows" / "linux-validation.yml");
    ASSERT_FALSE(wf.empty()) << "linux-validation.yml must be readable";

    EXPECT_TRUE(contains(wf, "pdfe_tests"))
        << "Linux leg must reference the pdfe_tests binary";
    EXPECT_TRUE(contains(wf, "--gtest_output=xml"))
        << "Linux leg must run the gtest binary directly with --gtest_output=xml "
           "(so results can be parsed), not the vacuous ctest discovery path";
    EXPECT_TRUE(contains(wf, "gtest-results.xml"))
        << "Linux leg must produce gtest-results.xml for the non-vacuous parse";
}

// The results must be parsed and a vacuous exit-0 (zero/partial tests) must
// FAIL the job: assert the XML gate checks tests>0 and failures/errors == 0.
TEST(CiSupplyChain, LinuxValidationFailsOnVacuousOrFailingRun) {
    std::string wf = stripHashComments(readFileContents(
        getProjectRoot() / ".github" / "workflows" / "linux-validation.yml"));
    ASSERT_FALSE(wf.empty());

    // Zero-tests guard (the crux of R2-24: a green run with no tests must fail).
    // The "VACUOUS" refuse-green must be reached via an ACTIVE tests-count
    // condition — assert the nearest preceding `if` tests the count. A disabled
    // guard (e.g. `if False:`) that keeps the message must still re-red, and an
    // unrelated `if tests == 0:` (count-summing fallback) elsewhere must not
    // accidentally satisfy it.
    size_t vpos = wf.find("VACUOUS");
    ASSERT_NE(vpos, std::string::npos)
        << "Linux leg must refuse a zero-test green (VACUOUS marker)";
    size_t guard = wf.rfind("if ", vpos);
    ASSERT_NE(guard, std::string::npos)
        << "the VACUOUS refuse-green must be inside an if-guard";
    std::string guardLine = wf.substr(guard, wf.find('\n', guard) - guard);
    EXPECT_TRUE(contains(guardLine, "tests"))
        << "the VACUOUS refuse-green must be guarded by an ACTIVE tests-count "
           "condition (found: '" << guardLine << "') — a disabled `if False:` fails this";
    // Real failures/errors must also fail the job.
    EXPECT_TRUE(contains(wf, "failures"))
        << "Linux leg must assert on the XML 'failures' count";
    EXPECT_TRUE(contains(wf, "errors"))
        << "Linux leg must assert on the XML 'errors' count";
}

// ---------------------------------------------------------------------------
// R2-27 : install_tools.ps1 SHA256-verifies every fetched scanner binary
// ---------------------------------------------------------------------------

namespace {

// Return the body text of a PowerShell `function <name> { ... }` block: from the
// function header up to (but not including) the next top-level "function "
// keyword, or end of file. Coarse but sufficient for ordering checks.
std::string functionBody(const std::string& src, const std::string& fnName) {
    std::string header = "function " + fnName;
    size_t start = src.find(header);
    if (start == std::string::npos) return "";
    size_t next = src.find("\nfunction ", start + header.size());
    if (next == std::string::npos) next = src.size();
    return src.substr(start, next - start);
}

} // namespace

// The pinned-hash map and the verification helper must exist, and the helper
// must actually use Get-FileHash -Algorithm SHA256.
TEST(CiSupplyChain, InstallToolsPinsHashesAndDefinesVerifier) {
    std::string ps = readFileContents(
        getProjectRoot() / "scripts" / "security" / "install_tools.ps1");
    ASSERT_FALSE(ps.empty()) << "install_tools.ps1 must be readable";

    EXPECT_TRUE(contains(ps, "$HASHES"))
        << "install_tools.ps1 must define a pinned SHA256 map ($HASHES)";
    EXPECT_TRUE(contains(ps, "_verify_sha256"))
        << "install_tools.ps1 must define a SHA256 verification helper";
    EXPECT_TRUE(contains(ps, "Get-FileHash") && contains(ps, "SHA256"))
        << "verification must use Get-FileHash -Algorithm SHA256";

    // Each fetched scanner artifact must have a pinned digest keyed by its
    // exact downloaded filename.
    EXPECT_TRUE(contains(ps, "osv-scanner_windows_amd64.exe"))
        << "osv-scanner artifact must be pinned in $HASHES";
    EXPECT_TRUE(contains(ps, "gitleaks_8.21.2_windows_x64.zip"))
        << "gitleaks artifact must be pinned in $HASHES";
    EXPECT_TRUE(contains(ps, "trufflehog_3.83.7_windows_amd64.tar.gz"))
        << "trufflehog artifact must be pinned in $HASHES";
}

// Structural oracle: within EACH scanner installer function, the _download call
// must be followed by a _verify_sha256 call. This re-fires (RED) if any single
// scanner's verification is dropped or moved before the download.
TEST(CiSupplyChain, EveryScannerDownloadIsFollowedByShaVerification) {
    std::string ps = readFileContents(
        getProjectRoot() / "scripts" / "security" / "install_tools.ps1");
    ASSERT_FALSE(ps.empty());

    const char* fns[] = { "Install-OsvScanner", "Install-Gitleaks", "Install-Trufflehog" };
    for (const char* fn : fns) {
        std::string body = stripHashComments(functionBody(ps, fn));
        ASSERT_FALSE(body.empty()) << "missing installer function: " << fn;
        size_t dl  = body.find("_download");
        size_t ver = body.find("_verify_sha256");
        EXPECT_NE(dl, std::string::npos)  << fn << " must download its binary";
        EXPECT_NE(ver, std::string::npos)
            << fn << " must SHA256-verify the download (R2-27)";
        if (dl != std::string::npos && ver != std::string::npos) {
            EXPECT_LT(dl, ver)
                << fn << ": _verify_sha256 must come AFTER _download, not before";
        }
    }
}

// ---------------------------------------------------------------------------
// SEC-80 (meta-audit follow-up F9) -- tests/security/deps/triage.md carried the
// heading "## MuPDF + QPDF advisory check (AC7)" TWICE, with contradictory
// bodies: the first said the advisory check was verified by a live osv-scanner
// run on 2026-04-25; the second said it was "unverified ... treat as the next
// required hands-on check". A reader who scrolled to the second one concluded
// the dependency audit had never been run.
//
// Duplicate headings in a triage document are therefore a supply-chain defect,
// not a formatting nit: the document's whole job is to record ONE disposition
// per item. This lint fails on any repeated heading.
// ---------------------------------------------------------------------------
TEST(CiSupplyChain, DepsTriageHeadingsAreUnique) {
    const std::filesystem::path triage =
        getProjectRoot() / "tests" / "security" / "deps" / "triage.md";
    const std::string body = readFileContents(triage);
    ASSERT_FALSE(body.empty()) << "tests/security/deps/triage.md must be readable";

    std::vector<std::string> headings;
    for (const std::string& raw : splitLines(body)) {
        // Trim leading whitespace; blockquoted lines ("> ## x") are commentary
        // about a removed section, not a heading of their own.
        size_t i = 0;
        while (i < raw.size() && (raw[i] == ' ' || raw[i] == '\t')) ++i;
        if (i >= raw.size() || raw[i] != '#') continue;
        std::string h = raw.substr(i);
        while (!h.empty() && (h.back() == ' ' || h.back() == '\t')) h.pop_back();
        if (h.size() < 2) continue;
        headings.push_back(h);
    }
    ASSERT_FALSE(headings.empty()) << "no headings parsed from triage.md -- the "
                                      "lint would pass vacuously";

    std::map<std::string, int> seen;
    for (const std::string& h : headings) seen[h]++;

    for (const auto& [heading, count] : seen) {
        EXPECT_EQ(count, 1)
            << "tests/security/deps/triage.md repeats the heading \"" << heading
            << "\" " << count << " times. Duplicated triage sections have already "
               "shipped contradictory dispositions once (SEC-80): one copy said "
               "the MuPDF/QPDF advisory check was verified, the other said it was "
               "not. Keep one section per item.";
    }
}
