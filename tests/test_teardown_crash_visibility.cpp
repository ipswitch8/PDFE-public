// -*- coding: utf-8 -*-
//
// DCA 0028 — a crash that arrives after the last assertion.
//
// A shard ran all 356 of its tests, passed them, wrote a complete and green
// JUnit XML, and then died with 0xC0000005. `run_gtest.py` printed
// `rc=3221225477 -> ok` and the aggregate said `CLEAN across all shards`.
//
// Two independent things had to be true for that to happen, and this file
// holds one guard for each:
//
//   1. THE CONTROL DID NOT EXIST. `_is_crashed()` inspected only the XML, so a
//      process that produced a perfect report and then faulted did not match
//      any crash condition and its exit code was printed and discarded.
//      GAP-A15 was an exit code of ZERO hiding failures in the XML; this is a
//      NON-ZERO exit code hidden BY the XML — the same blind spot, mirrored,
//      and still open after A15 was closed.
//
//   2. THE DEFECT WAS REACHABLE. A test cached MuPDF objects in a
//      function-local static, so they were destroyed during static
//      destruction — after main() returned, after QApplication was gone, and
//      after MuPDF's own teardown.
//
// The first guard is the INSTANCE: it drives the runner's classifier and
// requires it to call a structured-exception exit abnormal. The second is
// CLASS-LEVEL: it sweeps every test translation unit for the shape that
// created the hazard, so the next one is red before it ever runs.

#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::filesystem::path repoRoot()
{
    return std::filesystem::path(PDFE_REPO_ROOT);
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
    std::array<char, 2048> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe)) {
        out += buf.data();
    }
#if defined(_WIN32)
    return ::_pclose(pipe);
#else
    return ::pclose(pipe);
#endif
}

} // namespace

