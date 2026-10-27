// A tool must act on the page it was told about (GitHub #269).
//
// tools/gate/check_tool_acts_on_the_clicked_page.py is the fast-checks half of
// this control; this is the gtest half. Both exist deliberately: "CI green"
// means fast-checks.yml only, which runs no build and no gtest, so a guard
// living only there is never run by a developer before a commit, and one living
// only in gtest is never run on push. This property's family has produced four
// fatal defects (#234, #235, #236, #269) and earns both.
//
// WHAT CHANGED, AND WHY THIS FILE NO LONGER HAS A BASELINE
// =======================================================
// Twelve handlers in ten tools write an annotation through the tool's cached
// `page_` while reporting the event's `pageIndex`. The first version of this
// file treated each as a defect to reconcile in the handler and carried a
// twelve-entry ratchet.
//
// That was the wrong place. PdfCanvas is the only participant that knows both
// the clicked page and the document to resolve it from, so the reconciliation
// belongs at the DISPATCH SITE -- and now that PdfCanvas::bindToolToPage()
// binds the tool to the event's page before dispatching, all twelve handlers
// are correct without any of them changing. A ratchet listing them would be
// asserting a defect that is fixed: a list describing its own history rather
// than the tree.
//
// So the assertions moved to where the property is enforced, and there is no
// amnesty list to keep in step with anything.

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path canvasSource()
{
    return fs::path(PDFE_REPO_ROOT) / "src" / "ui" / "PdfCanvas.cpp";
}

std::string readFile(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Strip // and /* */ comments and string literals. A dispatch named in a
// comment is not a dispatch (DCA 0052).
std::string stripComments(const std::string& src)
{
    std::string out;
    out.reserve(src.size());
    const size_t n = src.size();
    for (size_t i = 0; i < n;) {
        if (i + 1 < n && src[i] == '/' && src[i + 1] == '/') {
            const size_t j = src.find('\n', i);
            i = (j == std::string::npos) ? n : j;
            continue;
        }
        if (i + 1 < n && src[i] == '/' && src[i + 1] == '*') {
            const size_t j = src.find("*/", i + 2);
            i = (j == std::string::npos) ? n : j + 2;
            out.push_back(' ');
            continue;
        }
        if (src[i] == '"') {
            size_t j = i + 1;
            while (j < n) {
                if (src[j] == '\\') { j += 2; continue; }
                if (src[j] == '"') break;
                ++j;
            }
            i = j + 1;
            out += "\"\"";
            continue;
        }
        out.push_back(src[i]);
        ++i;
    }
    return out;
}

std::vector<std::string> splitLines(const std::string& s)
{
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string line;
    while (std::getline(in, line)) out.push_back(line);
    return out;
}

// 1-based line numbers of each gesture-STARTING dispatch.
std::vector<int> gestureStartDispatches(const std::string& clean)
{
    static const std::regex re(
        R"(currentTool_\s*->\s*(onMousePress|onMouseDoubleClick)\s*\()");
    std::vector<int> out;
    const auto lines = splitLines(clean);
    for (size_t i = 0; i < lines.size(); ++i) {
        if (std::regex_search(lines[i], re)) out.push_back(static_cast<int>(i) + 1);
    }
    return out;
}

// Is the dispatch at `lineNo` inside a bindToolToPage() guard?
bool guardedByBind(const std::string& clean, int lineNo, int window = 6)
{
    const auto lines = splitLines(clean);
    const int lo = std::max(0, lineNo - 1 - window);
    for (int i = lo; i < lineNo && i < static_cast<int>(lines.size()); ++i) {
        if (lines[i].find("bindToolToPage") != std::string::npos) return true;
    }
    return false;
}

}  // namespace

// THE PROPERTY, where it is enforced: nothing reaches a tool before the tool
// has been bound to the page the event names.
TEST(ToolPageBinding, EveryGestureStartingDispatchBindsTheClickedPageFirst)
{
    const std::string clean = stripComments(readFile(canvasSource()));
    ASSERT_FALSE(clean.empty())
        << "could not read " << canvasSource().string();

    const std::vector<int> sites = gestureStartDispatches(clean);
    ASSERT_FALSE(sites.empty())
        << "no currentTool_->onMousePress / onMouseDoubleClick dispatch found "
           "in PdfCanvas.cpp. Either the dispatch moved or this test's pattern "
           "is stale -- either way it is checking nothing, which is the "
           "vacuous shape this file exists to avoid.";

    std::vector<int> unguarded;
    for (int ln : sites) {
        if (!guardedByBind(clean, ln)) unguarded.push_back(ln);
    }

    std::ostringstream detail;
    for (int ln : unguarded) detail << "\n    PdfCanvas.cpp:" << ln;

    EXPECT_TRUE(unguarded.empty())
        << unguarded.size() << " of " << sites.size()
        << " gesture-starting dispatch(es) are not guarded by "
           "bindToolToPage():" << detail.str()
        << "\n\n  GitHub #269. Without the bind the tool acts on the page it "
           "was last bound to -- MainWindow binds once, at activation -- so a "
           "click on any other page creates the annotation on the remembered "
           "one and reports the clicked one. MainWindow's read-back then "
           "resolves an index against the wrong page, throws across a Qt event "
           "handler, and the process dies with the document.";
}

