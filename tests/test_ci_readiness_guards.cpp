// -*- coding: utf-8 -*-
//
// F49 (deferred-backlog, follow-up phase-66) — the hole, not the defect.
//
// F49's DEFECT was that nothing read windows-validation's true CI conclusion.
// That was fixed for `fast-checks`. The HOLE stayed open: no general reader of
// this workflow's actual conclusion, and — worse — the run-level conclusion is
// the wrong thing to read. Run 31359621883 is the proof, and it is committed
// here as a fixture rather than described:
//
//     run-level conclusion: cancelled
//       step 13  failure   Run + verify CROSS-PLATFORM CORE PARITY suite (C-10)
//       step 15  failure   Code coverage (OpenCppCoverage -> Cobertura)
//       step 18  failure   Run ctest -L security (meta-audit F5 / SEC-23)
//
// Three steps red on their own merits, invisible behind one `cancelled` — a
// status that "alerts nobody and is indistinguishable from a deliberate
// cancellation" (audit-of-audits §4.3).
//
// WHY A RECORDED FIXTURE. The dry-run required by this phase's criterion 3 has
// to be reproducible from a working tree with no network and no credentials. A
// test that called the GitHub API would be untestable offline, would rot when
// the run ages out, and would turn a rate limit into a red suite. The recorded
// payload is the actual API response, saved 2026-08-15.
//
// FOUR CASES, because these fail independently:
//   * the checker's own contract (its self-test)
//   * the checker against the REAL masked run — the criterion's dry-run
//   * the fixture still being a masking example at all (if its run-level
//     conclusion ever stopped being `cancelled`, the dry-run would prove
//     nothing and would pass while proving nothing)
//   * the workflow actually invoking it, on the right triggers
//
// And one case for what must NOT have happened: nothing may register the F46
// runner autostart. That is the owner's explicit direction, and the difference
// between a deliberate omission and an oversight is exactly what a guard is
// for — an omission nobody wrote down gets "fixed" by the next person.

#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path repoRoot() { return fs::path(PDFE_REPO_ROOT); }

std::string readFile(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int runCapturing(const std::string& cmd, std::string& output)
{
    output.clear();
#if defined(_WIN32)
    FILE* pipe = ::_popen(cmd.c_str(), "r");
#else
    FILE* pipe = ::popen(cmd.c_str(), "r");
#endif
    if (!pipe) return -1;
    std::array<char, 4096> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe)) {
        output += buf.data();
    }
#if defined(_WIN32)
    return ::_pclose(pipe);
#else
    return ::pclose(pipe);
#endif
}

std::string quoted(const std::string& s) { return "\"" + s + "\""; }

fs::path checker() { return repoRoot() / "tools" / "gate" / "check_ci_readiness.py"; }
fs::path jobsFixture()
{
    return repoRoot() / "tools" / "gate" / "fixtures" /
           "run-31359621883-jobs.json";
}
fs::path workflow()
{
    return repoRoot() / ".github" / "workflows" / "windows-validation.yml";
}

// True when `needle` appears on a line that is not a YAML comment. The F48
// workflow guard was fooled once by its own explanatory comment; the same
// mistake is not worth making twice in the same programme.
bool inCodeLine(const std::string& haystack, const std::string& needle)
{
    std::istringstream in(haystack);
    std::string line;
    while (std::getline(in, line)) {
        const size_t first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos || line[first] == '#') continue;
        if (line.find(needle) != std::string::npos) return true;
    }
    return false;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
TEST(CiReadinessGuards, TheCheckerSatisfiesItsOwnContract)
{
    const std::string python = PDFE_PYTHON_EXECUTABLE;
    ASSERT_FALSE(python.empty())
        << "no Python 3 interpreter at configure time, so the CI readiness "
           "checker cannot be verified. A hard failure, not a skip.";
    ASSERT_TRUE(fs::exists(checker())) << checker().string() << " is missing";

    const std::string cmd = "set PYTHONIOENCODING=utf-8 && " + quoted(python) +
                            " " + quoted(checker().string()) +
                            " self-test 2>&1";
    std::string out;
    const int rc = runCapturing(cmd, out);
    EXPECT_EQ(rc, 0) << "check_ci_readiness.py self-test failed:\n\n" << out;

    const auto pos = out.find("self-test: ");
    ASSERT_NE(pos, std::string::npos)
        << "no check count printed; the self-test may have exited early.\n\n"
        << out;
    EXPECT_GE(std::atoi(out.c_str() + static_cast<long>(pos) + 11), 12)
        << "the self-test shrank. It must still cover runner online/offline/"
           "mislabelled/absent, tool present/missing, a red step inside a "
           "CANCELLED run, cancelled-and-skipped steps not counting as red, "
           "timed_out counting as red, and the fail-closed shapes.\n\n"
        << out;
}

