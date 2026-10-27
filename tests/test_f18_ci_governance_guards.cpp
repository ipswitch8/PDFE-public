// tests/test_f18_ci_governance_guards.cpp
//
// F18 (meta-audit follow-up phase-18) — standing guards over the CI governance
// facts this phase re-verified.
//
// A3-36 is one of the seventeen A3 STALE-OPEN rows: rows where the project
// claimed a closure the tree contradicted. Re-verifying it in the tree showed the
// row is stale in the OTHER direction — the register's complaint ("the lane still
// runs only PreflightOpen.*:PreflightXPlatform.*, so adding a cadence does not
// widen the subset") was true when it was written but is no longer true: commit
// 35a359e (coverage-expansion phase-5, C-10) added a CROSS-PLATFORM CORE PARITY
// step to the macOS lane that runs the shared selection from
// tools/gate/parity_suite.txt via tools/gate/parity_filter.py, with a
// --min-tests floor on the resulting JUnit XML.
//
// Nothing asserted that, so it could silently regress back to the thin subset and
// the support claim would go unverified again exactly as before. This file is the
// assertion. It reads the workflow and the shared selection out of the tree.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path repoRoot() { return fs::path(PDFE_REPO_ROOT); }

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// A3-36 — "run the full (or a representative core) suite on the macOS lane, or
// drop the platform's advertised support claim". The lane must run the shared
// parity selection, not just the two macOS-only preflight suites.
// ═══════════════════════════════════════════════════════════════════════════
TEST(F18CiGovernance, MacOsLaneRunsTheSharedCoreParitySelection) {
    const fs::path wf = repoRoot() / ".github" / "workflows" / "macos-validation.yml";
    ASSERT_TRUE(fs::exists(wf)) << "macOS validation workflow is missing: " << wf;
    const std::string yml = readFile(wf);

    EXPECT_TRUE(contains(yml, "tools/gate/parity_filter.py"))
        << "the macOS lane must build its gtest filter from the SHARED parity "
           "selection, not from a per-lane literal that can drift";
    EXPECT_TRUE(contains(yml, "macos-parity-results.xml"))
        << "the parity run must emit its own JUnit XML";
    EXPECT_TRUE(contains(yml, "check_junit_xml.py"))
        << "GAP-A15: the lane must PARSE the JUnit XML rather than trust the "
           "process exit code";
    EXPECT_TRUE(contains(yml, "--min-tests"))
        << "without a minimum-test floor the parity selection could silently "
           "shrink to a handful of green tests and still pass";

    // INVERTED 2026-08-13. This asserted the macOS lane runs on a RECURRING
    // trigger, which was the half R-36 closed. Every `schedule:` in this
    // repository was removed that day on the owner's instruction
    // (docs/audit/ci-manual-only-posture.md), so the lane is dispatch-only by
    // design and this went red the moment the policy changed -- the guard
    // working, not a defect.
    //
    // The property is inverted rather than dropped: the lane must stay
    // DISPATCHABLE and must not silently reacquire a timer. Note this lane is
    // also one of the two whose red is EXPECTED for a different reason entirely
    // -- macOS and Linux are not built yet -- so nothing here should be read as
    // a claim that the lane passes.
    EXPECT_FALSE(contains(yml, "schedule:"))
        << "the macOS lane has reacquired a schedule: trigger; this repository "
           "is MANUAL ONLY (docs/audit/ci-manual-only-posture.md)";
    EXPECT_TRUE(contains(yml, "workflow_dispatch:"))
        << "with no cadence, a lane that cannot be dispatched cannot run at all";
}

// The selection the lane depends on must exist and be substantial. A parity file
// trimmed to nothing would satisfy the checks above while asserting nothing.
TEST(F18CiGovernance, ParitySelectionIsSubstantialAndShared) {
    const fs::path sel = repoRoot() / "tools" / "gate" / "parity_suite.txt";
    ASSERT_TRUE(fs::exists(sel)) << "shared parity selection missing: " << sel;

    std::ifstream in(sel);
    std::string line;
    int patterns = 0;
    while (std::getline(in, line)) {
        // Strip whitespace; skip blanks and comments.
        const size_t b = line.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) continue;
        const size_t e = line.find_last_not_of(" \t\r\n");
        const std::string t = line.substr(b, e - b + 1);
        if (t.empty() || t[0] == '#') continue;
        ++patterns;
    }
    EXPECT_GE(patterns, 50)
        << "the cross-platform parity selection has shrunk to " << patterns
        << " suite patterns; it is the only thing making the macOS/Linux "
           "support claims mean anything";

    // All three lanes must consume the SAME generator, or the selection drifts
    // per platform and "parity" stops being parity.
    for (const char* lane : {"macos-validation.yml", "linux-validation.yml",
                             "windows-validation.yml"}) {
        const fs::path wf = repoRoot() / ".github" / "workflows" / lane;
        ASSERT_TRUE(fs::exists(wf)) << "missing workflow: " << lane;
        EXPECT_TRUE(contains(readFile(wf), "parity_filter.py"))
            << lane << " does not use the shared parity filter, so the three "
                       "lanes are not running the same selection";
    }
}
