// -*- coding: utf-8 -*-
//
// F48 (deferred-backlog, follow-up phase-65) — the UI-suite runner wrote every
// path into its artifacts absolute and host-rooted.
//
// THE DEFECT. `tests/_runtime/discovery_phase8/ui-suite-summary.json` is
// COMMITTED, because F19DocClaims.ComparisonDocUiE2eCountsMatchTheSummary
// Artifact (test_f19_doc_claim_guards.cpp) cites it by name as the evidence
// behind a documented UI-E2E figure. It carried 338 absolute paths, 330
// distinct, every one rooted at the developer's home directory — so a username
// entered permanent git history. Measured, not assumed: 338/330 is what
// tools/gate/relativize_ui_summary.py reported when it rewrote the file, which
// matches the 2026-08-10 reverification's independent count exactly.
//
// AND NOTHING READ THEM. All three consumers read scalars only —
// test_f19_doc_claim_guards.cpp:398-430 (`failure_count`, `files_run`),
// windows-validation.yml's gating step (`failure_count`,
// `files_with_problems_count`), and tools/gate/verify_f19_doc_claims.py:186-192.
// The host-specific half of the artifact was pure cost.
//
// SIX INDEPENDENT CASES, and the count grew for a reason worth recording: this
// file started with three, and the `validator` gate found a fourth and then a
// fifth emission point that those three could not see. Each case exists because
// the others are blind to something:
//
//   * TheProducerRefusesToEmitHostRootedPaths runs the producer's own
//     `--self-test`. It exercises the REAL portable_path/scrub_text. A C++
//     re-implementation of the rule, or a grep of the script for a call, would
//     both stay green while the producer was broken — and a sweep keyed on
//     phrasing always comes back (WIN-45 escaped four times that way).
//
//   * ARealRunnerInvocationEmitsNothingHostRooted drives run_one() over a real
//     pytest subprocess and reads the FIVE artifacts it wrote. The self-test
//     proves the rule is right; only this proves it is APPLIED. Its first
//     version scanned three artifacts and passed while the raw JUnit XML and
//     the merged XML leaked — hence the >= 5 floor it now asserts.
//
//   * EveryDirectPytestStepInCiScrubsWhatItUploads covers the CI steps that
//     bypass the producer entirely by calling `pytest --junitxml` themselves.
//     No amount of rigour inside the module reaches those.
//
//   * TheArtifactScrubberActuallyRemovesHostPaths runs the scrubber's own
//     self-test, because a correctly wired call to a broken tool passes the
//     case above and fails silently in CI.
//
//   * NoCommittedRunArtifactCarriesAHostRootedPath reads the artifact directly
//     and consults nothing Python. If the script's self-test ever stopped
//     covering a field, this is what notices.
//
//   * EveryTrackedRunArtifactIsPortable generalises that to every git-tracked
//     JSON under tests/_runtime, so the NEXT artifact somebody commits is
//     covered without anyone remembering to extend a list. Scoped to TRACKED
//     files on purpose: an untracked local leftover from a pre-fix run is not
//     in permanent history and must not red another developer's build. That
//     over-breadth is the mistake the binary-staleness guard made one phase
//     earlier (DCA 0031) — it failed itself by sweeping a directory the
//     artifact under test was not built from.

#include <gtest/gtest.h>
// gtest-spi.h provides EXPECT_FATAL_FAILURE, which is how
// AnUnusableAccountNameFailsTheScanInsteadOfPassingIt asserts that an
// assertion FIRES. Without it the best a test could do is check the helper's
// return value, which would not prove the call sites actually fail.
#include <gtest/gtest-spi.h>

#include <array>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <lmcons.h>
#endif

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

// Run `cmd` with stderr folded into stdout so the child's own diagnostics land
// in the gtest failure message rather than a console nobody reads during a
// sharded run. Same helper shape as test_extraction_sidecar_conformance.cpp.
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

// The account this build is running as. Used as a second, independent signal
// to the structural path check: a path can be host-rooted without being
// absolute-looking (a bare "Users/<name>/..." fragment), and a username is the
// specific thing F48 is about keeping out of history.
std::string currentUserName()
{
#if defined(_WIN32)
    wchar_t buf[UNLEN + 1] = {};
    DWORD len = UNLEN + 1;
    if (!::GetUserNameW(buf, &len) || len <= 1) return {};
    std::string out;
    for (DWORD i = 0; i + 1 < len; ++i) {
        out.push_back(static_cast<char>(buf[i] & 0x7F));
    }
    return out;
#else
    const char* u = std::getenv("USER");
    return u ? std::string(u) : std::string();
#endif
}

