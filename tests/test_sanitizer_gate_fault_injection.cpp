// SEC-21 — fault injection against the MSVC-ASan CI gate.
//
// The gap-register row reads: "Inject a deliberate heap-use-after-free into a
// test-only fixture, dispatch windows-sanitizer.yml, and confirm the job
// conclusion is `failure`." Dispatching the workflow is not available to this
// suite (and CI in this project is manual-only by standing instruction), so
// the injection was performed HEADLESSLY and the gate's own logic is driven
// here over the artifacts that injection produced.
//
// What was actually run, and what came back
// -----------------------------------------
// A deliberate heap-use-after-free was compiled with
//   cl /EHsc /Zi /fsanitize=address uaf.cpp
// and executed under this lane's exact options:
//   ASAN_OPTIONS=halt_on_error=0:print_stacktrace=1:detect_leaks=0:
//                abort_on_error=0:log_path=asan_ci_finding
//
// The row's literal question answers YES: the process exits 1 (ASan's default
// exitcode), so the job would conclude `failure`. But the run also exposed two
// defects in HOW the gate reaches that conclusion, and both are about where
// the evidence is rather than what counts as a finding:
//
//   1. THE DOCUMENTED REDUNDANCY DOES NOT EXIST. The workflow said log_path
//      makes ASan "ALSO" write findings to <base>.<pid> files and that it
//      scans "BOTH" that and stdout. Setting log_path MOVES the report off
//      stderr; it does not copy it. The captured stdout of a genuine
//      heap-use-after-free contains ZERO "==<pid>==ERROR:" lines. The stdout
//      scan — the original detector — has been dead since the log_path
//      hardening landed, so a two-source design had one live source.
//
//   2. THAT SOURCE WAS COUPLED TO THE PROCESS CWD. log_path must be relative
//      and colon-free (a drive-letter colon is an ASAN_OPTIONS separator and
//      makes ASan abort), so the runtime resolves it against the process CWD.
//      Scanning only $GITHUB_WORKSPACE worked because the step's CWD happened
//      to equal it — an invariant nothing asserted. Run the same binary from
//      anywhere else and a real finding is counted as zero.
//
// Neither defect could flip the job to green on its own, because ASan's exit
// code carries the failure. What they do is make a failing run REPORT zero
// findings and never upload the report file — which is how a genuine
// memory-safety finding gets triaged as a flaky exit.
//
// The fixtures under tests/fixtures/sanitizer/ are the verbatim stdout and
// report file from that run. They are real instrument output; nothing here is
// a hand-written sample of what ASan "would" print.

#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::filesystem::path repoRoot()
{
    return std::filesystem::path(PDFE_REPO_ROOT);
}

std::filesystem::path fixtureDir()
{
    return repoRoot() / "tests" / "fixtures" / "sanitizer";
}

std::filesystem::path gateScript()
{
    return repoRoot() / "tests" / "scripts" / "asan_gate_verdict.ps1";
}

std::string readFile(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int runCapturing(const std::string& cmd, std::string& out)
{
    out.clear();
#if defined(_WIN32)
    FILE* pipe = ::_popen(cmd.c_str(), "r");
#else
    FILE* pipe = ::popen(cmd.c_str(), "r");
#endif
    if (!pipe) return -1;
    std::array<char, 4096> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe)) {
        out += buf.data();
    }
#if defined(_WIN32)
    return ::_pclose(pipe);
#else
    return ::pclose(pipe);
#endif
}

// Invoke the gate script exactly as the workflow does and return its JSON.
// `extraArgs` is appended verbatim so each test states its own scenario.
std::string runGate(const std::string& extraArgs)
{
    std::string cmd = "pwsh -NoProfile -File \"" + gateScript().string() + "\" " + extraArgs;
    std::string out;
    // The script exits 1 on a FAIL verdict by design, so the exit code is not
    // an error signal here — the JSON is.
    (void)runCapturing(cmd + " 2>&1", out);
    return out;
}

