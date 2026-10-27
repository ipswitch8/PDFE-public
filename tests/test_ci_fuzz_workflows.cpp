// test_ci_fuzz_workflows.cpp
// Static-configuration tests for the CI fuzz + validation workflows
// (R2-13 + R2-14). These parse the GitHub Actions YAML from the source
// tree and assert the acceptance criterion:
//
//   "fuzz + validation workflows get a real trigger (nightly schedule at
//    minimum); the chronically-red Windows fuzz job is stabilised or moved
//    to a supported runner; corpora persist + minimise across runs so
//    coverage compounds"
//
// No build/runtime dependency — the tests read the workflow files located
// relative to the project root (via TEST_FIXTURES_DIR, same pattern as
// test_installer.cpp) and assert real substrings / structure.

#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

// tests/fixtures -> tests -> <project root>  (same as test_installer.cpp)
std::filesystem::path getProjectRoot() {
    return std::filesystem::path(TEST_FIXTURES_DIR).parent_path().parent_path();
}

std::filesystem::path workflowPath(const std::string& name) {
    return getProjectRoot() / ".github" / "workflows" / name;
}

std::string readFileContents(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return "";
    }
    return std::string(std::istreambuf_iterator<char>(file),
                       std::istreambuf_iterator<char>());
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// Drop whole-line YAML comments (first non-space char is '#') so a commented-out
// directive (or a backtick-quoted mention in a rationale comment) cannot satisfy
// an ACTIVE-directive assertion. Mutation-survivor hardening.
std::string stripYamlComments(const std::string& src) {
    std::string out, line;
    for (char c : src) {
        if (c == '\n') {
            std::size_t p = line.find_first_not_of(" \t");
            if (!(p != std::string::npos && line[p] == '#')) { out += line; out += '\n'; }
            line.clear();
        } else if (c != '\r') {
            line.push_back(c);
        }
    }
    std::size_t p = line.find_first_not_of(" \t");
    if (!(p != std::string::npos && line[p] == '#')) out += line;
    return out;
}

// Extract the top-level `on:` trigger block. It starts at a line beginning
// with "on:" (column 0) and runs until the next column-0, non-comment,
// non-blank key (e.g. "jobs:", "concurrency:", "env:"). Good enough to tell
// a schedule/dispatch trigger inside the trigger block from an unrelated
// "schedule" word elsewhere in the file.
std::string extractOnBlock(const std::string& yaml) {
    std::size_t pos = 0;
    // Find a line that is exactly "on:" at column 0.
    while (pos < yaml.size()) {
        std::size_t eol = yaml.find('\n', pos);
        std::string line = yaml.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        if (line.rfind("on:", 0) == 0) {
            std::size_t start = pos;
            // Advance to the end of the block.
            std::size_t cur = (eol == std::string::npos) ? yaml.size() : eol + 1;
            while (cur < yaml.size()) {
                std::size_t nextEol = yaml.find('\n', cur);
                std::string l = yaml.substr(cur, nextEol == std::string::npos ? std::string::npos : nextEol - cur);
                bool topLevelKey = !l.empty() && l[0] != ' ' && l[0] != '\t' &&
                                   l[0] != '#' && l.find(':') != std::string::npos;
                if (topLevelKey) {
                    return yaml.substr(start, cur - start);
                }
                cur = (nextEol == std::string::npos) ? yaml.size() : nextEol + 1;
            }
            return yaml.substr(start);
        }
        pos = (eol == std::string::npos) ? yaml.size() : eol + 1;
    }
    return "";
}

/// `yaml` with every full-line and trailing `#` comment removed. Only used for
/// presence checks, so it does not need to understand `#` inside a quoted
/// scalar -- no workflow key in this repository contains one, and a false
/// POSITIVE here (a stripped `#` that was really data) would make the sweep
/// stricter, never laxer.
std::string stripComments(const std::string& yaml) {
    std::string out;
    std::size_t pos = 0;
    while (pos <= yaml.size()) {
        const std::size_t eol = yaml.find('\n', pos);
        std::string line = yaml.substr(pos, eol == std::string::npos
                                                ? std::string::npos
                                                : eol - pos);
        const std::size_t hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        out += line;
        out += '\n';
        if (eol == std::string::npos) break;
        pos = eol + 1;
    }
    return out;
}

} // namespace