// The criterion's dry-run: would this have surfaced the three masked steps?
TEST(CiReadinessGuards, TheCheckerSurfacesTheThreeMaskedStepsOfRun31359621883)
{
    const std::string python = PDFE_PYTHON_EXECUTABLE;
    ASSERT_FALSE(python.empty()) << "no Python 3 interpreter at configure time";
    ASSERT_TRUE(fs::exists(jobsFixture()))
        << "the recorded job data for run 31359621883 is missing: "
        << jobsFixture().string()
        << ". Without it this phase's dry-run criterion cannot be demonstrated.";

    const std::string cmd = "set PYTHONIOENCODING=utf-8 && " + quoted(python) +
                            " " + quoted(checker().string()) + " steps --jobs " +
                            quoted(jobsFixture().string()) + " --enforce 2>&1";
    std::string out;
    const int rc = runCapturing(cmd, out);

    EXPECT_NE(rc, 0)
        << "the checker exited 0 on a run containing three failed steps. It "
           "must report red under --enforce, or it would have cleared run "
           "31359621883 exactly as the run-level conclusion did.\n\n"
        << out;

    // Named individually. A count alone would pass if it found three different
    // steps, and the point of the record is which three.
    for (const char* needle : {"CROSS-PLATFORM CORE PARITY",
                               "OpenCppCoverage",
                               "ctest -L security"}) {
        EXPECT_NE(out.find(needle), std::string::npos)
            << "the report does not name the masked step containing '"
            << needle << "'.\n\n" << out;
    }
    EXPECT_NE(out.find("3 step(s) went RED"), std::string::npos)
        << "expected exactly three red steps in this run.\n\n" << out;

    // The UI step was CANCELLED, not failed. Counting it red would make every
    // interrupted run fail for the wrong reason and bury the real three.
    EXPECT_EQ(out.find("Run UI E2E suite"), std::string::npos)
        << "a cancelled step was reported as red. Cancellation is what the "
           "masking looks like from the inside; the steps that failed on their "
           "own merits are the signal.\n\n" << out;
}

// The fixture must still BE a masking example. If its run-level conclusion
// stopped being `cancelled`, the case above would still pass while
// demonstrating nothing about masking.
TEST(CiReadinessGuards, TheFixtureIsStillAnExampleOfRunLevelMasking)
{
    const std::string jobs = readFile(jobsFixture());
    ASSERT_FALSE(jobs.empty()) << jobsFixture().string() << " is missing/empty";

    EXPECT_NE(jobs.find("\"conclusion\":\"cancelled\""), std::string::npos)
        << "the recorded job no longer reports `cancelled` at the job level, "
           "so it no longer demonstrates a red step hidden behind a non-red "
           "status — which is the whole reason this fixture is committed.";

    const std::string run = readFile(repoRoot() / "tools" / "gate" /
                                     "fixtures" / "run-31359621883-run.json");
    ASSERT_FALSE(run.empty()) << "the run-level payload fixture is missing";
    EXPECT_NE(run.find("\"conclusion\":\"cancelled\""), std::string::npos)
        << "the recorded RUN-level conclusion is no longer `cancelled`.";
    EXPECT_NE(run.find("\"status\":\"completed\""), std::string::npos)
        << "the recorded run is no longer `completed`; a still-running run "
           "would be a different situation entirely.";
}