// What an account-name leak scan concluded, and whether it actually looked.
//
// GitHub #273. The three leak checks in this file each used to decide for
// themselves whether the account name was usable, inline, as the condition of
// an `if` wrapped around their only assertion (two cases) or around a
// GTEST_SKIP (one case). All three therefore had the same property: when the
// name could not be determined, the check that a committed artifact does not
// contain it reported a NON-FAILURE without looking at anything.
//
// `looked` is the point of this type. It makes "searched, found nothing"
// structurally distinct from "did not search" — which is the whole defect
// class, and which a bare `bool clean` cannot express.
struct AccountNameScan {
    bool        looked = false;  ///< did we really search the body?
    bool        clean  = true;   ///< meaningful ONLY when `looked`
    std::size_t offset = 0;      ///< first hit, when `looked && !clean`
    std::string unusable;        ///< non-empty => the NAME was unusable input
};

// Search `body` for `user`.
//
// An empty or shorter-than-3-character `user` is UNUSABLE INPUT, not a
// satisfied property: the scan reports `unusable` and leaves `looked` false,
// and every caller must treat that as a FAILURE. Returning a clean verdict for
// a name we could not search for is precisely how 418 occurrences of this
// string reached six shipped, signed binaries (GitHub #199).
//
// Taking the name as a PARAMETER rather than calling currentUserName() is what
// makes the unusable-input behaviour testable: see
// UiSuitePathHygiene.AnUnusableAccountNameFailsTheScanInsteadOfPassingIt,
// which drives both unusable values and — the half none of these checks
// previously had — a planted positive, proving the search can find a name at
// all. Without that control, a find() that always returned npos would have
// reported every artifact clean.
AccountNameScan scanForAccountName(const std::string& body,
                                   const std::string& user)
{
    AccountNameScan out;
    if (user.empty()) {
        out.unusable =
            "cannot determine the build account name, so the check that this "
            "artifact does not contain it cannot run. Failing rather than "
            "passing: a green here would mean 'not checked', which is how 418 "
            "occurrences of this string reached six shipped binaries "
            "(GitHub #199).";
        return out;
    }
    if (user.size() < 3) {
        out.unusable =
            "the build account name is " + std::to_string(user.size()) +
            " characters, too short to search for without matching unrelated "
            "text. That is unusable input for this check, not a pass — fix the "
            "environment or this case, do not let it go green.";
        return out;
    }
    out.looked = true;
    const std::size_t at = body.find(user);
    out.clean = (at == std::string::npos);
    out.offset = out.clean ? 0 : at;
    return out;
}

// Windows drive-letter or UNC roots, matching _ABS_PATH_RE in run_ui_suite.py.
// Kept as a literal here rather than shared with the script: this half is meant
// to be independent of the producer, so it states the property itself.
const std::regex& absolutePathRe()
{
    static const std::regex re(R"((?:[A-Za-z]:[\\/]|\\\\)[^\s"'<>|*?\r\n]*)");
    return re;
}

std::vector<std::string> absolutePathsIn(const std::string& text)
{
    std::vector<std::string> hits;
    for (auto it = std::sregex_iterator(text.begin(), text.end(),
                                        absolutePathRe());
         it != std::sregex_iterator(); ++it) {
        hits.push_back(it->str());
    }
    return hits;
}

// git-tracked files under `rel`. Empty vector distinguishes "git could not be
// run" (reported by the caller as a hard failure) from "git ran and the tree is
// empty", which the caller also treats as a failure — there is at least one
// tracked artifact and a guard that finds none is guarding nothing.
std::vector<std::string> trackedFilesUnder(const std::string& rel, bool& gitRan)
{
    const std::string cmd = "git -C " + quoted(repoRoot().string()) +
                            " ls-files -- " + quoted(rel) + " 2>&1";
    std::string out;
    const int rc = runCapturing(cmd, out);
    gitRan = (rc == 0);
    std::vector<std::string> files;
    if (!gitRan) return files;
    std::istringstream in(out);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        if (!line.empty()) files.push_back(line);
    }
    return files;
}

// Report at most `n` offenders — a summary artifact with a systemic leak has
// hundreds, and dumping all of them buries the count that matters.
std::string sample(const std::vector<std::string>& hits, size_t n = 5)
{
    std::ostringstream ss;
    for (size_t i = 0; i < hits.size() && i < n; ++i) {
        ss << "\n    " << hits[i];
    }
    if (hits.size() > n) ss << "\n    ... and " << (hits.size() - n) << " more";
    return ss.str();
}