// Deliberately primitive: the script emits a single compressed JSON object, so
// a substring probe is unambiguous and avoids pulling a JSON library into the
// test tree for four assertions.
bool jsonHas(const std::string& json, const std::string& keyColonValue)
{
    return json.find(keyColonValue) != std::string::npos;
}

// A scratch directory unique to the calling test, so a leftover file from a
// previous run cannot make a scenario pass or fail for the wrong reason.
std::filesystem::path freshDir(const std::string& name)
{
    auto d = std::filesystem::temp_directory_path() / ("pdfe_sec21_" + name);
    std::error_code ec;
    std::filesystem::remove_all(d, ec);
    std::filesystem::create_directories(d, ec);
    return d;
}

} // namespace

// ── The injected fault, seen through the gate's own logic ───────────────────
//
// Places the REAL report file where ASan put it and requires the gate to
// count it and return FAIL. This is the row's question, answered by running
// the gate rather than by reading it.
TEST(SanitizerGateFaultInjection, ARealHeapUseAfterFreeIsCountedAndFailsTheGate)
{
    const auto report = fixtureDir() / "asan_real_heap_use_after_free.report.txt";
    ASSERT_TRUE(std::filesystem::exists(report))
        << "missing real ASan report fixture at " << report.string();

    // Sanity on the fixture itself: it must be a real finding, or every
    // assertion below would pass against an empty file.
    const std::string reportText = readFile(report);
    EXPECT_NE(reportText.find("AddressSanitizer: heap-use-after-free"), std::string::npos)
        << "fixture is not a heap-use-after-free report";

    const auto dir = freshDir("real_finding");
    std::filesystem::copy_file(report, dir / "asan_ci_finding.14232",
                               std::filesystem::copy_options::overwrite_existing);

    const std::string json = runGate("-LogDir \"" + dir.string() + "\" -Rc 1");

    EXPECT_TRUE(jsonHas(json, "\"verdict\":\"FAIL\""))
        << "a real heap-use-after-free did not produce a FAIL verdict. JSON: " << json;
    EXPECT_TRUE(jsonHas(json, "\"findingsLogFiles\":1"))
        << "the finding was not counted from the log_path file. JSON: " << json;
    EXPECT_TRUE(jsonHas(json, "\"evidenceCollected\":true"))
        << "evidence was present and should not be flagged as missing. JSON: " << json;
}

// ── Defect 1, pinned: the stdout scan is blind under log_path ───────────────
//
// This is the finding, not a design preference. The fixture is the genuine
// captured stdout of the run whose report is asserted above: same process,
// same options, same fault. If a future change to ASAN_OPTIONS makes ASan
// print to stderr again, this test goes red and the comment in the workflow
// that explains the single-source design must be revisited — which is the
// point of pinning it.
TEST(SanitizerGateFaultInjection, TheStdoutScanSeesNothingWhenLogPathIsSet)
{
    const auto stdoutFixture = fixtureDir() / "asan_real_stdout_with_log_path.txt";
    ASSERT_TRUE(std::filesystem::exists(stdoutFixture))
        << "missing real stdout fixture at " << stdoutFixture.string();

    // The captured stdout proves the process ran and faulted...
    const std::string text = readFile(stdoutFixture);
    EXPECT_NE(text.find("SEC21: allocating"), std::string::npos)
        << "fixture is not the stdout of the injected-fault run";
    // ...and yet carries no ASan finding line at all.
    EXPECT_EQ(text.find("==ERROR:"), std::string::npos)
        << "stdout unexpectedly contains an ASan finding line; the workflow's "
           "single-live-source rationale needs revisiting";

    const auto emptyDir = freshDir("stdout_only");
    const std::string json =
        runGate("-StdoutLog \"" + stdoutFixture.string() + "\" -LogDir \"" +
                emptyDir.string() + "\" -Rc 1");

    EXPECT_TRUE(jsonHas(json, "\"findingsStdout\":0"))
        << "stdout scan reported a finding it cannot see. JSON: " << json;
}

