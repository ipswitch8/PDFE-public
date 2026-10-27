// -*- coding: utf-8 -*-
//
// GitHub #87 — TOUCH INPUT IS A NON-GOAL, AND THIS IS THE GUARD THAT KEEPS
// THAT TRUE OR MAKES IT VISIBLY FALSE.
//
// The scope decision lives in docs/touch-input-scope-decision.md: touch and
// gesture handling on PdfCanvas is DEFERRED, deliberately, and nothing is
// implemented. Section 5 of that document explains why prose alone was judged
// insufficient. Restated here because a guard whose reason lives only in
// another file gets deleted by whoever cannot see the reason:
//
//   The failure mode is NOT that someone disagrees with the decision. It is
//   that someone adds a touchEvent() override to PdfCanvas for one narrow
//   purpose, ships it untested, and the decision record goes on asserting that
//   touch is not implemented. The tree and the record then disagree, silently.
//
// So this file does not test behaviour. It re-derives the touch API surface of
// src/ui/PdfCanvas.{h,cpp} from the SOURCE on every run and fails if a touch
// or gesture symbol appears in CODE with nothing in tests/ referring to it.
//
// HOW IT CAN GO RED — a guard that cannot redden is decoration, and one that
// can only redden is equally useless. This one has four red paths and is green
// on today's tree:
//
//   1. touch handling added to PdfCanvas with no reference from tests/    RED
//   2. docs/touch-input-scope-decision.md deleted or renamed             RED
//   3. src/ui/PdfCanvas.cpp deleted or renamed (fail closed)             RED
//   4. the scanner stops seeing source at all (non-vacuity sentinel)     RED
//
// And it stays GREEN when touch is added TOGETHER WITH test coverage: that is
// the decision being reversed on purpose, which is allowed. The guard's job is
// to stop it happening by accident.
//
// THE "AND THERE IS NO TEST COVERAGE" HALF IS NOT ASSUMED. On today's tree
// there is no touch handling, so the coverage branch would never execute and
// could be arbitrarily broken without anyone noticing. TouchScannerSelfTest
// below drives the same scanner and the same predicate over SYNTHETIC inputs
// that do contain touch symbols, in both the covered and the uncovered
// arrangement, so both branches are exercised on every run.
//
// WHAT IT CANNOT DO, said rather than implied: this is a lexical scan, not an
// AST pass. Touch handling reached through a macro, or through a bare
// QObject::event() dispatch that never names a touch type, is invisible to it.
// It is a floor, not a proof — the same caveat test_page_load_guard_sites.cpp
// states for the page-load guard.

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

// The touch / gesture API surface. Presence of any of these IN CODE is what
// "PdfCanvas handles touch" means for the purposes of this guard.
const std::vector<std::string>& touchSymbols() {
    static const std::vector<std::string> kSymbols = {
        "QTouchEvent",
        "QTouchDevice",
        "QPointingDevice",
        "touchEvent",
        "TouchBegin",
        "TouchUpdate",
        "TouchEnd",
        "TouchCancel",
        "TouchPointPressed",
        "WA_AcceptTouchEvents",
        "setAcceptTouchEvents",
        "QGestureEvent",
        "QGestureRecognizer",
        "grabGesture",
        "ungrabGesture",
        "QPinchGesture",
        "QPanGesture",
        "QSwipeGesture",
        "QTapGesture",
        "QTapAndHoldGesture",
    };
    return kSymbols;
}

// Blank out // comments, /* */ comments and "..." / '...' literals, preserving
// line structure so line numbers stay meaningful.
//
// Stripping is LOAD-BEARING here, not hygiene. src/ui/PdfCanvas.cpp says "this
// destructor having to touch a Document" and "the only pan gesture this canvas
// had was the LEFT-button drag". A scan that did not strip comments would
// report the canvas as already implementing touch and this guard would be red
// from the day it was written, which is the same as useless.
std::string stripCppNonCode(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    enum class St { Code, Line, Block, Str, Chr } st = St::Code;
    for (size_t i = 0; i < src.size(); ++i) {
        const char c = src[i];
        const char n = (i + 1 < src.size()) ? src[i + 1] : '\0';
        switch (st) {
        case St::Code:
            if (c == '/' && n == '/') { st = St::Line;  out += "  "; ++i; }
            else if (c == '/' && n == '*') { st = St::Block; out += "  "; ++i; }
            else if (c == '"') { st = St::Str; out += ' '; }
            else if (c == '\'') { st = St::Chr; out += ' '; }
            else out += c;
            break;
        case St::Line:
            if (c == '\n') { st = St::Code; out += c; } else out += ' ';
            break;
        case St::Block:
            if (c == '*' && n == '/') { st = St::Code; out += "  "; ++i; }
            else out += (c == '\n') ? '\n' : ' ';
            break;
        case St::Str:
            if (c == '\\') { out += "  "; ++i; }
            else if (c == '"') { st = St::Code; out += ' '; }
            else out += (c == '\n') ? '\n' : ' ';
            break;
        case St::Chr:
            if (c == '\\') { out += "  "; ++i; }
            else if (c == '\'') { st = St::Code; out += ' '; }
            else out += (c == '\n') ? '\n' : ' ';
            break;
        }
    }
    return out;
}