const char* kNamedArtifact =
    "tests/_runtime/discovery_phase8/ui-suite-summary.json";

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Half one: the producer itself.
// ─────────────────────────────────────────────────────────────────────────────
TEST(UiSuitePathHygiene, TheProducerRefusesToEmitHostRootedPaths)
{
    const std::string python = PDFE_PYTHON_EXECUTABLE;
    ASSERT_FALSE(python.empty())
        << "no Python 3 interpreter was found at configure time, so the UI "
           "runner's path-portability contract cannot be checked. A hard "
           "failure rather than a skip: a build that cannot verify the "
           "producer must not report the producer as verified.";

    const fs::path script = repoRoot() / "tests" / "scripts" / "run_ui_suite.py";
    ASSERT_TRUE(fs::exists(script))
        << "run_ui_suite.py is missing: " << script.string();

    const std::string cmd = "set PYTHONIOENCODING=utf-8 && " + quoted(python) +
                            " " + quoted(script.string()) + " --self-test 2>&1";
    std::string output;
    const int rc = runCapturing(cmd, output);

    EXPECT_EQ(rc, 0)
        << "run_ui_suite.py --self-test reported a break in the F48 "
           "path-portability contract. Each line names the case, what it "
           "produced and what it must produce.\n\n"
        << output;

    // Exit 0 is necessary, not sufficient: a self-test whose case list had
    // been emptied would also exit 0, having checked nothing, while making the
    // run LOOK protected. That is strictly worse than no guard.
    const auto pos = output.find("self-test: ");
    ASSERT_NE(pos, std::string::npos)
        << "--self-test exited " << rc << " without printing its check count; "
           "it may have terminated early.\n\n" << output;
    const int checks = std::atoi(output.c_str() +
                                 static_cast<long>(pos) + 11);
    EXPECT_GE(checks, 10)
        << "the self-test ran only " << checks << " checks. The contract has "
           "more cases than that (repo-relative, out-of-repo output, redacted "
           "foreign path, UNC, idempotence, case-insensitive root, text "
           "scrubbing); a shrunken list means cases were removed.\n\n"
        << output;
}

// ─────────────────────────────────────────────────────────────────────────────
// Half one-and-a-half: a REAL invocation, inspecting what was WRITTEN.
//
// The self-test above proves the RULE is right. It cannot prove the rule is
// APPLIED everywhere a value leaves the runner, and that is where three of the
// four F48 emission points were — two of them (a failure MESSAGE and pytest's
// own per-file .log) in fields whose names say nothing about paths.
//
// This is also the case that found the hole in the rule itself: pytest prints
// `rootdir: C:\Users\<account>` when it cannot locate a project root, and the
// `[redacted]/<basename>` form turned that into `[redacted]/<account>` —
// redacted in shape and leaking in substance. A synthetic self-test would not
// have produced that input.
// ─────────────────────────────────────────────────────────────────────────────
TEST(UiSuitePathHygiene, ARealRunnerInvocationEmitsNothingHostRooted)
{
    const std::string python = PDFE_PYTHON_EXECUTABLE;
    ASSERT_FALSE(python.empty())
        << "no Python 3 interpreter was found at configure time, so a real "
           "runner invocation cannot be driven. A hard failure rather than a "
           "skip.";

    const fs::path probe = repoRoot() / "tools" / "gate" /
                           "f48_producer_probe.py";
    ASSERT_TRUE(fs::exists(probe))
        << "f48_producer_probe.py is missing: " << probe.string();

    const std::string cmd = "set PYTHONIOENCODING=utf-8 && " + quoted(python) +
                            " " + quoted(probe.string()) + " 2>&1";
    std::string output;
    const int rc = runCapturing(cmd, output);

    EXPECT_EQ(rc, 0)
        << "a real run_one() invocation over a real pytest subprocess wrote "
           "host-rooted data into an artifact. Each line names the artifact "
           "and the offending string.\n\n"
        << output;

    // The probe self-reports how many artifacts it looked at, and internally
    // requires its pytest to have actually failed once. Both matter: a probe
    // whose subprocess never ran would inspect empty strings and pass.
    const auto pos = output.find("OK: ");
    ASSERT_NE(pos, std::string::npos)
        << "the probe exited " << rc << " without reporting what it "
           "inspected; it may have terminated early.\n\n" << output;
    const int inspected = std::atoi(output.c_str() +
                                    static_cast<long>(pos) + 4);

    // Five artifacts, and the floor is not decoration. The FIRST version of
    // this fix scrubbed three of them and the probe passed, because the two it
    // missed -- the raw per-file JUnit XML and the merged XML built from it --
    // were not in the scanned set. A probe that silently stops covering an
    // artifact is indistinguishable from one that covers it and finds nothing.
    EXPECT_GE(inspected, 5)
        << "the probe inspected only " << inspected << " artifacts. It must "
           "cover the run_one() record, the per-file log, the per-file JUnit "
           "XML, the merged JUnit XML and collect_failures()' output. A "
           "shrunken set means an artifact stopped being checked.\n\n"
        << output;
}