// ── INSTANCE: the runner must call a crash exit a crash ────────────────────
//
// Driven, not grepped. The classifier is imported and evaluated on the exact
// value that was mis-reported (3221225477 == 0xC0000005) plus gtest's two
// legitimate exits, so a future edit that widens or narrows it goes red here
// rather than in a suite run six months from now.
TEST(TeardownCrashVisibility, TheRunnerClassifiesACrashExitAsAbnormal)
{
    const std::string python = PDFE_PYTHON_EXECUTABLE;
    ASSERT_FALSE(python.empty())
        << "no Python 3 interpreter at configure time; the runner's crash "
           "classifier cannot be exercised, and a build that cannot check its "
           "own harness must not read as one that checked it";

    const auto runner = repoRoot() / "tests" / "scripts" / "run_gtest.py";
    ASSERT_TRUE(std::filesystem::exists(runner)) << runner.string();

    // importlib against the file's real path, so this cannot accidentally
    // pass against some other module that happens to be importable.
    //
    // Written to a SCRIPT FILE rather than passed with -c. A -c payload has to
    // survive cmd.exe's quoting on top of C++'s, and the interpreter path
    // contains a space ("C:\\Program Files\\..."), which is exactly the
    // combination that silently produced
    //   'C:/Program' is not recognized as an internal or external command
    // on the first run of this test. A quoting bug here reads as a failing
    // control, which is the most expensive kind of false alarm.
    const auto scriptPath =
        std::filesystem::temp_directory_path() / "pdfe_dca0028_probe.py";
    {
        std::ofstream f(scriptPath, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(f.good()) << scriptPath.string();
        f << "# -*- coding: utf-8 -*-\n"
          << "import importlib.util\n"
          << "spec = importlib.util.spec_from_file_location('rg', r\"\"\""
          << runner.string() << "\"\"\")\n"
          << "m = importlib.util.module_from_spec(spec)\n"
          << "spec.loader.exec_module(m)\n"
          << "print('CRASH=%d' % m._is_abnormal_exit(3221225477))\n"
          << "print('SEGV=%d' % m._is_abnormal_exit(139))\n"
          << "print('OK=%d' % m._is_abnormal_exit(0))\n"
          << "print('FAIL=%d' % m._is_abnormal_exit(1))\n";
    }

    std::string out;
    // One further pair of quotes wraps the whole command: cmd.exe strips the
    // outermost pair, leaving both quoted paths intact.
    const std::string cmd =
        "\"\"" + python + "\" \"" + scriptPath.string() + "\"\" 2>&1";
    const int rc = runCapturing(cmd, out);
    ASSERT_EQ(rc, 0) << "could not evaluate the classifier:\n" << out;

    EXPECT_NE(out.find("CRASH=1"), std::string::npos)
        << "0xC0000005 (3221225477) is not classified abnormal — a shard that "
           "passes every test and then faults will read as 'ok' again.\n" << out;
    EXPECT_NE(out.find("SEGV=1"), std::string::npos)
        << "a POSIX signal exit is not classified abnormal.\n" << out;

    // The other half: gtest's own exits must stay normal, or every ordinary
    // failing run would be mislabelled a crash and the signal would be worth
    // nothing.
    EXPECT_NE(out.find("OK=0"), std::string::npos) << out;
    EXPECT_NE(out.find("FAIL=0"), std::string::npos)
        << "exit 1 is gtest's ordinary failure code and must NOT be reported "
           "as a crash.\n" << out;
}

// The classifier existing is not enough — it has to be WIRED, and its result
// has to reach the return value. A run whose tests all pass and whose process
// crashes must not return 0.
TEST(TeardownCrashVisibility, ATeardownCrashMakesTheRunNotClean)
{
    const std::string runner =
        readFile(repoRoot() / "tests" / "scripts" / "run_gtest.py");
    ASSERT_FALSE(runner.empty());

    EXPECT_NE(runner.find("_is_abnormal_exit"), std::string::npos);
    EXPECT_NE(runner.find("teardown_crashed_shards"), std::string::npos)
        << "the classifier is defined but nothing collects its result";

    // The CLEAN path must be guarded by it. Without this, a run with zero
    // failures returns 0 and prints CLEAN over the top of a crash.
    const std::regex cleanGuard(
        R"(if\s+teardown_crashed_shards:\s*\n\s*return\s+1)");
    EXPECT_TRUE(std::regex_search(runner, cleanGuard))
        << "the no-failures path does not return non-zero when a teardown "
           "crash was recorded, so `CLEAN` can still be printed over one";
}

// ── CLASS-LEVEL: the shape that made the crash reachable ───────────────────
//
// The instance was one test caching a MuPdfContext/Document/Page in a
// function-local static. The CLASS is any test translation unit parking an
// object that owns an engine or GUI resource in a static, where its destructor
// runs after main() returns.
//
// This is a source sweep rather than a runtime check because the failure it
// prevents happens at a point no runtime check is still alive to observe —
// which is the whole lesson of this DCA.
TEST(TeardownCrashVisibility, NoTestParksAnEngineOwningObjectInAStatic)
{
    const auto testsDir = repoRoot() / "tests";
    ASSERT_TRUE(std::filesystem::exists(testsDir)) << testsDir.string();

    // Matched as a DECLARATION, not as a substring.
    //
    // The first version of this sweep searched for the type names anywhere on
    // a line containing "static", and flagged `static_cast<int>(kPageH)` —
    // "static" plus "Page" inside an unrelated identifier — in four files. A
    // guard with false positives is a guard somebody switches off, so the
    // pattern now requires `static` as a whole word at the start of a
    // declaration, and the type as a whole word followed by a
    // pointer/reference/template-close/identifier character.
    static const std::regex kOwningStatic(
        R"(^\s*static\b[^;()]*\b)"
        R"((Document|Page|AnnotationManager|FormManager|SignatureManager|)"
        R"(RedactionManager|ImageManager|QApplication|QDialog|QMainWindow)\b)"
        R"(\s*[>*&\s])");

    // KNOWN, pre-existing, and NOT fixed by this phase — recorded rather than
    // allowlisted silently, which is this project's "N KNOWN, not all"
    // convention (see WIN-87's enumeration in the phase-60 and phase-61
    // dispositions).
    //
    // Six signature-suite fixtures hold `static std::shared_ptr<MuPdfContext>`
    // class members. That is the SAME SHAPE as the defect this record is
    // about, and it is the reason `MuPdfContext` is not in the pattern above:
    // adding it would make this guard red on arrival against six files this
    // phase did not touch and cannot verify, and a control that is red on
    // arrival gets weakened rather than obeyed.
    //
    // They have not crashed, and the difference is instructive: a bare
    // context's destructor is self-contained, whereas the defect here held a
    // context AND a Document AND a Page AND two managers, whose destruction
    // ORDER is what faulted. That makes them lower risk, not safe. Converting
    // them to per-test construction is tracked work, not a reason to narrow
    // the record.
    //
    // Three more hold `static QApplication app(argc, argv);` inside a lazily
    // initialised helper. Those predate `test_main.cpp` owning a single
    // process-wide QApplication and are dead in practice — but they are the
    // same post-main() destruction shape and are listed rather than pattern-
    // excluded, so removing them stays visible work instead of becoming
    // invisible permission.
    static constexpr const char* kKnownPreExisting[] = {
        "test_image_properties_panel.cpp",
        "test_image_tool.cpp",
        "test_metadata_dialog.cpp",
        "test_dss_save_fault_injection.cpp",
        "test_ltv_dss_gc_survival.cpp",
        "test_ltv_dss_writer.cpp",
        "test_ltv_validator.cpp",
        "test_ocsp_crl_parser.cpp",
        "test_validation_request.cpp",
    };

    std::vector<std::string> offenders;
    int filesScanned = 0;

    std::error_code ec;
    for (const auto& de :
         std::filesystem::recursive_directory_iterator(testsDir, ec)) {
        if (ec) break;
        if (!de.is_regular_file(ec)) continue;
        const auto& p = de.path();
        if (p.extension() != ".cpp" && p.extension() != ".h") continue;
        // This file names the types in prose; excluding it keeps the guard
        // from flagging its own documentation.
        if (p.filename() == "test_teardown_crash_visibility.cpp") continue;
        ++filesScanned;

        const std::string leaf = p.filename().string();
        bool known = false;
        for (const char* k : kKnownPreExisting) {
            if (leaf == k) { known = true; break; }
        }
        if (known) continue;

        const std::string src = readFile(p);
        std::istringstream lines(src);
        std::string line;
        int lineNo = 0;
        while (std::getline(lines, line)) {
            ++lineNo;
            const auto firstNonWs = line.find_first_not_of(" \t");
            if (firstNonWs == std::string::npos) continue;
            if (line.compare(firstNonWs, 2, "//") == 0) continue;
            if (!std::regex_search(line, kOwningStatic)) continue;
            // A static FUNCTION returning an owning type is not a static
            // OBJECT — `static std::unique_ptr<Document> openDoc(` declares no
            // storage and has no destructor. Matching it would be the same
            // substring sloppiness the pattern above was tightened to avoid.
            if (std::regex_search(line, std::regex(R"(\w\s*\()"))) continue;
            offenders.push_back(
                std::filesystem::relative(p, repoRoot(), ec).string() +
                ":" + std::to_string(lineNo) + "  " + line.substr(firstNonWs));
        }
    }

    // The known set must still BE what it says it is. If one of those files
    // stops holding the shape, it should leave the list rather than sit there
    // granting an exemption nobody needs — an allowlist that outlives its
    // reason is how a guard quietly stops guarding.
    for (const char* k : kKnownPreExisting) {
        const auto path = testsDir / k;
        if (!std::filesystem::exists(path)) continue;
        const std::string src = readFile(path);
        const bool stillHasTheShape =
            src.find("static std::shared_ptr<MuPdfContext>") != std::string::npos ||
            src.find("static QApplication") != std::string::npos;
        EXPECT_TRUE(stillHasTheShape)
            << k << " is on the known-pre-existing list but no longer holds a "
                 "static engine- or GUI-owning object. Remove it from "
                 "kKnownPreExisting — an exemption that outlives its reason is "
                 "how a guard quietly stops guarding.";
    }

    EXPECT_GT(filesScanned, 100)
        << "the sweep saw only " << filesScanned << " test files; it cannot "
           "audit a tree it cannot see";

    std::string joined;
    for (const auto& o : offenders) joined += "  " + o + "\n";
    EXPECT_TRUE(offenders.empty())
        << offenders.size() << " test declaration(s) park an engine- or "
           "GUI-owning object in a static. Its destructor runs AFTER main() "
           "returns — after QApplication is gone and after MuPDF has torn "
           "down — so the process faults on the way out with every test "
           "passing and the XML green:\n"
        << joined
        << "\nBuild these per-test instead. One extra document open is not "
           "worth a lifetime hazard. See docs/audit/dca/"
           "0028-a-crash-that-arrives-after-the-last-assertion.md";
}