// bindToolToPage must actually bind. A guard that calls a function which does
// not set the page would pass the test above while changing nothing.
TEST(ToolPageBinding, TheBindActuallySetsThePageOnTheTool)
{
    const std::string clean = stripComments(readFile(canvasSource()));
    ASSERT_FALSE(clean.empty());

    const auto at = clean.find("bool PdfCanvas::bindToolToPage");
    ASSERT_NE(at, std::string::npos)
        << "PdfCanvas::bindToolToPage is not defined, so the dispatch guards "
           "call something that does not exist";

    const std::string body = clean.substr(at, 2000);
    EXPECT_NE(body.find("setPage("), std::string::npos)
        << "bindToolToPage() does not call setPage(), so it binds nothing and "
           "the dispatch guards would pass while the tool kept its old page";
}

// THE LIFETIME RULE, which is the part a later edit breaks most easily.
// `toolPage_` OWNS a Page that the tool points into, so it must be released
// only after the tool has been told to let go.
TEST(ToolPageBinding, TheOwnedPageIsReleasedAfterTheToolIsTold)
{
    const std::string clean = stripComments(readFile(canvasSource()));
    ASSERT_FALSE(clean.empty());

    for (const char* fn : {"void PdfCanvas::clearDocument",
                           "void PdfCanvas::setDocument"}) {
        const auto at = clean.find(fn);
        ASSERT_NE(at, std::string::npos) << fn << " not found";
        const std::string body = clean.substr(at, 3000);

        const auto tell = body.find("onDocumentClosing()");
        const auto rel = body.find("releaseToolPage()");

        ASSERT_NE(rel, std::string::npos)
            << fn << " does not call releaseToolPage(); the canvas would keep "
                     "owning a Page belonging to a document that has closed";
        ASSERT_NE(tell, std::string::npos)
            << fn << " does not call currentTool_->onDocumentClosing(); the "
                     "tool would keep a raw pointer into a freed page "
                     "(GitHub #235)";
        EXPECT_LT(tell, rel)
            << fn << " releases the owned Page BEFORE telling the tool. That "
                     "order IS GitHub #235: onDocumentClosing() is what nulls "
                     "the tool's own page_, so releasing first leaves the tool "
                     "pointing into memory the canvas has just freed, and a "
                     "repaint queued after the close dereferences it.";
    }
}

// NON-VACUITY. Every assertion above is an emptiness or ordering check over a
// lexical scan, which is the project's own "empty collection" vacuous shape: a
// scan matching nothing satisfies them forever. This drives the two predicates
// directly, in both directions.
TEST(ToolPageBinding, ThePredicatesSeparateAGuardedDispatchFromABareOne)
{
    const std::string guarded = stripComments(
        "void PdfCanvas::mousePressEvent(QMouseEvent* event) {\n"
        "    if (bindToolToPage(pageIdx)) {\n"
        "        currentTool_->onMousePress(event, pageIdx, pdfPoint);\n"
        "    }\n"
        "}\n");
    auto sites = gestureStartDispatches(guarded);
    ASSERT_EQ(sites.size(), 1u) << "the dispatch pattern did not match a "
                                   "guarded dispatch at all";
    EXPECT_TRUE(guardedByBind(guarded, sites[0]))
        << "a dispatch inside a bindToolToPage() guard read as unguarded";

    const std::string bare = stripComments(
        "void PdfCanvas::mousePressEvent(QMouseEvent* event) {\n"
        "    currentTool_->onMousePress(event, pageIdx, pdfPoint);\n"
        "}\n");
    sites = gestureStartDispatches(bare);
    ASSERT_EQ(sites.size(), 1u);
    EXPECT_FALSE(guardedByBind(bare, sites[0]))
        << "an UNGUARDED dispatch read as guarded -- this test could not "
           "catch the defect it exists for";

    const std::string commented = stripComments(
        "void PdfCanvas::mousePressEvent(QMouseEvent* event) {\n"
        "    // currentTool_->onMousePress(event, pageIdx, pdfPoint);\n"
        "    somethingElse();\n"
        "}\n");
    EXPECT_TRUE(gestureStartDispatches(commented).empty())
        << "a commented-out dispatch counted as a dispatch";

    // A continuation dispatch must NOT be required to bind: a drag keeps its
    // press-time page, which InkTool/ShapeTool/StampTool already assume.
    const std::string release = stripComments(
        "void PdfCanvas::mouseReleaseEvent(QMouseEvent* event) {\n"
        "    currentTool_->onMouseRelease(event, pageIdx, pdfPoint);\n"
        "}\n");
    EXPECT_TRUE(gestureStartDispatches(release).empty())
        << "onMouseRelease was treated as a gesture-STARTING dispatch; "
           "requiring a rebind there would move a drag's stroke to whatever "
           "page the cursor ended over";
}