//
// (a) THE FUZZ + VALIDATION WORKFLOWS ARE MANUAL-ONLY.
//
// INVERTED 2026-08-13. These two cases used to assert that each workflow
// DECLARES a nightly `schedule:` trigger. Every scheduled trigger in this
// repository was removed that day on the owner's instruction, so the assertion
// they made is now false BY DESIGN and they went red the moment the policy
// changed — which is the guard doing its job, not a defect.
//
// They are inverted rather than deleted, because the property worth pinning did
// not disappear with the cron. It changed direction: these lanes must remain
// DISPATCHABLE and must NOT quietly reacquire a timer. A workflow that grows a
// `schedule:` back without the posture document being updated is exactly the
// drift that docs/audit/ci-manual-only-posture.md exists to prevent, and three
// Python gate checkers read that document to decide whether an absent cadence
// is a decision or an accident. This is the same rule stated in C++, so the
// gtest suite alone would catch it.
//

TEST(CiFuzzWorkflows, WindowsFuzzIsManualOnlyAndStaysDispatchable) {
    std::string yaml = readFileContents(workflowPath("windows-fuzz.yml"));
    ASSERT_FALSE(yaml.empty()) << "windows-fuzz.yml should be readable";

    std::string on = extractOnBlock(yaml);
    ASSERT_FALSE(on.empty()) << "windows-fuzz.yml should have a top-level on: block";

    EXPECT_FALSE(contains(on, "schedule:"))
        << "windows-fuzz.yml has reacquired a schedule: trigger. This "
           "repository is MANUAL ONLY (docs/audit/ci-manual-only-posture.md): "
           "the self-hosted runner is not a service, so a scheduled run with no "
           "runner online queues and is then reported `cancelled`, which alerts "
           "nobody. If a schedule is genuinely wanted, update the posture "
           "document FIRST — three gate checkers read it.";
    EXPECT_TRUE(contains(on, "workflow_dispatch:"))
        << "windows-fuzz.yml must keep workflow_dispatch: with no schedule, a "
           "lane that cannot be dispatched cannot run at all";
}

TEST(CiFuzzWorkflows, WindowsValidationIsManualOnlyAndStaysDispatchable) {
    std::string yaml = readFileContents(workflowPath("windows-validation.yml"));
    ASSERT_FALSE(yaml.empty()) << "windows-validation.yml should be readable";

    std::string on = extractOnBlock(yaml);
    ASSERT_FALSE(on.empty()) << "windows-validation.yml should have a top-level on: block";

    EXPECT_FALSE(contains(on, "schedule:"))
        << "windows-validation.yml has reacquired a schedule: trigger — see "
           "docs/audit/ci-manual-only-posture.md. This lane in particular held "
           "the MACHINE-GLOBAL single-instance mutex for hours through its UI "
           "E2E step, redding ShellAndInstanceLaunchTest in every concurrent "
           "local run on the same host.";
    EXPECT_TRUE(contains(on, "workflow_dispatch:"))
        << "windows-validation.yml must keep workflow_dispatch for manual runs";
}

TEST(CiFuzzWorkflows, NoWorkflowInThisRepositoryCarriesASchedule) {
    // The class-level form of the two cases above: naming two files by hand is
    // exactly the corpus mistake DCA 0023 records, so the posture is asserted
    // over every workflow rather than the two that happened to be edited.
    const std::filesystem::path dir = workflowPath("windows-fuzz.yml").parent_path();
    int checked = 0;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (it->path().extension() != ".yml") continue;
        const std::string yaml = readFileContents(it->path());
        if (yaml.empty()) continue;
        const std::string on = extractOnBlock(yaml);
        if (on.empty()) continue;
        ++checked;
        // COMMENT LINES ARE STRIPPED FIRST, and leaving that out produced two
        // false failures on the first run: hetzner-sanitizer.yml and
        // linux-validation.yml both EXPLAIN, in a comment inside their own `on:`
        // block, why they deliberately have no schedule. A sweep that reads its
        // own documentation as configuration reports the opposite of the truth
        // -- the same species as the WIN-87 grep that returned five hits, every
        // one a comment describing the defect that had just been removed.
        EXPECT_FALSE(contains(stripComments(on), "schedule:"))
            << it->path().filename().string() << " carries a schedule: trigger; "
            << "this repository is MANUAL ONLY "
            << "(docs/audit/ci-manual-only-posture.md)";
    }
    // VACUITY GUARD: a walk that found nothing would satisfy every expectation
    // above, and "there are no workflows" is false about this repository.
    EXPECT_GE(checked, 8)
        << "only " << checked << " workflow(s) were examined; the sweep broke "
           "rather than the posture holding";
}

//
// (b) The chronically-red Windows fuzz job has its stabilisation measure:
//     continue-on-error at the job level WITH a documented rationale that
//     references the known CI limitation (so coverage is not deleted).
//

