// -*- coding: utf-8 -*-
//
// PDF-85 / GitHub #32 and #159 — CLOSE THE PAGE-LOAD GUARD AS A CLASS.
//
// WHY THIS FILE EXISTS, STATED PLAINLY
// ------------------------------------
// The first version of the #159 fix guarded `Document::page()` and
// `Document::createDisplayList()`. It passed both phase gates and was reported
// as complete. It was not: `src/` had THIRTEEN places that load a page by
// index, and eleven were still fatal — including `Document::pageInfo()`, which
// is reached by simply OPENING a document (PagePanel builds a thumbnail per
// page). The original threat model — open a hostile file, the process dies,
// no interaction beyond opening — was still live behind a green suite.
//
// `NestedFormXObjectDepthBounded` could not have caught it, because it
// exercised the two entry points that WERE guarded. That is the recorded
// lesson that fixing one instance of a pattern is not fixing the pattern, and
// a per-instance test can only ever find per-instance regressions.
//
// So this file does not test behaviour at all. It re-derives the CALL-SITE
// LIST from the source on every run and fails if any page-load call site is
// not the guarded one. A new `fz_load_page(` anywhere in `src/` reddens here
// even if every behavioural test still passes.
//
// WHAT IT CANNOT DO, said rather than implied. This is a lexical scan, not an
// AST pass — the documented preference in this project is to count call sites
// with an AST, and no C++ AST tool is wired into this suite. It strips line
// and block comments and string literals before matching, which covers the
// realistic ways a mention is not a call, but a call assembled through a macro
// or a function pointer would be invisible to it. It is a floor, not a proof,
// and the behavioural oracle in test_malicious_pdf_advanced.cpp
// (`NestedFormXObjectDepthBounded`, which now exercises pageInfo as well) is
// what checks the guard actually works.

#include <gtest/gtest.h>

#include "core/FormXObjectDepthGuard.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

struct Hit {
    std::string file;   // repo-relative, forward slashes
    int line;
    std::string symbol;
};