// Criteria 1 and 2: the workflow must actually invoke it, on the right triggers.
TEST(CiReadinessGuards, TheWorkflowInvokesBothHalvesOnTheRightTriggers)
{
    const std::string wf = readFile(workflow());
    ASSERT_FALSE(wf.empty()) << workflow().string() << " is missing";

    ASSERT_TRUE(inCodeLine(wf, "check_ci_readiness.py"))
        << "windows-validation.yml never invokes the readiness checker outside "
           "a comment. F49's hole is only closed if something actually runs it.";

    // Split into steps and find the two invocations, so their `if:` conditions
    // can be checked against the step they belong to rather than the file.
    const std::string marker = "\n      - name:";
    std::vector<std::string> steps;
    size_t at = wf.find(marker);
    while (at != std::string::npos) {
        const size_t next = wf.find(marker, at + 1);
        steps.push_back(wf.substr(
            at, (next == std::string::npos ? wf.size() : next) - at));
        at = next;
    }
    ASSERT_FALSE(steps.empty())
        << "no steps parsed from the workflow; this guard would pass vacuously.";

    bool readinessOnDispatch = false;
    bool stepsAlways = false;
    int invocations = 0;
    for (const auto& step : steps) {
        if (!inCodeLine(step, "check_ci_readiness.py")) continue;
        ++invocations;
        if (inCodeLine(step, " readiness ") || inCodeLine(step, "readiness `")) {
            // Criterion 1: specifically on manual submit.
            if (inCodeLine(step, "workflow_dispatch")) readinessOnDispatch = true;
        }
        if (inCodeLine(step, " steps ") || inCodeLine(step, "steps `")) {
            // Criterion 2: must run even when earlier steps failed, or it can
            // never report on the run it is meant to unmask.
            if (inCodeLine(step, "always()")) stepsAlways = true;
        }
    }

    EXPECT_GE(invocations, 2)
        << "expected two invocations (readiness preflight, step-conclusion "
           "postflight); found " << invocations << ".";
    EXPECT_TRUE(readinessOnDispatch)
        << "the readiness step must be conditioned on "
           "github.event_name == 'workflow_dispatch' — criterion 1 asks for it "
           "specifically on manual submit.";
    EXPECT_TRUE(stepsAlways)
        << "the step-conclusion step must be `if: always()`. Without it, the "
           "one situation it exists for — a run that went wrong — is the one "
           "situation it would not run in.";
}

// Criterion 4: the F46 autostart must remain uninstalled, and deliberately so.
TEST(CiReadinessGuards, NothingRegistersTheF46RunnerAutostart)
{
    const fs::path wfDir = repoRoot() / ".github" / "workflows";
    ASSERT_TRUE(fs::is_directory(wfDir)) << wfDir.string() << " is missing";

    std::vector<std::string> offenders;
    int scanned = 0;
    for (const auto& e : fs::directory_iterator(wfDir)) {
        if (!e.is_regular_file()) continue;
        const auto ext = e.path().extension().string();
        if (ext != ".yml" && ext != ".yaml") continue;
        ++scanned;
        const std::string body = readFile(e.path());
        for (const char* needle : {"register_runner_service.ps1",
                                   "runner_watchdog.ps1"}) {
            if (inCodeLine(body, needle)) {
                offenders.push_back(e.path().filename().string() +
                                    " invokes " + needle);
            }
        }
    }

    EXPECT_GT(scanned, 0)
        << "no workflow files scanned; this guard would pass vacuously.";
    EXPECT_TRUE(offenders.empty())
        << "a workflow now registers or runs the F46 runner autostart. Those "
           "scripts stay UNREGISTERED by the owner's explicit direction "
           "(governance.yml, header note \"THE ACTUAL DETECTION MECHANISM\", "
           "crash/logout bullet) — the readiness check above is the "
           "alternative that was asked for instead. If this is a deliberate "
           "reversal, the direction has to change first."
        << [&] {
               std::string s;
               for (const auto& o : offenders) s += "\n    " + o;
               return s;
           }();

    // The scripts themselves must still EXIST — the decision is "not
    // registered", not "deleted". Deleting them would silently convert a
    // reversible policy choice into a lost capability.
    for (const char* name : {"register_runner_service.ps1",
                             "runner_watchdog.ps1"}) {
        EXPECT_TRUE(fs::exists(repoRoot() / "tools" / "ops" / name))
            << "tools/ops/" << name << " has been deleted. The direction is "
               "that it stays UNREGISTERED, not that it stops existing.";
    }
}
