// -*- coding: utf-8 -*-
//
// Standing controls for the conditional-assertion defect class.
// GitHub #273. See docs/audit/dca/0112-*.md.
//
// THE CLASS. A test computes something, then guards its only assertion on a
// property of that same computation:
//
//     if (!user.empty() && user.size() >= 3) {
//         EXPECT_EQ(body.find(user), std::string::npos);   // the leak check
//     }
//
// When the guard is false the test passes having asserted nothing, and it
// passes in exactly the circumstance where the property is least likely to
// hold. The two instances above were the account-name leak checks in
// `test_ui_suite_path_hygiene.cpp`: on a host where `GetUserNameW` fails, the
// test that a committed artifact carries no account name passed WITHOUT
// LOOKING. That is the same shape as 418 occurrences reaching six shipped
// binaries under #199 — a guard whose population was empty reporting zero.
//
// WHY THESE CASES AND NOT A C++ REIMPLEMENTATION OF THE DETECTOR.
// The detector is `tools/gate/check_magnitude_guard.py`: it parses the test
// tree, finds assertions whose only path is gated on a non-zero threshold, and
// requires each to be either unconditional / both-path, or carried in
// `magnitude_guard_baseline.json` with a written fixture-shape reason.
// Reimplementing that parse here would give two detectors to keep in step and
// a second place for the blindness to live. What these cases add is the thing
// the Python checker cannot give by itself: the detector running on the
// PRIMARY suite surface — the one a developer actually invokes — rather than
// only in `fast-checks.yml`. A guard wired to one surface is a guard that
// stops running the moment that surface changes (`guards_wired_two_surfaces`).
//
// BOTH DIRECTIONS, DELIBERATELY. A guard no input can redden is decoration; a
// guard that can only redden is equally useless. So one case asserts the
// detector FAILS on the planted fixtures and another that it PASSES on the
// live tree. Without the first, "the live tree is clean" would be
// indistinguishable from "the detector no longer detects anything" — which is
// precisely how #273 itself came to be misdiagnosed: the guard was run without
// `--baseline`, and a plan-shaped output was read as a defect list.

#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <cstdlib>
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

fs::path guard()
{
    return repoRoot() / "tools" / "gate" / "check_magnitude_guard.py";
}

fs::path baseline()
{
    return repoRoot() / "tools" / "gate" / "magnitude_guard_baseline.json";
}

fs::path fixtureDir() { return repoRoot() / "tools" / "gate" / "fixtures"; }

// Run the detector over `paths`. `withBaseline` mirrors CI's invocation; the
// difference is load-bearing rather than cosmetic, which is the whole lesson of
// #273: without `--baseline` the checker loads no adjudications and reports
// every suspect as unreviewed, so a plan-shaped listing is produced that looks
// exactly like a defect list.
int runGuard(const std::string& paths, bool withBaseline, std::string& out)
{
    const std::string python = PDFE_PYTHON_EXECUTABLE;
    if (python.empty()) return -1;
    std::string cmd = "set PYTHONIOENCODING=utf-8 && " + quoted(python) + " " +
                      quoted(guard().string()) + " --paths " + quoted(paths);
    if (withBaseline) cmd += " --baseline " + quoted(baseline().string());
    cmd += " 2>&1";
    return runCapturing(cmd, out);
}

// True when `needle` appears on a line that is not a `#` comment. Every
// mitigation in the workflow sits under prose that names the flag it explains,
// so a whole-file search would be satisfied by the explanation alone and a
// deleted step would keep this green. Measured on this programme's sibling
// file: two mutants survived exactly that way before the stripper existed.
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