// Python variant: # to end of line, plus '...' / "..." literals. Triple-quoted
// blocks are handled only as far as the quote characters go, which is enough
// for the "is this a real reference or a docstring mention" question.
std::string stripPythonNonCode(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    enum class St { Code, Comment, Str1, Str2 } st = St::Code;
    for (size_t i = 0; i < src.size(); ++i) {
        const char c = src[i];
        switch (st) {
        case St::Code:
            if (c == '#') { st = St::Comment; out += ' '; }
            else if (c == '\'') { st = St::Str1; out += ' '; }
            else if (c == '"') { st = St::Str2; out += ' '; }
            else out += c;
            break;
        case St::Comment:
            if (c == '\n') { st = St::Code; out += c; } else out += ' ';
            break;
        case St::Str1:
            if (c == '\\') { out += "  "; ++i; }
            else if (c == '\'') { st = St::Code; out += ' '; }
            else out += (c == '\n') ? '\n' : ' ';
            break;
        case St::Str2:
            if (c == '\\') { out += "  "; ++i; }
            else if (c == '"') { st = St::Code; out += ' '; }
            else out += (c == '\n') ? '\n' : ' ';
            break;
        }
    }
    return out;
}

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

// True when `pos` starts a whole identifier rather than the tail of a longer
// one, so "myQTapGestureHelper" is not counted as "QTapGesture".
bool isWholeIdentifier(const std::string& hay, size_t pos, size_t len) {
    if (pos > 0) {
        const char prev = hay[pos - 1];
        if (std::isalnum(static_cast<unsigned char>(prev)) || prev == '_')
            return false;
    }
    const size_t after = pos + len;
    if (after < hay.size()) {
        const char nxt = hay[after];
        if (std::isalnum(static_cast<unsigned char>(nxt)) || nxt == '_')
            return false;
    }
    return true;
}

// THE SCANNER. `code` must already have been stripped. Returns the distinct
// touch symbols present, sorted, so a failure message can name them.
std::set<std::string> touchSymbolsIn(const std::string& code) {
    std::set<std::string> found;
    for (const auto& sym : touchSymbols()) {
        for (size_t pos = code.find(sym); pos != std::string::npos;
             pos = code.find(sym, pos + 1)) {
            if (!isWholeIdentifier(code, pos, sym.size())) continue;
            found.insert(sym);
            break;
        }
    }
    return found;
}

// THE PREDICATE. Separated out so both of its branches can be driven directly
// by the self-test, instead of only the branch today's tree happens to reach.
// Adding touch is not a violation; adding it with nothing in tests/ referring
// to it is.
bool isUncoveredTouchHandling(const std::set<std::string>& inCanvas,
                              const std::set<std::string>& inTests) {
    return !inCanvas.empty() && inTests.empty();
}

std::string join(const std::set<std::string>& s) {
    std::string out;
    for (const auto& v : s) {
        if (!out.empty()) out += ", ";
        out += v;
    }
    return out;
}

fs::path repoRoot() { return fs::path(PDFE_REPO_ROOT); }

// This file, so the guard does not read its own symbol table as either an
// implementation or as coverage.
const char* const kThisFile = "test_touch_input_scope_guard.cpp";

} // namespace

// ---------------------------------------------------------------------------
// The scanner and the predicate, driven over synthetic input. This is what
// makes the "and there is no test coverage" half real rather than assumed:
// on today's tree the canvas has no touch symbols, so without these cases the
// coverage branch would never run.
// ---------------------------------------------------------------------------

