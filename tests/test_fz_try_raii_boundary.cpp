// -*- coding: utf-8 -*-
//
// Phase-15 (follow-up phase-68) — the fz_try / RAII boundary sweep.
//
// WHAT THIS GUARDS
// ----------------
// MuPDF's fz_try/fz_always/fz_catch is setjmp/longjmp. A longjmp out of a
// guarded region does not run destructors for locals declared inside it, and a
// C++ `throw` raised inside one unwinds through MuPDF's C frames, which are not
// unwind-aware. Both are silent: the leak has no observable effect on any
// assertion this suite has, and the corrupted MuPDF error stack only shows up
// later, somewhere else.
//
// A sweep for that shape had been attempted twice with text matching. Both
// undercounted, and the reason is structural rather than careless:
//
//   * `fz_try` occurs 121 times in the tree; 49 of those are in comments and 9
//     more are prose in headers.
//   * The LARGER population contains no `fz_try` token at all — every
//     MuPdfContext::execute / executeReturn lambda body runs inside an fz_try
//     that lives in MuPdfContext.
//   * "Does this local have a non-trivial destructor?" is a question about the
//     whole type graph. `std::string s;` and `fz_rect r;` are the same string
//     of characters to a regex.
//
// tools/gate/fz_try_raii_scan.py asks the compiler instead: regions come from
// clang's preprocessing record and AST, and destructor triviality from the C++
// front end's own __is_trivially_destructible. It finds 102 hazards where two
// brace-matching passes found 36.
//
// WHY EACH CASE EXISTS
// --------------------
// A guard that reports "0 hazards" is indistinguishable from a guard that
// parsed nothing, so the scanner's own self-test — which includes a fixture
// with known hazards it must FIND — is a required case here, not an optional
// extra. Without it, every other case in this file could pass while measuring
// nothing at all.

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

fs::path scanner()
{
    return repoRoot() / "tools" / "gate" / "fz_try_raii_scan.py";
}

fs::path baselinePath()
{
    return repoRoot() / "tools" / "gate" / "fz_try_raii_baseline.json";
}

fs::path workflow()
{
    return repoRoot() / ".github" / "workflows" / "windows-validation.yml";
}

// The files this phase repaired. Every one of them still contains guarded
// regions, so "no hazard here" is a statement about the repairs, not about the
// scanner having stopped looking.
const std::vector<std::string>& repairedFiles()
{
    static const std::vector<std::string> v = {
        "src/annotations/AnnotationManager.cpp",
        "src/attachments/AttachmentManager.cpp",
        "src/image/ExifStripper.cpp",
        "src/core/PageOperations.cpp",
        "src/core/Document.cpp",
        "src/redaction/RedactionScrubber.cpp",
    };
    return v;
}

struct ScanOutput {
    int rc = -1;
    std::string text;
    int hazards = -1;
    int regions = -1;
    int translationUnits = -1;
};

int fieldAfter(const std::string& text, const std::string& label)
{
    const auto pos = text.find(label);
    if (pos == std::string::npos) return -1;
    const auto colon = text.find(':', pos);
    if (colon == std::string::npos) return -1;
    return std::atoi(text.c_str() + static_cast<long>(colon) + 1);
}

// One scan, shared by every case that needs it: each invocation re-parses the
// translation unit with all of Qt in scope, so running it per-test would cost
// minutes for no extra coverage.
const ScanOutput& repairedFileScan()
{
    static ScanOutput out = [] {
        ScanOutput o;
        const std::string python = PDFE_PYTHON_EXECUTABLE;
        if (python.empty()) return o;
        std::string cmd = "set PYTHONIOENCODING=utf-8 && " + quoted(python) +
                          " " + quoted(scanner().string());
        for (const auto& f : repairedFiles()) {
            cmd += " --file " + quoted((repoRoot() / f).string());
        }
        cmd += " 2>&1";
        o.rc = runCapturing(cmd, o.text);
        o.hazards = fieldAfter(o.text, "hazards ");
        o.regions = fieldAfter(o.text, "guarded regions ");
        o.translationUnits = fieldAfter(o.text, "translation units ");
        return o;
    }();
    return out;
}