// ── Defect 2, pinned: a failing run that collected no evidence is called out ─
//
// rc != 0 with zero findings is exactly what a real finding looks like when
// its report landed in an unscanned directory. Before this phase the gate
// failed silently correct: exit 1, "findings: 0", no artifact. Now it says so.
TEST(SanitizerGateFaultInjection, ANonZeroExitWithNoCollectedEvidenceIsFlagged)
{
    const auto emptyDir = freshDir("evidence_gap");
    const std::string json = runGate("-LogDir \"" + emptyDir.string() + "\" -Rc 1");

    EXPECT_TRUE(jsonHas(json, "\"verdict\":\"FAIL\""))
        << "a non-zero exit must still fail. JSON: " << json;
    EXPECT_TRUE(jsonHas(json, "\"evidenceCollected\":false"))
        << "the missing-evidence condition was not flagged. JSON: " << json;
    EXPECT_NE(json.find("EVIDENCE GAP"), std::string::npos)
        << "no diagnostic explaining where the report may have gone. JSON: " << json;
}

// ── GAP-A15 mirror: a green exit over a red XML still fails ─────────────────
TEST(SanitizerGateFaultInjection, AGreenExitCodeCannotCarryAFailingXml)
{
    const auto dir = freshDir("red_xml");
    const auto xml = dir / "results.xml";
    {
        std::ofstream out(xml, std::ios::binary | std::ios::trunc);
        out << "<?xml version=\"1.0\"?>\n"
               "<testsuites tests=\"2\" failures=\"1\" errors=\"0\">\n"
               "  <testsuite name=\"S\" tests=\"2\" failures=\"1\" errors=\"0\">\n"
               "    <testcase name=\"ok\" classname=\"S\" status=\"run\"/>\n"
               "    <testcase name=\"bad\" classname=\"S\" status=\"run\"><failure/></testcase>\n"
               "  </testsuite>\n"
               "</testsuites>\n";
    }

    const std::string json =
        runGate("-LogDir \"" + dir.string() + "\" -XmlPath \"" + xml.string() + "\" -Rc 0");

    EXPECT_TRUE(jsonHas(json, "\"verdict\":\"FAIL\""))
        << "a failing XML rode a green exit code. JSON: " << json;
    EXPECT_TRUE(jsonHas(json, "\"gtestFailures\":1"))
        << "XML failures were not parsed. JSON: " << json;
}

// ── NON-VACUITY: the gate must be able to say PASS ──────────────────────────
//
// Without this, every assertion above is satisfied by a script that returns
// FAIL unconditionally.
TEST(SanitizerGateFaultInjection, ACleanRunStillPasses)
{
    const auto dir = freshDir("clean");
    const auto xml = dir / "results.xml";
    {
        std::ofstream out(xml, std::ios::binary | std::ios::trunc);
        out << "<?xml version=\"1.0\"?>\n"
               "<testsuites tests=\"1\" failures=\"0\" errors=\"0\">\n"
               "  <testsuite name=\"S\" tests=\"1\" failures=\"0\" errors=\"0\">\n"
               "    <testcase name=\"ok\" classname=\"S\" status=\"run\"/>\n"
               "  </testsuite>\n"
               "</testsuites>\n";
    }

    const std::string json =
        runGate("-LogDir \"" + dir.string() + "\" -XmlPath \"" + xml.string() + "\" -Rc 0");

    EXPECT_TRUE(jsonHas(json, "\"verdict\":\"PASS\""))
        << "a clean run did not pass — the oracle cannot distinguish. JSON: " << json;
    EXPECT_TRUE(jsonHas(json, "\"evidenceCollected\":true")) << "JSON: " << json;
}