TEST(TouchScannerSelfTest, FindsTouchHandlingInCode) {
    const std::string src =
        "void PdfCanvas::touchEvent(QTouchEvent* e) {\n"
        "    setAttribute(Qt::WA_AcceptTouchEvents, true);\n"
        "}\n";
    const auto found = touchSymbolsIn(stripCppNonCode(src));
    EXPECT_FALSE(found.empty())
        << "the scanner did not see a literal touchEvent(QTouchEvent*) "
           "override; it cannot detect the thing this guard exists to detect";
    EXPECT_TRUE(found.count("QTouchEvent") > 0) << join(found);
    EXPECT_TRUE(found.count("touchEvent") > 0) << join(found);
    EXPECT_TRUE(found.count("WA_AcceptTouchEvents") > 0) << join(found);
}

TEST(TouchScannerSelfTest, IgnoresTouchWordsInCommentsAndStrings) {
    const std::string src =
        "// the only pan gesture this canvas had was the LEFT-button drag\n"
        "/* a QTouchEvent would arrive here if we accepted touch */\n"
        "void f() { log(\"QPinchGesture unsupported\"); }\n";
    const auto found = touchSymbolsIn(stripCppNonCode(src));
    EXPECT_TRUE(found.empty())
        << "prose mentioning touch was counted as touch handling: "
        << join(found)
        << " -- src/ui/PdfCanvas.cpp contains exactly this kind of prose, so "
           "the guard would be permanently red";
}

TEST(TouchScannerSelfTest, DoesNotMatchIdentifierSubstrings) {
    const std::string src = "int myQTapGestureHelperCount = 0;\n";
    EXPECT_TRUE(touchSymbolsIn(stripCppNonCode(src)).empty())
        << "a longer identifier containing a touch symbol was counted";
}

TEST(TouchScannerSelfTest, PythonStripperIgnoresCommentsAndStrings) {
    const std::string py =
        "# QTouchEvent is not injectable from pywinauto\n"
        "NOTE = \"QPinchGesture\"\n";
    EXPECT_TRUE(touchSymbolsIn(stripPythonNonCode(py)).empty())
        << "a Python comment or string was counted as touch coverage";

    const std::string real = "def test_touch():\n    send(QTouchEvent)\n";
    EXPECT_FALSE(touchSymbolsIn(stripPythonNonCode(real)).empty())
        << "the Python stripper blanked real code; every .py file would then "
           "read as 'no coverage'";
}

TEST(TouchScannerSelfTest, PredicateFiresOnlyForUncoveredTouchHandling) {
    const std::set<std::string> some = {"QTouchEvent"};
    const std::set<std::string> none = {};

    EXPECT_TRUE(isUncoveredTouchHandling(some, none))
        << "touch in the canvas with nothing in tests/ referring to it must be "
           "a violation; this is the whole guard";
    EXPECT_FALSE(isUncoveredTouchHandling(some, some))
        << "touch added TOGETHER WITH coverage is a deliberate reversal of the "
           "scope decision and must be allowed";
    EXPECT_FALSE(isUncoveredTouchHandling(none, none))
        << "the current tree -- no touch, no touch tests -- must be green";
    EXPECT_FALSE(isUncoveredTouchHandling(none, some))
        << "a touch test with no implementation is someone preparing to "
           "implement, not a violation";
}

// ---------------------------------------------------------------------------
// The guard proper.
// ---------------------------------------------------------------------------

class TouchInputScopeGuard : public ::testing::Test {
protected:
    static std::vector<fs::path> canvasSources() {
        return {
            repoRoot() / "src" / "ui" / "PdfCanvas.h",
            repoRoot() / "src" / "ui" / "PdfCanvas.cpp",
            repoRoot() / "src" / "ui" / "PdfCanvasAccessible.h",
            repoRoot() / "src" / "ui" / "PdfCanvasAccessible.cpp",
        };
    }
};

// Fail closed. A renamed or moved PdfCanvas.cpp must not read as "no touch
// handling found".
TEST_F(TouchInputScopeGuard, CanvasSourcesAreWhereTheGuardLooks) {
    for (const auto& p : canvasSources()) {
        ASSERT_TRUE(fs::exists(p))
            << "cannot find " << p.string()
            << " -- this guard must fail closed, not pass silently. If the "
               "canvas moved, update canvasSources() and "
               "docs/touch-input-scope-decision.md together.";
    }
}