TEST(CiFuzzWorkflows, WindowsFuzzJobIsStabilised) {
    // Strip comments so a backtick-quoted mention of the directive in a rationale
    // comment cannot satisfy the assertion — only the ACTIVE job directive counts.
    std::string yaml = stripYamlComments(readFileContents(workflowPath("windows-fuzz.yml")));
    ASSERT_FALSE(yaml.empty());

    // Gated as continue-on-error so a residual mis-triaged benign C++
    // deadly-signal does not red the nightly and lose the accumulated corpus.
    EXPECT_TRUE(contains(yaml, "continue-on-error: true"))
        << "windows-fuzz job must be gated continue-on-error: true "
           "(stabilisation for the chronically-red campaign)";

    // The rationale must be documented, not silent — reference the known
    // limitation so a reader understands WHY it is non-blocking.
    EXPECT_TRUE(contains(yaml, "CI-LIMITATION-WINDOWS-FUZZ-CXX-EXCEPTION-DEADLY-SIGNAL"))
        << "windows-fuzz.yml must document the stabilisation rationale "
           "(reference the CI limitation)";
}

//
// (c) Corpora persist + minimise across runs so coverage compounds:
//     an actions/cache keyed to accumulate, plus a libFuzzer -merge=1
//     minimisation step.
//

TEST(CiFuzzWorkflows, WindowsFuzzPersistsCorpusAcrossRuns) {
    std::string yaml = readFileContents(workflowPath("windows-fuzz.yml"));
    ASSERT_FALSE(yaml.empty());

    EXPECT_TRUE(contains(yaml, "actions/cache@"))
        << "windows-fuzz.yml must cache the corpus so it persists across "
           "scheduled runs";
    // Accumulating cache pattern: a per-run key plus restore-keys prefix so
    // each run restores the newest prior corpus and saves a superset.
    EXPECT_TRUE(contains(yaml, "restore-keys:"))
        << "corpus cache must use restore-keys so coverage accumulates run "
           "over run";
    EXPECT_TRUE(contains(yaml, "fuzz-corpus-"))
        << "corpus cache key should be namespaced (fuzz-corpus-...)";
}

TEST(CiFuzzWorkflows, WindowsFuzzMinimisesCorpus) {
    std::string yaml = readFileContents(workflowPath("windows-fuzz.yml"));
    ASSERT_FALSE(yaml.empty());

    // libFuzzer corpus minimisation merges redundant inputs so the cached
    // corpus stays coverage-dense instead of growing unboundedly.
    EXPECT_TRUE(contains(yaml, "-merge=1"))
        << "windows-fuzz.yml must run libFuzzer corpus minimisation (-merge=1)";
}

// R2-15/16: the four write/mutation-surface harnesses (the areas holding PDFE's
// real historical bugs) must ACTUALLY RUN in the nightly campaign and be
// selectable by the manual driver — not merely build + one-shot smoke. A harness
// built but absent from the campaign array gives zero ongoing fuzzing coverage
// (the security-audit gate caught exactly this gap). Comments are stripped so a
// commented-out array entry cannot satisfy the check.
TEST(CiFuzzWorkflows, WindowsFuzzCampaignRunsWriteSurfaceHarnesses) {
    const std::string yaml =
        stripYamlComments(readFileContents(workflowPath("windows-fuzz.yml")));
    ASSERT_FALSE(yaml.empty());
    const std::string driver = stripYamlComments(readFileContents(
        getProjectRoot() / "scripts" / "security" / "run_fuzz.ps1"));
    ASSERT_FALSE(driver.empty());

    // Meta-audit F13 (SEC-58) extends this list beyond the four write-surface
    // harnesses to the two DECODE/PARSE harnesses that cover the surfaces
    // where PDFE's only RCE-class defect lived (image codecs, #50/DCA-0011)
    // and the surface that had no fuzz coverage at all (font programs).
    // Built-but-not-campaigned is zero coverage for those exactly as it was
    // for the write surfaces.
    const char* writeSurfaces[] = {
        "fuzz_content_stream_edit", "fuzz_save_roundtrip",
        "fuzz_mujs_exec", "fuzz_redaction_apply",
        "fuzz_image_decode", "fuzz_font_program"
    };
    for (const char* h : writeSurfaces) {
        EXPECT_TRUE(contains(yaml, h))
            << h << " must be in the windows-fuzz.yml nightly campaign array "
                 "(built-but-not-run = zero fuzzing coverage)";
        EXPECT_TRUE(contains(driver, h))
            << h << " must be selectable in run_fuzz.ps1 $AllHarnesses";
    }
}