// ── WIRING: a correct predicate nothing consults is the commoner failure ────
//
// This project has met that shape three times (the self-disarming CI checkers
// of the WIN-87 phase, the RtmLiveness filename near-miss, and DCA 0028's
// classifier). Assert the workflow actually calls the script whose behaviour
// every test above verifies.
TEST(SanitizerGateFaultInjection, TheSanitizerWorkflowInvokesTheSharedVerdictScript)
{
    const auto wf = repoRoot() / ".github" / "workflows" / "windows-sanitizer.yml";
    ASSERT_TRUE(std::filesystem::exists(wf)) << "missing " << wf.string();
    const std::string text = readFile(wf);

    EXPECT_NE(text.find("asan_gate_verdict.ps1"), std::string::npos)
        << "windows-sanitizer.yml does not invoke tests/scripts/asan_gate_verdict.ps1 — "
           "the verdict logic under test is not the logic CI runs";

    // The multi-directory scan is the fix for defect 2; a revert to a single
    // hard-coded workspace scan must not pass silently.
    EXPECT_NE(text.find("-LogDir @($env:GITHUB_WORKSPACE"), std::string::npos)
        << "the log_path scan is no longer passed multiple candidate directories; "
           "a finding written outside GITHUB_WORKSPACE would be counted as zero";
}

// ── A1-95 standing control: this lane can never carry leak evidence ─────────
//
// Measured, not assumed: running a deliberate leak under detect_leaks=1 with
// the MSVC ASan runtime prints
//   "AddressSanitizer: detect_leaks is not supported on this platform."
// and exits 1 before any user code runs. detect_leaks=0 is therefore the only
// value that lets this lane run at all, and the lane cannot answer a leak
// question. A future edit that "enables leak checking here" would red every
// run; this test explains why before that happens.
// Scanned line by line with COMMENTS EXCLUDED, not as one substring search over
// the whole file. The first version of this guard searched the raw text and went
// red against the very comment that explains why detect_leaks=1 is forbidden —
// the same shape DCA 0028 records, where a sweep matched a substring instead of
// the declaration it meant. A guard that cannot tell a setting from a sentence
// about the setting punishes documenting the rule.
TEST(SanitizerGateFaultInjection, TheSanitizerLaneNeverEnablesUnsupportedLeakDetection)
{
    const auto wf = repoRoot() / ".github" / "workflows" / "windows-sanitizer.yml";
    ASSERT_TRUE(std::filesystem::exists(wf)) << "missing " << wf.string();

    std::ifstream in(wf);
    std::string line;
    int effectiveLines = 0;
    bool pinsZero = false;
    std::string offender;
    int offenderLine = 0;
    int lineNo = 0;

    while (std::getline(in, line)) {
        ++lineNo;
        const auto firstNonSpace = line.find_first_not_of(" \t");
        if (firstNonSpace == std::string::npos) continue;
        if (line[firstNonSpace] == '#') continue;   // a comment, not a setting
        ++effectiveLines;
        if (line.find("detect_leaks=0") != std::string::npos) pinsZero = true;
        if (line.find("detect_leaks=1") != std::string::npos && offender.empty()) {
            offender = line.substr(firstNonSpace);
            offenderLine = lineNo;
        }
    }

    // A sweep that silently reads nothing passes. Assert its reach.
    ASSERT_GT(effectiveLines, 100)
        << "only " << effectiveLines << " non-comment lines were read from "
        << wf.string() << " — the scan did not reach the workflow body";

    EXPECT_TRUE(pinsZero)
        << "the ASan lane no longer pins detect_leaks=0 on any effective line";
    EXPECT_TRUE(offender.empty())
        << "windows-sanitizer.yml:" << offenderLine << " sets detect_leaks=1 ("
        << offender << "). LeakSanitizer is absent from the MSVC ASan runtime: the "
           "process prints 'AddressSanitizer: detect_leaks is not supported on this "
           "platform' and exits 1 before running any test, so this would red every "
           "run of the lane. Leak coverage lives in tests/test_leak_soak_r205_r206.cpp.";
}