// Non-vacuity. A stripper bug, an unreadable file or a wrong path would make
// the guard green by seeing nothing at all. Prove it is really reading the
// canvas by requiring a symbol that IS in it.
TEST_F(TouchInputScopeGuard, ScannerActuallyReadsTheCanvasSource) {
    const std::string code =
        stripCppNonCode(readAll(repoRoot() / "src" / "ui" / "PdfCanvas.cpp"));
    ASSERT_FALSE(code.empty()) << "PdfCanvas.cpp read as empty";
    EXPECT_NE(code.find("mousePressEvent"), std::string::npos)
        << "the scanner did not find mousePressEvent in PdfCanvas.cpp. It is "
           "reading nothing useful, so 'no touch symbols found' means nothing";
    EXPECT_NE(code.find("wheelEvent"), std::string::npos)
        << "the scanner did not find wheelEvent in PdfCanvas.cpp";
}

// The decision this guard enforces must remain readable, or the next person
// finds a red guard with no stated reason and deletes it.
TEST_F(TouchInputScopeGuard, ScopeDecisionRecordExists) {
    const fs::path doc = repoRoot() / "docs" / "touch-input-scope-decision.md";
    ASSERT_TRUE(fs::exists(doc))
        << "the touch scope decision record is missing from " << doc.string()
        << "; GitHub #87's answer would exist only as this test's behaviour";
    const std::string text = readAll(doc);
    EXPECT_NE(text.find("GitHub #87"), std::string::npos)
        << "the decision record no longer cites the issue it answers";
    EXPECT_NE(text.find("DEFERRED"), std::string::npos)
        << "the decision record no longer states the deferral this guard "
           "enforces; if the decision was reversed, this guard should have "
           "been reconsidered at the same time";
}

// THE GUARD.
TEST_F(TouchInputScopeGuard, TouchHandlingIsAbsentOrElseCoveredByTests) {
    std::set<std::string> inCanvas;
    for (const auto& p : canvasSources()) {
        ASSERT_TRUE(fs::exists(p)) << p.string() << " is missing";
        for (const auto& s : touchSymbolsIn(stripCppNonCode(readAll(p))))
            inCanvas.insert(s);
    }

    // Coverage: any reference from tests/, in code rather than prose, other
    // than this file's own symbol table.
    std::set<std::string> inTests;
    std::vector<std::string> coveringFiles;
    const fs::path testsRoot = repoRoot() / "tests";
    ASSERT_TRUE(fs::exists(testsRoot)) << testsRoot.string() << " is missing";
    for (const auto& e : fs::recursive_directory_iterator(testsRoot)) {
        if (!e.is_regular_file()) continue;
        if (e.path().filename().string() == kThisFile) continue;
        const auto ext = e.path().extension().string();

        std::string code;
        if (ext == ".cpp" || ext == ".h" || ext == ".hpp" || ext == ".cc")
            code = stripCppNonCode(readAll(e.path()));
        else if (ext == ".py")
            code = stripPythonNonCode(readAll(e.path()));
        else
            continue;

        const auto found = touchSymbolsIn(code);
        if (found.empty()) continue;
        coveringFiles.push_back(
            fs::relative(e.path(), repoRoot()).generic_string());
        for (const auto& s : found) inTests.insert(s);
    }

    EXPECT_FALSE(isUncoveredTouchHandling(inCanvas, inTests))
        << "PdfCanvas now handles touch or gesture input -- the symbols found "
           "in code are: "
        << join(inCanvas)
        << " -- but NOTHING under tests/ refers to any touch or gesture "
           "symbol, so none of it is exercised.\n"
           "docs/touch-input-scope-decision.md records touch as a DEFERRED "
           "non-goal (GitHub #87). Adding it is allowed, but not silently: "
           "add a test that drives the new handling, and update that document "
           "so the record and the tree agree.";

    // The other direction, so the state of the tree is stated rather than
    // merely tolerated. Today there is no touch handling; if that changes,
    // this reminds whoever changed it that the record is now stale.
    if (!inCanvas.empty()) {
        const std::string doc =
            readAll(repoRoot() / "docs" / "touch-input-scope-decision.md");
        EXPECT_EQ(doc.find("**Status:** DEFERRED"), std::string::npos)
            << "PdfCanvas handles touch (" << join(inCanvas)
            << ") and it is covered by " << coveringFiles.size()
            << " test file(s), but docs/touch-input-scope-decision.md still "
               "says the decision is DEFERRED. Update the record: the decision "
               "has been reversed.";
    }
}