// Blank out // comments, /* */ comments and "..." literals, preserving line
// structure so line numbers stay meaningful. Crude by design: it only has to
// be right about whether a call-looking token is code.
std::string stripNonCode(const std::string& src)
{
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

std::string readAll(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

// True when `pos` in `hay` starts a genuine call to `sym`, i.e. the character
// before it is not part of an identifier. Without this, `safeLoadPage(` would
// be counted as a call to `LoadPage(` by a naive substring search, and, worse,
// the guarded loaders would be reported as the very violations they fix.
bool isWholeCall(const std::string& hay, size_t pos, const std::string& sym)
{
    if (pos > 0) {
        const char prev = hay[pos - 1];
        if (std::isalnum(static_cast<unsigned char>(prev)) || prev == '_')
            return false;
    }
    return true;
}

std::vector<Hit> scan(const std::vector<std::string>& symbols)
{
    std::vector<Hit> hits;
    const fs::path root = fs::path(PDFE_REPO_ROOT) / "src";
    if (!fs::exists(root)) return hits;

    for (const auto& e : fs::recursive_directory_iterator(root)) {
        if (!e.is_regular_file()) continue;
        const auto ext = e.path().extension().string();
        if (ext != ".cpp" && ext != ".h" && ext != ".hpp" && ext != ".cc")
            continue;

        const std::string code = stripNonCode(readAll(e.path()));
        std::string rel = fs::relative(e.path(), fs::path(PDFE_REPO_ROOT)).string();
        for (char& c : rel) if (c == '\\') c = '/';

        for (const auto& sym : symbols) {
            const std::string needle = sym + "(";
            for (size_t pos = code.find(needle); pos != std::string::npos;
                 pos = code.find(needle, pos + 1)) {
                if (!isWholeCall(code, pos, sym)) continue;
                const int line = 1 + static_cast<int>(
                    std::count(code.begin(), code.begin() + pos, '\n'));
                hits.push_back({rel, line, sym});
            }
        }
    }
    return hits;
}

// The ONLY file allowed to call MuPDF's page loaders directly: the guard's own
// implementation, which is what every other site now goes through.
constexpr const char* kGuardImpl = "src/core/FormXObjectDepthGuard.cpp";

// The guard's own HEADER, excluded separately from its implementation. The two
// files play different roles and conflating them cost a real assertion: the
// header's two DECLARATIONS of safeLoadPage/safeLoadPdfPage look exactly like
// call sites to a lexical scan, so counting them inflated the guarded-site
// total by two. With that padding present the floor below survived a mutation
// that removed a genuine call site (M9), which is precisely the "green while
// measuring nothing" shape. Found by mutation, not by inspection.
constexpr const char* kGuardHeader = "src/core/FormXObjectDepthGuard.h";

}  // namespace

// THE CLASS ASSERTION. No raw page load anywhere in src/ except inside the
// guard itself.
TEST(PageLoadGuardSites, NoUnguardedPageLoadCallSitesRemain)
{
    const auto raw = scan({"fz_load_page", "pdf_load_page"});

    // NON-VACUITY, and this is the guard on the guard. If the scanner is
    // pointed at the wrong root, or the comment stripper eats everything, or
    // the whole-word rule is inverted, it finds NOTHING and the assertion
    // below is trivially satisfied. The guard implementation is known to
    // contain exactly one call to each, so seeing them is proof the scanner
    // works on this tree.
    int inGuardImpl = 0;
    for (const auto& h : raw) if (h.file == kGuardImpl) ++inGuardImpl;
    ASSERT_EQ(inGuardImpl, 2)
        << "the scanner found " << inGuardImpl << " raw page-load calls in "
        << kGuardImpl << ", expected exactly 2 (fz_load_page and "
           "pdf_load_page). The scanner is not seeing the source it thinks it "
           "is, so its verdict below means nothing.";

    for (const auto& h : raw) {
        if (h.file == kGuardImpl) continue;
        ADD_FAILURE()
            << h.file << ":" << h.line << " calls " << h.symbol
            << "() directly. Every page load in src/ must go through "
               "pdfe::core::safeLoadPage() / safeLoadPdfPage(), which refuse "
               "an over-deep Form XObject nest before MuPDF's recursive "
               "resource walk starts. An unguarded load is an untrappable "
               "stack overflow on a hostile document (GitHub #159): fz_try is "
               "setjmp-based and cannot recover a stack that no longer exists.";
    }
}

// The other half. The assertion above is also satisfied by deleting every page
// load in the product, so the guarded call sites are counted too.
//
// The number is a FLOOR, not an equality: pinning the exact count would redden
// on any legitimate new caller, which trains people to bump the constant
// without thinking. A floor only reddens when sites DISAPPEAR, which is the
// direction that would make the test above vacuous.
TEST(PageLoadGuardSites, TheGuardedLoadersAreActuallyUsed)
{
    const auto guarded = scan({"safeLoadPage", "safeLoadPdfPage"});

    // The two definitions live in the guard implementation; everything else is
    // a call site.
    int callSites = 0;
    for (const auto& h : guarded) {
        if (h.file == kGuardImpl || h.file == kGuardHeader) continue;
        ++callSites;
    }

    // 12 is the enumeration made when #159 was closed as a class:
    //   Document::pageInfo / page / createDisplayList  3
    //   AutoTagger::tagPage                            1
    //   RedactionVerifier                              1
    //   BatchEngine                                    1
    //   PageOperations (get/set CropBox)               2
    //   HeaderFooterEngine                             2
    //   AlignmentInjector                              2
    // A FLOOR, not an equality: pinning the exact number would redden on any
    // legitimate new caller and train people to bump the constant without
    // thinking. A floor only reddens when sites DISAPPEAR, which is the
    // direction that would make the assertion above vacuous.
    EXPECT_GE(callSites, 12)
        << "only " << callSites << " guarded page-load call sites found in "
           "src/, expected at least 12. Sites disappearing means either the "
           "product stopped loading pages or someone routed around the guard.";
}

// The bound itself, read from the linked library rather than re-declared here.
// A test that restated the constant would agree with itself even if the guard
// had been dropped from the build.
TEST(PageLoadGuardSites, BoundSitsBelowTheUntrappableRegion)
{
    const int limit = pdfe::core::maxFormXObjectDepth();
    EXPECT_GT(limit, 8) << "the bound is below ordinary nesting depths";
    EXPECT_LT(limit, 1024)
        << "the Form XObject depth bound is " << limit << ", at or above where "
           "the native stack is exhausted — a bound there refuses only "
           "documents MuPDF already handles and leaves the process-killing "
           "case exactly as it was";
}