bool mentions(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

// True when `needle` appears on a line that is not a YAML comment. The F48
// workflow guard was fooled once by its own explanatory comment.
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
// The scanner's own contract. If this fails, every other case below is
// measuring nothing and their PASS means nothing.
TEST(FzTryRaiiBoundary, TheScannerSatisfiesItsOwnContract)
{
    const std::string python = PDFE_PYTHON_EXECUTABLE;
    ASSERT_FALSE(python.empty())
        << "no Python 3 interpreter at configure time, so the fz_try/RAII "
           "scanner cannot be verified. A hard failure, not a skip.";
    ASSERT_TRUE(fs::exists(scanner())) << scanner().string() << " is missing";

    const std::string cmd = "set PYTHONIOENCODING=utf-8 && " + quoted(python) +
                            " " + quoted(scanner().string()) +
                            " --self-test 2>&1";
    std::string out;
    const int rc = runCapturing(cmd, out);
    EXPECT_EQ(rc, 0) << "fz_try_raii_scan.py --self-test failed:\n\n" << out;

    const auto pos = out.find("self-test: ");
    ASSERT_NE(pos, std::string::npos)
        << "no check count printed; the self-test exited early.\n\n" << out;
    EXPECT_GE(std::atoi(out.c_str() + static_cast<long>(pos) + 11), 33)
        << "the self-test shrank. It must still cover: std::string, "
           "std::vector, std::unique_ptr and a struct whose only non-trivial "
           "part is a member; a pointer to a type with a virtual destructor "
           "staying TRIVIAL; the commented-out fz_try NOT counting; an "
           "execute() on an unrelated class NOT counting; and an unnameable "
           "type reporting UNKNOWN rather than trivial.\n\n"
        << out;
}

// ─────────────────────────────────────────────────────────────────────────────
// Criterion 3's regression test. The finding said RedactionScrubber.cpp:605 was
// "a return inside the 133-line lambda body that skips fz_always/fz_catch".
// It is not a lambda — it is a literal fz_try — and its five owning locals are
// already declared BEFORE it, deliberately and with a comment saying so. The
// returns inside the block belong to inner lambdas passed to pruneNameTree and
// return normally to their caller; none crosses the boundary.
//
// So the defect to guard against is not a defect that exists: it is the
// possibility of someone moving one of those five declarations back inside the
// fz_try. That is what this pins, and moving any one of them reddens it.
TEST(FzTryRaiiBoundary, ScrubDocumentScopedSurfacesKeepsItsLocalsOutsideFzTry)
{
    const ScanOutput& scan = repairedFileScan();
    ASSERT_NE(scan.translationUnits, -1)
        << "the scan produced no summary at all:\n\n" << scan.text;
    ASSERT_GT(scan.regions, 0)
        << "the scan found no guarded regions, so finding no hazards proves "
           "nothing:\n\n" << scan.text;

    EXPECT_FALSE(mentions(scan.text, "redactionscrubber.cpp:"))
        << "a boundary hazard reappeared in RedactionScrubber.cpp. The five "
           "owning locals of scrubDocumentScopedSurfaces (seen, scratch, "
           "scratch2, doomedKeys, outlineScratch) must stay declared BEFORE "
           "fz_try:\n\n"
        << scan.text;
}

// ─────────────────────────────────────────────────────────────────────────────
// The repairs themselves. Named locals rather than a bare count, so that a
// regression says which one came back.
TEST(FzTryRaiiBoundary, TheRepairedLocalsStayOutsideTheirGuardedRegions)
{
    const ScanOutput& scan = repairedFileScan();
    ASSERT_GT(scan.regions, 0) << scan.text;

    const std::vector<std::string> repaired = {
        "fileName", "safe", "target", "ofs",      // AnnotationManager
        "baseName", "outPath", "out",             // AttachmentManager
        "qImage", "cleanQImage",                  // ExifStripper
        "pageList",                               // Document::rearrangePages
    };
    for (const auto& name : repaired) {
        // The listing prints "<type> <name>  [in <region> ...]", so a bare
        // substring search would match a longer name; anchor on the bracket.
        EXPECT_FALSE(mentions(scan.text, " " + name + "  [in "))
            << "'" << name << "' is declared inside a guarded region again. "
               "A longjmp will not run its destructor.\n\n"
            << scan.text;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// The forbidden pattern, separately: a C++ throw inside an execute() lambda.
// This one is not a leak — it leaves MuPDF's error frame pushed, and the
// damage surfaces in an unrelated later operation.
TEST(FzTryRaiiBoundary, NoRepairedFileThrowsACxxExceptionAcrossTheBoundary)
{
    const ScanOutput& scan = repairedFileScan();
    ASSERT_GT(scan.regions, 0) << scan.text;

    EXPECT_FALSE(mentions(scan.text, "cxx_throw"))
        << "a C++ throw crosses a setjmp/longjmp boundary again. Use "
           "fz_throw(ctx, ...) inside execute()/executeReturn() lambdas and "
           "inside fz_try blocks — see the project CLAUDE.md gotcha.\n\n"
        << scan.text;
}

// ─────────────────────────────────────────────────────────────────────────────
// The baseline exists and still describes a population, not an empty set. A
// baseline that had silently become `{"accepted": []}` would make the gate
// pass on any tree.
TEST(FzTryRaiiBoundary, TheCommittedBaselineStillDescribesAPopulation)
{
    ASSERT_TRUE(fs::exists(baselinePath()))
        << baselinePath().string() << " is missing; the CI gate has nothing "
           "to compare against and would accept any new hazard.";
    const std::string text = readFile(baselinePath());
    ASSERT_FALSE(text.empty());

    // Count entries by their most distinctive key rather than parsing JSON.
    size_t entries = 0;
    for (size_t p = text.find("\"hazard\""); p != std::string::npos;
         p = text.find("\"hazard\"", p + 1)) {
        ++entries;
    }
    EXPECT_GE(entries, 50u)
        << "the accepted baseline collapsed to " << entries << " entries. It "
           "records the hazards this phase did NOT repair; if it empties, the "
           "gate stops distinguishing 'already known' from 'newly introduced' "
           "and starts passing everything.";

    // The repaired sites must NOT be in the accepted list, or the gate would
    // tolerate their return.
    for (const auto& name : {"pageList", "cleanQImage", "cxx_throw"}) {
        EXPECT_FALSE(mentions(text, std::string("\"") + name + "\""))
            << "'" << name << "' is still listed as accepted in the baseline, "
               "so reintroducing it would not fail the gate.";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// A one-off scan is not a control. This is the difference between "we looked
// once" and "new hazards are caught".
TEST(FzTryRaiiBoundary, TheScanIsWiredIntoContinuousIntegration)
{
    ASSERT_TRUE(fs::exists(workflow())) << workflow().string() << " is missing";
    const std::string wf = readFile(workflow());
    ASSERT_FALSE(wf.empty());

    EXPECT_TRUE(inCodeLine(wf, "fz_try_raii_scan.py"))
        << "windows-validation.yml does not invoke the scanner on a code line. "
           "A scanner that only ever ran by hand during the phase that wrote "
           "it catches nothing afterwards.";
    EXPECT_TRUE(inCodeLine(wf, "fz_try_raii_baseline.json"))
        << "the CI invocation does not pass --baseline, so it would report "
           "hazards without failing on new ones.";
}

// ─────────────────────────────────────────────────────────────────────────────
// GitHub #196 — the INTERPROCEDURAL case, which the scanner above cannot see.
//
// WHY THIS IS A SOURCE-SHAPE GUARD AND NOT A HEAP MEASUREMENT, said plainly so
// nobody reads more into it than it proves.
//
// The defect: reconcileStructTreeAfterRearrange() and
// pruneAcroFormAfterRearrange() are plain static functions, so neither body is
// itself a guarded region -- but both are called from inside the fz_try that
// lives in MuPdfContext::execute, one frame down. Each used to declare a local
// `std::set<int>`, and a fz_throw from the pdf_count_pages / pdf_lookup_page_obj
// calls they make WHILE that set is live longjmps past its destructor.
//
// fz_try_raii_scan.py cannot report this, by design and by its own admission:
// "A call to a function that itself constructs RAII objects and can throw is
// not reported: that is interprocedural and undecidable from one translation
// unit." Its region model is (a) real fz_try macro expansions and (b) execute /
// executeReturn lambda bodies. These two functions are neither. So the scan
// stays green across the whole defect and the whole fix, which is why this case
// exists separately rather than as another entry in the baseline test above.
//
// The fix is ownership: the sets are constructed in Document::rearrangePages'
// frame, ABOVE the setjmp, and passed in by reference. Ordinary C++ unwinding
// then frees them -- execute() rethrows the longjmp as a C++ MuPdfException
// once it is back outside the guarded region.
//
// What this test can and cannot do. A runtime oracle would need a fz_throw
// forced from inside those functions on a real document, which nothing in the
// tree can arrange without adding a shipped test seam. What it CAN do is pin
// the shape the fix consists of, mechanically, such that reverting the fix --
// moving either declaration back into the callee -- reddens it. That is a
// weaker claim than "no bytes leaked", and it is stated as such.
TEST(FzTryRaiiBoundary, GitHub196ScratchSetsAreOwnedAboveTheSetjmp)
{
    const fs::path src = repoRoot() / "src" / "core" / "Document.cpp";
    ASSERT_TRUE(fs::exists(src)) << src.string() << " is missing";
    std::string text = readFile(src);
    ASSERT_FALSE(text.empty()) << "could not read " << src.string();

    // Strip // comments. The explanatory comments around this fix name the very
    // types being searched for, and a guard that its own rationale can satisfy
    // is not a guard.
    std::string code;
    code.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            while (i < text.size() && text[i] != '\n') ++i;
        } else {
            code.push_back(text[i++]);
        }
    }

    // The body of `name`, by brace matching from its signature.
    const auto bodyOf = [&](const std::string& name) -> std::string {
        const std::size_t sig = code.find("static void " + name + "(");
        if (sig == std::string::npos) return {};
        const std::size_t open = code.find('{', sig);
        if (open == std::string::npos) return {};
        int depth = 0;
        for (std::size_t i = open; i < code.size(); ++i) {
            if (code[i] == '{') ++depth;
            else if (code[i] == '}' && --depth == 0)
                return code.substr(open, i - open + 1);
        }
        return {};
    };
    const auto signatureOf = [&](const std::string& name) -> std::string {
        const std::size_t sig = code.find("static void " + name + "(");
        if (sig == std::string::npos) return {};
        const std::size_t open = code.find('{', sig);
        if (open == std::string::npos) return {};
        return code.substr(sig, open - sig);
    };

    struct Callee {
        const char* fn;
        const char* param;   // the caller-owned scratch it must receive
        const char* proves;  // a call that can fz_throw while the set is live
    };
    const Callee callees[] = {
        {"reconcileStructTreeAfterRearrange", "livePageNums", "pdf_lookup_page_obj"},
        {"pruneAcroFormAfterRearrange",       "liveAnnots",   "pdf_count_pages"},
    };

    for (const auto& c : callees) {
        SCOPED_TRACE(c.fn);
        const std::string body = bodyOf(c.fn);
        const std::string sig = signatureOf(c.fn);

        // Non-vacuity first: an empty body would make every EXPECT below pass
        // for the wrong reason (the function renamed, the scan finding nothing).
        ASSERT_FALSE(body.empty())
            << c.fn << " was not found in Document.cpp, so nothing was checked. "
               "If it was renamed, rename it here too rather than deleting the "
               "case.";
        ASSERT_NE(body.find(c.proves), std::string::npos)
            << c.fn << " no longer calls " << c.proves
            << ", so it may no longer be able to fz_throw while the scratch set "
               "is live -- re-derive whether this guard still describes a real "
               "hazard before weakening it.";

        // The fix: the scratch arrives by reference from the caller's frame.
        EXPECT_NE(sig.find("std::set<int>& "), std::string::npos)
            << c.fn << " does not take its scratch set by reference:\n"
            << sig
            << "\nIt is called from inside MuPdfContext::execute's fz_try, so a "
               "set it owns itself leaks on the longjmp path (GitHub #196).";

        // And no non-trivially-destructible local is declared in the body.
        for (const char* owning : {"std::set<", "std::vector<", "std::map<",
                                   "std::string ", "std::unique_ptr<"}) {
            EXPECT_EQ(body.find(owning), std::string::npos)
                << c.fn << " declares a `" << owning
                << "` local. Its destructor does not run when MuPDF longjmps "
                   "out of the guarded region that calls this function. Hoist "
                   "it into Document::rearrangePages and pass it by reference, "
                   "as livePageNums/liveAnnots are.";
        }
    }

    // The other half: the owner really is above the setjmp. Both declarations
    // must appear in rearrangePages BEFORE the execute() that guards the calls,
    // not at the top of the lambda -- only a frame that exists before the
    // setjmp is safe.
    const std::size_t fn = code.find("void Document::rearrangePages(");
    ASSERT_NE(fn, std::string::npos) << "Document::rearrangePages not found";
    const std::size_t generalPath =
        code.find("pdf_rearrange_pages(ctx, doc", fn);
    ASSERT_NE(generalPath, std::string::npos)
        << "the general (pdf_rearrange_pages) path was not found after "
           "rearrangePages, so the ordering check below has no anchor";
    // The execute() whose lambda contains the general path.
    const std::size_t guard = code.rfind("context->execute(", generalPath);
    ASSERT_NE(guard, std::string::npos);
    ASSERT_GT(guard, fn);

    for (const char* decl : {"std::set<int> livePageNums;",
                             "std::set<int> liveAnnots;"}) {
        const std::size_t at = code.find(decl, fn);
        ASSERT_NE(at, std::string::npos)
            << "Document::rearrangePages no longer declares `" << decl
            << "`; the scratch has moved back inside the guarded region "
               "(GitHub #196).";
        EXPECT_LT(at, guard)
            << "`" << decl << "` is declared AFTER the execute() that guards "
               "the pruning calls. It must be in the caller's frame, before the "
               "setjmp -- inside the lambda is inside the fz_try.";
    }
}