// ─────────────────────────────────────────────────────────────────────────────
// The CI side. Everything above guards the PRODUCER; this guards the workflow
// steps that bypass it.
//
// The validator gate found this after it had already found the JUnit-XML miss:
// `windows-validation.yml`'s render-verification step invokes
// `pytest --junitxml=...` DIRECTLY. It never calls run_one(), so none of the
// scrubbing above applies to it — and it builds its test list from
// `Get-ChildItem | ForEach-Object { $_.FullName }`, so absolute paths go
// straight in. `render-junit.xml` and `render-suite.log` are then uploaded with
// 14-day retention carrying the runner account name.
//
// The five emission points closed in the producer were each closed correctly.
// The COMPLETENESS CLAIM around them was what was wrong, and a claim like that
// needs a mechanical check rather than another careful reading.
//
// This is structural, not phrasing-keyed: it finds every step that runs pytest
// with --junitxml and requires that step to either go through a runner that
// scrubs, or invoke the scrubber itself. A new step added tomorrow is covered
// without anyone remembering this rule. (A sweep keyed on wording always comes
// back — WIN-45 escaped four times that way.)
// ─────────────────────────────────────────────────────────────────────────────
TEST(UiSuitePathHygiene, EveryDirectPytestStepInCiScrubsWhatItUploads)
{
    const fs::path wf = repoRoot() / ".github" / "workflows" /
                        "windows-validation.yml";
    const std::string body = readFile(wf);
    ASSERT_FALSE(body.empty()) << "missing workflow: " << wf.string();

    // Split into steps on the `      - name:` boundary the file uses.
    const std::string kStepMarker = "\n      - name:";
    std::vector<std::string> steps;
    size_t at = body.find(kStepMarker);
    while (at != std::string::npos) {
        const size_t next = body.find(kStepMarker, at + 1);
        steps.push_back(body.substr(
            at, (next == std::string::npos ? body.size() : next) - at));
        at = next;
    }
    ASSERT_FALSE(steps.empty())
        << "no workflow steps parsed — this guard would pass vacuously. The "
           "step indentation in windows-validation.yml may have changed.";

    // Only CODE lines count, never comments. The first version of this guard
    // searched the whole step text and was immediately fooled by a comment in
    // the very step it was written for — the comment explaining that the step
    // does NOT go through run_ui_suite.py made the guard skip it as if it did.
    // A guard that reads prose is a guard keyed on phrasing.
    const auto mentionedInCode = [](const std::string& step,
                                    const std::string& needle) {
        std::istringstream in(step);
        std::string line;
        while (std::getline(in, line)) {
            const size_t first = line.find_first_not_of(" \t\r");
            if (first == std::string::npos || line[first] == '#') continue;
            if (line.find(needle) != std::string::npos) return true;
        }
        return false;
    };

    std::vector<std::string> offenders;
    int directPytestSteps = 0;
    for (const auto& step : steps) {
        if (!mentionedInCode(step, "--junitxml")) continue;
        // A step that delegates to a runner inherits that runner's scrub.
        if (mentionedInCode(step, "run_ui_suite.py")) continue;
        if (mentionedInCode(step, "run_render_suite.py")) continue;
        ++directPytestSteps;
        if (!mentionedInCode(step, "scrub_artifacts.py")) {
            const size_t nl = step.find('\n', 1);
            offenders.push_back(step.substr(0, nl == std::string::npos
                                                   ? step.size()
                                                   : nl));
        }
    }

    // Non-vacuity. There IS such a step (render-verification). If this drops to
    // zero the guard has stopped finding anything and would pass while covering
    // nothing — the failure mode DCA 0031 records.
    EXPECT_GE(directPytestSteps, 1)
        << "no step was found invoking pytest --junitxml directly. Either the "
           "workflow changed shape or this guard's parser no longer matches "
           "it; a guard that inspects nothing is worse than no guard.";

    EXPECT_TRUE(offenders.empty())
        << "these workflow steps run pytest --junitxml themselves and never "
           "scrub what they upload, so the runner's account name reaches a "
           "retained artifact:"
        << sample(offenders)
        << "\n    Fix: invoke tools/gate/scrub_artifacts.py on the produced "
           "files before the step exits, or route the step through "
           "run_ui_suite.py / run_render_suite.py.";
}