// The workflow text of the ONE step that invokes `marker`, from the step's own
// `- name:` through to the start of the next step.
//
// WHY THIS EXISTS, MEASURED. The first version of the wiring case below asked
// whether `--baseline` appeared anywhere in fast-checks.yml. A mutation run
// that STRIPPED the flag from the magnitude-guard step left the case GREEN,
// because line 235 passes `--baseline` to an entirely different guard
// (`reachability_skip_baseline.json`). The token was present; the mechanism was
// gone. That is this file's own subject matter -- an assertion satisfied by
// something other than the property it names -- so the fix is to scope the
// window to the step and assert inside it.
//
// Returns empty when the marker is absent, which the caller must treat as a
// failure rather than as "nothing to check".
std::string stepInvoking(const std::string& workflow, const std::string& marker)
{
    const std::size_t at = workflow.find(marker);
    if (at == std::string::npos) return {};

    // Back up to this step's `- name:`; forward to the next one.
    const std::size_t begin = workflow.rfind("- name:", at);
    const std::size_t next = workflow.find("- name:", at);
    const std::size_t from = (begin == std::string::npos) ? 0 : begin;
    const std::size_t to = (next == std::string::npos) ? workflow.size() : next;
    return workflow.substr(from, to - from);
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// The NON-VACUITY control. Must come first in reading order: every other case
// in this file is worthless if the detector cannot detect.
//
// `tools/gate/fixtures/magnitude_guard_positive.cpp` holds two planted
// conditional assertions and is deliberately outside the `tests` tree so it
// cannot contaminate the live count. No baseline is passed, so both must be
// reported.
TEST(MagnitudeGuardControls, TheDetectorRednessOnPlantedViolations)
{
    const std::string python = PDFE_PYTHON_EXECUTABLE;
    ASSERT_FALSE(python.empty())
        << "no Python 3 interpreter at configure time, so the conditional-"
           "assertion detector cannot be exercised. A hard failure, not a "
           "skip: a skipped control is a control that is not running, and "
           "this file's other cases all assume this one passed.";
    ASSERT_TRUE(fs::exists(guard())) << guard().string() << " is missing";
    ASSERT_TRUE(fs::exists(fixtureDir() / "magnitude_guard_positive.cpp"))
        << "the planted-violation fixture is gone. Without it, "
           "TheLiveTestTreeHasNoUnreviewedConditionalAssertion proves nothing "
           "— a clean report would be indistinguishable from a detector that "
           "stopped detecting.";

    std::string out;
    const int rc = runGuard(fixtureDir().string(), /*withBaseline=*/false, out);

    EXPECT_NE(rc, 0)
        << "the detector exited 0 over a fixture directory containing planted "
           "conditional assertions. It cannot be relied on to redden anything, "
           "so every green it produces elsewhere is uninformative.\n\n"
        << out;

    // Named individually. A count of two would also be satisfied by finding
    // two different guards, and which two is the point of the fixture.
    for (const char* needle : {"elapsedMs > 100", "seconds >= 0.25"}) {
        EXPECT_NE(out.find(needle), std::string::npos)
            << "the report does not name the planted violation '" << needle
            << "'. The detector reddened, but not for the reason this control "
               "asserts.\n\n"
            << out;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// THE CLASS-LEVEL TEST (DCA 0112).
//
// Catches the whole family rather than the two instances: every assertion in
// the test tree whose only path is gated on a non-zero threshold must be
// either made unconditional / both-path, or adjudicated in the baseline with a
// written reason. A new test reintroducing the shape anywhere under `tests/`
// reddens here, on the primary suite surface, without anyone having to
// remember that #273 happened.
TEST(MagnitudeGuardControls, TheLiveTestTreeHasNoUnreviewedConditionalAssertion)
{
    const std::string python = PDFE_PYTHON_EXECUTABLE;
    ASSERT_FALSE(python.empty())
        << "no Python 3 interpreter at configure time";
    ASSERT_TRUE(fs::exists(baseline())) << baseline().string() << " is missing";

    std::string out;
    const int rc = runGuard((repoRoot() / "tests").string(),
                            /*withBaseline=*/true, out);

    EXPECT_EQ(rc, 0)
        << "a test gates its only assertion on a non-zero threshold and is "
           "not adjudicated in magnitude_guard_baseline.json.\n\n"
           "Fix the test — make the assertion unconditional, or assert on "
           "BOTH paths — or, if the condition really describes fixture shape "
           "rather than a measurement magnitude, add it to the baseline with "
           "a written reason. Do not silence this by deleting the "
           "assertion.\n\n"
        << out;
}

// ─────────────────────────────────────────────────────────────────────────────
// An adjudication with no reasoning is indistinguishable from a silenced
// guard, and is the cheapest way for this control to decay: the baseline grows
// an entry per inconvenience until it carries the whole defect class as
// "reviewed". The detector enforces presence of its keys; what it cannot
// enforce is that someone actually thought. A length floor is a crude proxy
// for that, and crude is enough to stop a one-word "fixture".
TEST(MagnitudeGuardControls, EveryBaselineEntryCarriesAWrittenReason)
{
    const std::string body = readFile(baseline());
    ASSERT_FALSE(body.empty()) << baseline().string() << " is empty or missing";

    // Deliberately not a JSON parse: this file has no JSON dependency and the
    // properties asserted are countable. `"classification"` and `"reason"` must
    // each appear once per entry, so a mismatch means an entry is missing one.
    const auto countOf = [&body](const std::string& needle) {
        std::size_t n = 0, at = 0;
        while ((at = body.find(needle, at)) != std::string::npos) {
            ++n;
            at += needle.size();
        }
        return n;
    };

    const std::size_t classifications = countOf("\"classification\"");
    const std::size_t reasons = countOf("\"reason\"");

    ASSERT_GT(classifications, 0u)
        << "no entries found in the baseline at all. Either the file's schema "
           "changed or it was emptied; both make "
           "TheLiveTestTreeHasNoUnreviewedConditionalAssertion weaker than it "
           "looks, since an empty baseline makes every suspect unreviewed and "
           "would redden loudly — but an absent FILE would too, for the wrong "
           "reason.";
    EXPECT_EQ(reasons, classifications)
        << "the baseline holds " << classifications << " classified entr(ies) "
           "but " << reasons << " reason(s). Every adjudication must say why; "
           "an entry recorded as reviewed with no reasoning is a silenced "
           "guard wearing a baseline entry's clothes.";
}

// ─────────────────────────────────────────────────────────────────────────────
// Both surfaces. This file is one; `fast-checks.yml` is the other, and it is
// the one that gates a push. The flags matter individually: dropping `--paths
// tests` would sweep the planted fixtures into the live count, and dropping
// `--baseline` would report all 25 adjudicated entries as unreviewed — the
// exact misreading that produced #273's false premise.
TEST(MagnitudeGuardControls, TheDetectorStaysWiredIntoFastChecks)
{
    const fs::path wf = repoRoot() / ".github" / "workflows" / "fast-checks.yml";
    const std::string body = readFile(wf);
    ASSERT_FALSE(body.empty()) << wf.string() << " is empty or missing";

    ASSERT_TRUE(inCodeLine(body, "check_magnitude_guard.py"))
        << "fast-checks.yml no longer invokes the conditional-assertion "
           "detector outside a comment. The control in this file would keep "
           "passing locally while nothing gated a push.";

    // Scoped to the step, not the file. See stepInvoking() for the mutation
    // that proved a file-wide search here was satisfied by a different guard's
    // --baseline on line 235 while this step's had been deleted.
    const std::string step = stepInvoking(body, "check_magnitude_guard.py");
    ASSERT_FALSE(step.empty())
        << "could not isolate the step invoking check_magnitude_guard.py, so "
           "the flags below cannot be attributed to it. Treated as a failure: "
           "an unlocatable step is not a verified one.";

    EXPECT_NE(step.find("--baseline"), std::string::npos)
        << "the detector is invoked without --baseline, so it loads no "
           "adjudications and reports every adjudicated entry as unreviewed. "
           "That is not a stricter check — it is a guaranteed red that invites "
           "being silenced by deleting the step, and it is the exact misreading "
           "that produced GitHub #273's false premise.\n\nstep:\n"
        << step;
    EXPECT_NE(step.find("--paths"), std::string::npos)
        << "the detector is invoked without --paths, so it no longer scans "
           "`tests` specifically and would sweep the planted control fixtures "
           "in tools/gate/fixtures into the live count.\n\nstep:\n"
        << step;

    EXPECT_TRUE(inCodeLine(body, "test_check_magnitude_guard.py"))
        << "the detector's own pytest suite is no longer run. It is what "
           "asserts the baseline enumerates every live shape match — the "
           "property whose single violation was the real cause of #273.";
}