// The scrubber the step above depends on must itself work. Same two-halves
// reasoning as the producer: the workflow guard proves the CALL is wired, this
// proves the thing it calls does something.
TEST(UiSuitePathHygiene, TheArtifactScrubberActuallyRemovesHostPaths)
{
    const std::string python = PDFE_PYTHON_EXECUTABLE;
    ASSERT_FALSE(python.empty()) << "no Python 3 interpreter at configure time";

    const fs::path tool = repoRoot() / "tools" / "gate" / "scrub_artifacts.py";
    ASSERT_TRUE(fs::exists(tool)) << "missing: " << tool.string();

    const std::string cmd = "set PYTHONIOENCODING=utf-8 && " + quoted(python) +
                            " " + quoted(tool.string()) + " --self-test 2>&1";
    std::string output;
    const int rc = runCapturing(cmd, output);
    EXPECT_EQ(rc, 0) << "scrub_artifacts.py --self-test failed:\n\n" << output;

    const auto pos = output.find("self-test: ");
    ASSERT_NE(pos, std::string::npos)
        << "no check count reported; the self-test may have exited early.\n\n"
        << output;
    EXPECT_GE(std::atoi(output.c_str() + static_cast<long>(pos) + 11), 8)
        << "the scrubber's self-test shrank; it must still cover a log, an "
           "XML, idempotence, a missing file and a clean file.\n\n" << output;
}

// ─────────────────────────────────────────────────────────────────────────────
// GitHub #273 — the standing control for the three cases below it.
//
// Read this one first. The three leak checks in this file each assert that a
// committed artifact does not contain the build account name; this asserts
// that those checks FAIL when they cannot do their job, instead of reporting
// success. Before the fix, all three gated themselves on the name being
// determinable and >= 3 characters, so on a host where GetUserNameW fails they
// passed without looking — and a green result was indistinguishable from a
// clean artifact.
//
// EXPECT_FATAL_FAILURE, not a return-value check. Asserting that
// scanForAccountName() *reports* unusable input would only test the classifier;
// what matters is that the ASSERT_ at the call sites actually fires and reddens
// the case. The helper below is the call sites' precondition in miniature, so
// this case exercises the real failure path.
//
// Four properties, each able to fail on its own. The third is the half none of
// these checks previously had: proof the search can find a name at all. Without
// it a find() that always returned npos would report every artifact clean —
// the same vacuous-oracle hole one level down.
// ─────────────────────────────────────────────────────────────────────────────
namespace {

// The call sites' precondition, extracted so EXPECT_FATAL_FAILURE can drive it.
// Must be a void function taking no captured locals: the macro runs the
// statement inside a helper class.
void assertAccountNameUsable(const char* body, const char* user)
{
    const AccountNameScan scan = scanForAccountName(body, user);
    ASSERT_TRUE(scan.unusable.empty()) << scan.unusable;
}

} // namespace

TEST(UiSuitePathHygiene, AnUnusableAccountNameFailsTheScanInsteadOfPassingIt)
{
    // 1. Undeterminable (GetUserNameW failed). The assertion must FIRE.
    EXPECT_FATAL_FAILURE(assertAccountNameUsable("any body text", ""),
                         "cannot determine the build account name");
    {
        const AccountNameScan s = scanForAccountName("any body text", "");
        EXPECT_FALSE(s.unusable.empty());
        EXPECT_FALSE(s.looked) << "reported unusable input but claims to have "
                                  "searched; `looked` is what distinguishes "
                                  "'found nothing' from 'did not look'.";
    }

    // 2. Too short to search for without matching unrelated text.
    EXPECT_FATAL_FAILURE(assertAccountNameUsable("any body text", "ab"),
                         "too short to search for");
    {
        const AccountNameScan s = scanForAccountName("any body text", "ab");
        EXPECT_FALSE(s.unusable.empty());
        EXPECT_FALSE(s.looked);
    }

    // 3. POSITIVE CONTROL. A planted name must be found, at the right offset.
    //    This is the non-vacuity half: it proves the scan is capable of
    //    failing, which is what makes the clean verdicts in the cases below
    //    mean anything.
    {
        const std::string needle = "Plausible";
        const AccountNameScan s =
            scanForAccountName("prefix " + needle + " suffix", needle);
        EXPECT_TRUE(s.unusable.empty()) << s.unusable;
        EXPECT_TRUE(s.looked);
        EXPECT_FALSE(s.clean)
            << "the scan did not find a name planted in the body. Every "
               "'clean' verdict this file reports would be worthless.";
        EXPECT_EQ(s.offset, 7u) << "found it, but at the wrong offset";
    }

    // 4. And the negative: a body without the name is clean, HAVING LOOKED.
    {
        const AccountNameScan s =
            scanForAccountName("nothing to see here", "Plausible");
        EXPECT_TRUE(s.unusable.empty()) << s.unusable;
        EXPECT_TRUE(s.looked);
        EXPECT_TRUE(s.clean);
        EXPECT_EQ(s.offset, 0u);
    }

    // 5. The real account name must itself be usable on this host — otherwise
    //    the three cases below are about to fail, and this says why in one
    //    place rather than three.
    const AccountNameScan live =
        scanForAccountName("nothing to see here", currentUserName());
    EXPECT_TRUE(live.unusable.empty())
        << "this host's account name is unusable, so the leak checks below "
           "will fail by design: " << live.unusable;
}

// ─────────────────────────────────────────────────────────────────────────────
// Half two: the committed artifact, read directly. Consults nothing Python.
// ─────────────────────────────────────────────────────────────────────────────
TEST(UiSuitePathHygiene, NoCommittedRunArtifactCarriesAHostRootedPath)
{
    const fs::path artifact = repoRoot() / kNamedArtifact;
    const std::string body = readFile(artifact);
    ASSERT_FALSE(body.empty())
        << kNamedArtifact << " is missing or empty. It is committed on "
           "purpose — F19DocClaims.ComparisonDocUiE2eCountsMatchTheSummary"
           "Artifact cites it by name as its evidence.";

    const auto hits = absolutePathsIn(body);
    EXPECT_TRUE(hits.empty())
        << kNamedArtifact << " carries " << hits.size()
        << " absolute path(s). This artifact is in permanent git history and "
           "no consumer reads a path from it — all three read scalars only. "
           "Regenerate with: python tools/gate/relativize_ui_summary.py "
        << kNamedArtifact << sample(hits);

    // GitHub #273. This assertion used to sit behind
    //     if (!user.empty() && user.size() >= 3)
    // which meant that on any host where GetUserNameW fails, or where the
    // account name is one or two characters, the check that a committed
    // artifact carries no account name PASSED WITHOUT LOOKING AT ANYTHING.
    // A green result could mean "the artifact is clean" or "we never looked",
    // and nothing distinguished them.
    //
    // Both of those are UNUSABLE INPUT, not a satisfied property, so they now
    // FAIL. ASSERT_ rather than EXPECT_ because there is nothing useful to
    // check afterwards if we cannot name what we are searching for.
    // The account name is deliberately NOT interpolated into these messages.
    // A leak check that prints the string it hunts becomes the next producer,
    // and these messages land in CI logs that get pasted into issues. The
    // offset is what a reader needs in order to act.
    const AccountNameScan scan = scanForAccountName(body, currentUserName());
    ASSERT_TRUE(scan.unusable.empty()) << scan.unusable;
    ASSERT_TRUE(scan.looked)
        << "the scan reported usable input but did not search; that "
           "combination is impossible unless scanForAccountName has been "
           "changed, and it would mean this case is no longer checking "
           "anything.";
    EXPECT_TRUE(scan.clean)
        << kNamedArtifact << " contains the account name this build is running "
           "as (first at byte offset " << scan.offset
        << "; the name itself is not printed here on purpose). Even without a "
           "drive letter that is host-identifying data entering permanent "
           "history.";
}

// ─────────────────────────────────────────────────────────────────────────────
// Half three: every tracked run artifact, so the next one is covered too.
// ─────────────────────────────────────────────────────────────────────────────
TEST(UiSuitePathHygiene, EveryTrackedRunArtifactIsPortable)
{
    bool gitRan = false;
    const auto tracked = trackedFilesUnder("tests/_runtime", gitRan);
    ASSERT_TRUE(gitRan)
        << "could not run `git ls-files` to enumerate tracked run artifacts. "
           "Failing rather than skipping: the point of this case is that a "
           "NEWLY committed artifact is covered without anyone extending a "
           "list, and a silent skip would hand that back.";

    std::vector<std::string> jsons;
    for (const auto& f : tracked) {
        if (f.size() > 5 && f.compare(f.size() - 5, 5, ".json") == 0) {
            jsons.push_back(f);
        }
    }
    ASSERT_FALSE(jsons.empty())
        << "no tracked *.json found under tests/_runtime. At least "
        << kNamedArtifact << " is tracked, so an empty enumeration means this "
           "guard is looking in the wrong place and is passing vacuously.";

    for (const auto& rel : jsons) {
        const std::string body = readFile(repoRoot() / rel);
        if (body.empty()) continue;
        const auto hits = absolutePathsIn(body);
        EXPECT_TRUE(hits.empty())
            << rel << " carries " << hits.size() << " absolute path(s). Every "
               "committed run artifact must be host-independent; regenerate "
               "with tools/gate/relativize_ui_summary.py." << sample(hits);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Half four: the gtest-side producer, which the F48 sweep never covered.
//
// EveryTrackedRunArtifactIsPortable above enumerates `tests/_runtime` ONLY. It
// is titled "every tracked run artifact" and reads as class-wide coverage, but
// its corpus is one directory — so a run artifact written anywhere else is
// invisible to it. `tests/support/ConformanceChecker.cpp` writes exactly such
// an artifact into `docs/audit/`, and on 2026-08-25 running the suite was
// measured taking tracked-file occurrences of the operator's account name from
// 0 to 9. That is the same string the 2026-08-22 history rewrite existed to
// remove from this repository, regenerated on every run.
//
// Why F48 missed it: all five emission points it fixed were in the PYTHON UI
// runner, and `tools/gate/scrub_artifacts.py` covers CI-PRODUCED artifacts.
// Neither reaches a C++ producer writing into a tracked docs path.
//
// This case is deliberately about the ARTIFACT rather than the producer, so it
// keeps holding if the record is ever written by something else.
// ─────────────────────────────────────────────────────────────────────────────
TEST(UiSuitePathHygiene, TheTrackedConformanceRecordIsPortable)
{
    const std::string rel =
        "docs/audit/meta-audit-2026-07/followup/phase-14/"
        "conformance-checker-record.json";
    const fs::path artifact = repoRoot() / rel;

    const std::string body = readFile(artifact);
    ASSERT_FALSE(body.empty())
        << rel << " is missing or empty. It is TRACKED and is rewritten by "
           "ConformanceChecker.cpp on every suite run, so an empty read means "
           "this guard is looking in the wrong place and passing vacuously.";

    // Scan the DECODED value, not the JSON source. In JSON every backslash is
    // doubled, so a perfectly portable "<repo>\\tools\\..." contains the
    // literal two-character sequence `\\`, which absolutePathRe() matches as a
    // UNC root and reports as an absolute path. Unescaping first removes that
    // false positive WITHOUT weakening detection: a genuine leak is written as
    // "C:\\Users\\someone\\..." and decodes to "C:\Users\someone\...", which
    // still matches the `[A-Za-z]:[\\/]` branch. The sibling case above never
    // hit this only because its artifact uses forward slashes.
    std::string decoded;
    decoded.reserve(body.size());
    for (std::size_t i = 0; i < body.size(); ++i) {
        if (body[i] == '\\' && i + 1 < body.size() && body[i + 1] == '\\') {
            decoded += '\\';
            ++i;
        } else {
            decoded += body[i];
        }
    }

    const auto hits = absolutePathsIn(decoded);
    EXPECT_TRUE(hits.empty())
        << rel << " carries " << hits.size() << " absolute path(s). This file "
           "is tracked, so a suite run would commit the build machine's "
           "directory layout into permanent history. ConformanceChecker.cpp's "
           "portablePath() must reduce them to "
           "<repo>/<home>/<temp>/<verapdf>. Note <verapdf>: a fresh checkout "
           "has no tools/conformance/verapdf (it is fetched, not tracked), so "
           "the resolver falls through to a conventional system root such as "
           "C:/verapdf and that root must be scrubbed too -- which is how CI "
           "run 37366537472 failed here while a developer box with the "
           "provisioned copy passed."
        << sample(hits);

    // GitHub #273 — same defect as the sibling case above, same remedy.
    // Gating this on the account name being present and >= 3 characters meant
    // the check could pass without ever running. Unusable input now FAILS.
    // Name deliberately not interpolated; see the sibling case. Offset only.
    const AccountNameScan scan = scanForAccountName(body, currentUserName());
    ASSERT_TRUE(scan.unusable.empty()) << rel << ": " << scan.unusable;
    ASSERT_TRUE(scan.looked)
        << "usable input but no search performed; see the sibling case.";
    EXPECT_TRUE(scan.clean)
        << rel << " contains the account name this build runs as (first at "
           "byte offset " << scan.offset
        << "; not printed here on purpose). This repository had its history "
           "rewritten across 1,290 commits to remove exactly that string; a "
           "producer that writes it back into a tracked file undoes the "
           "rewrite on every run.";
}

// ─────────────────────────────────────────────────────────────────────────────
// CLASS-LEVEL (DCA 0042). The family is not "the conformance record leaks" — it
// is "SOME producer writes the build account's identity into a tracked file".
// Every control that existed before this one was scoped to a producer or a
// directory (F48 → the Python UI runner; scrub_artifacts.py → CI-produced
// artifacts; EveryTrackedRunArtifactIsPortable → tests/_runtime), so each could
// only ever catch the instances inside its own scope. A fourth producer landing
// anywhere else was invisible to all three, which is exactly what happened.
//
// This case is scoped to the PROPERTY instead: no tracked file, anywhere in the
// tree, may contain the account name this build runs as. It therefore covers
// producers that do not exist yet, in directories nobody has thought of.
//
// It deliberately asserts on the ACCOUNT NAME rather than on absolute paths in
// general: 19 of the 37 tracked JSON artifacts under docs/audit currently carry
// host-rooted paths that were scrubbed to a placeholder account
// ("C:\Users\dev\..."). Those are a real tidiness debt but NOT an identity leak,
// and folding them in here would make this case fail for a reason unrelated to
// the property it exists to protect. Absolute-path hygiene for those 19 is
// tracked separately.
//
// `git grep` is the oracle rather than a hand-rolled walk: it already honours
// .gitignore and skips binary files, and using it means the test asks the same
// question a reviewer would.
// ─────────────────────────────────────────────────────────────────────────────
TEST(UiSuitePathHygiene, NoTrackedFileCarriesTheBuildAccountName)
{
    // GitHub #273, third door into the same defect. This was:
    //
    //     if (user.size() < 3) { GTEST_SKIP() << "...nothing to assert..."; }
    //
    // The two sibling cases above gated an ASSERTION; this one gated the whole
    // case with a SKIP, which is worse in this project: a skip announces "not
    // run" and is then counted as a non-failure by the gtest summary, the
    // merged XML and the skip baseline alike. On a host where the account name
    // is undeterminable, the control this file's own comment calls "the only
    // class-level control over the whole tree" quietly did not run.
    //
    // It is also the reason the detector never saw it: check_magnitude_guard.py
    // keys on conditional ASSERTIONS, and GTEST_SKIP is not one, so this shape
    // is structurally invisible to it. Filed separately as its own gap.
    //
    // Unusable input is a FAILURE here, exactly as in the siblings.
    const std::string user = currentUserName();
    ASSERT_FALSE(user.empty())
        << "cannot determine the build account name, so the only tree-wide "
           "account-name leak control cannot run. Failing rather than "
           "skipping: a skip hands back the one class-level guarantee this "
           "file provides, and reports it as a non-failure.";
    ASSERT_GE(user.size(), 3u)
        << "the build account name is " << user.size() << " characters, too "
           "short to `git grep` for without matching arbitrary text. That is "
           "unusable input, not 'nothing to assert'.";

    const std::string cmd = "git -C " + quoted(repoRoot().string()) +
                            " grep -I -l -F -- " + quoted(user) + " 2>&1";
    std::string out;
    const int rc = runCapturing(cmd, out);

    // git grep exits 1 with no output when nothing matches — that is the pass.
    // Any other non-zero, or output, needs looking at.
    std::vector<std::string> hits;
    std::istringstream in(out);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        if (!line.empty()) hits.push_back(line);
    }

    ASSERT_TRUE(rc == 0 || rc == 1)
        << "`git grep` did not run (rc=" << rc << "). Failing rather than "
           "skipping: this case is the only class-level control over the whole "
           "tree, and a silent skip would hand that back. Output: " << out;

    // The name is NOT printed, for the same reason as the sibling cases: this
    // message lands in CI logs that get pasted into issues, so a leak check
    // that echoes what it hunts becomes the next producer. The file list below
    // is what a reader needs in order to act.
    EXPECT_TRUE(hits.empty())
        << hits.size() << " TRACKED file(s) contain the build account name ("
        << user.size() << " characters, not printed here on purpose). This "
           "repository had its history rewritten across 1,290 "
           "commits, over three separate leak surfaces, and was finally "
           "recreated to drop PR refs — all to remove that string. A producer "
           "that writes it back into a tracked file undoes that on every run, "
           "and the repository is intended to become public."
        << sample(hits);
}
