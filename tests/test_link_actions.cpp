// -*- coding: utf-8 -*-
// Link action types (karen gap #1): AnnotationManager now supports
// proper /A dictionary writes for URI, GoTo, and Named actions instead
// of the old URI-in-/Contents stub. These tests verify round-trip
// semantics through save+reopen.
#include <gtest/gtest.h>
#include "annotations/AnnotationManager.h"
#include "annotations/LinkAction.h"
#include "annotations/AnnotationType.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/PathSecurity.h"

#include "security_oracles.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

using namespace pdfe::annotations;
using namespace pdfe::core;

namespace {

std::filesystem::path fixturesDir()
{
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

std::filesystem::path workCopy(const char* name, const char* source)
{
    auto d = std::filesystem::temp_directory_path() / "pdfe_link_actions";
    std::filesystem::create_directories(d);
    auto p = d / name;
    std::filesystem::copy_file(fixturesDir() / source, p,
        std::filesystem::copy_options::overwrite_existing);
    return p;
}

} // namespace

class LinkActionTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!std::filesystem::exists(fixturesDir() / "test_3page.pdf")) {
            GTEST_SKIP() << "test_3page.pdf not available";
        }
        ctx = std::make_shared<MuPdfContext>();
        path = workCopy("link_actions_work.pdf", "test_3page.pdf");
        doc = Document::open(ctx, path);
        page = doc->page(0);
        mgr = std::make_unique<AnnotationManager>(ctx);
        auto a = mgr->createAnnotation(page.get(), AnnotationType::Link,
                                       QRectF(100, 100, 80, 20));
        annotIdx = a.annotIndex;
    }
    std::shared_ptr<MuPdfContext> ctx;
    std::filesystem::path path;
    std::unique_ptr<Document> doc;
    std::unique_ptr<Page> page;
    std::unique_ptr<AnnotationManager> mgr;
    int annotIdx = -1;
};

TEST_F(LinkActionTest, DefaultLinkHasNoAction)
{
    LinkAction a = mgr->getLinkAction(page.get(), annotIdx);
    EXPECT_EQ(a.type, LinkActionType::Uri);
    EXPECT_TRUE(a.uri.empty());
}

TEST_F(LinkActionTest, SetUriRoundTrips)
{
    LinkAction in;
    in.type = LinkActionType::Uri;
    in.uri = "https://example.com/path?q=1";
    mgr->setLinkAction(page.get(), annotIdx, in);

    LinkAction out = mgr->getLinkAction(page.get(), annotIdx);
    EXPECT_EQ(out.type, LinkActionType::Uri);
    EXPECT_EQ(out.uri, in.uri);
}

TEST_F(LinkActionTest, SetGoToRoundTrips)
{
    LinkAction in;
    in.type = LinkActionType::GoTo;
    in.pageIndex = 2;
    mgr->setLinkAction(page.get(), annotIdx, in);

    LinkAction out = mgr->getLinkAction(page.get(), annotIdx);
    EXPECT_EQ(out.type, LinkActionType::GoTo);
    EXPECT_EQ(out.pageIndex, 2);
}

TEST_F(LinkActionTest, SetNamedRoundTrips)
{
    LinkAction in;
    in.type = LinkActionType::Named;
    in.namedAction = "NextPage";
    mgr->setLinkAction(page.get(), annotIdx, in);

    LinkAction out = mgr->getLinkAction(page.get(), annotIdx);
    EXPECT_EQ(out.type, LinkActionType::Named);
    EXPECT_EQ(out.namedAction, "NextPage");
}

TEST_F(LinkActionTest, SetLaunchRoundTrips)
{
    LinkAction in;
    in.type = LinkActionType::Launch;
    in.launchPath = "docs/readme.txt";
    mgr->setLinkAction(page.get(), annotIdx, in);

    LinkAction out = mgr->getLinkAction(page.get(), annotIdx);
    EXPECT_EQ(out.type, LinkActionType::Launch);
    EXPECT_EQ(out.launchPath, "docs/readme.txt");
}

TEST_F(LinkActionTest, ReplacingActionClearsPreviousData)
{
    LinkAction uri;
    uri.type = LinkActionType::Uri;
    uri.uri = "mailto:test@example.com";
    mgr->setLinkAction(page.get(), annotIdx, uri);

    LinkAction named;
    named.type = LinkActionType::Named;
    named.namedAction = "Print";
    mgr->setLinkAction(page.get(), annotIdx, named);

    LinkAction out = mgr->getLinkAction(page.get(), annotIdx);
    EXPECT_EQ(out.type, LinkActionType::Named);
    EXPECT_EQ(out.namedAction, "Print");
    EXPECT_TRUE(out.uri.empty());
}

TEST_F(LinkActionTest, GoToOutOfRangeThrows)
{
    LinkAction in;
    in.type = LinkActionType::GoTo;
    in.pageIndex = 999;  // test_3page.pdf has 3 pages
    EXPECT_ANY_THROW(mgr->setLinkAction(page.get(), annotIdx, in));
}

TEST_F(LinkActionTest, SavedUriLandsInFileAsProperADict)
{
    // /A dict is the spec-compliant location for link actions. This
    // test verifies the on-disk representation — read-back via
    // getAnnotations is a separate concern (MuPDF's pdf_first_annot
    // historically excludes /Link; a follow-up can broaden the iterator
    // to include them).
    LinkAction in;
    in.type = LinkActionType::Uri;
    in.uri = "https://persist.test/roundtrip";
    mgr->setLinkAction(page.get(), annotIdx, in);

    auto savedPath = std::filesystem::temp_directory_path()
                     / "pdfe_link_actions" / "saved_uri.pdf";
    std::filesystem::remove(savedPath);
    doc->saveAs(savedPath);
    ASSERT_TRUE(std::filesystem::exists(savedPath));

    std::ifstream f(savedPath, std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    const std::string bytes = ss.str();
    // /A <</S/URI/URI(https://persist.test/roundtrip)>>
    EXPECT_NE(bytes.find("/S/URI"), std::string::npos)
        << "saved file missing /S /URI marker";
    EXPECT_NE(bytes.find("persist.test/roundtrip"), std::string::npos)
        << "URI value missing from saved file";
}

TEST_F(LinkActionTest, SavedGoToLandsInFileAsProperADict)
{
    LinkAction in;
    in.type = LinkActionType::GoTo;
    in.pageIndex = 1;
    mgr->setLinkAction(page.get(), annotIdx, in);

    auto savedPath = std::filesystem::temp_directory_path()
                     / "pdfe_link_actions" / "saved_goto.pdf";
    std::filesystem::remove(savedPath);
    doc->saveAs(savedPath);

    std::ifstream f(savedPath, std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    const std::string bytes = ss.str();
    EXPECT_NE(bytes.find("/S/GoTo"), std::string::npos);
    // /D array must be present too; /Fit is the viewport mode we pick.
    EXPECT_NE(bytes.find("/Fit"), std::string::npos);
}

TEST_F(LinkActionTest, UriActionSurvivesSaveAndReopen)
{
    LinkAction in;
    in.type = LinkActionType::Uri;
    in.uri = "https://reopen.test/uri";
    mgr->setLinkAction(page.get(), annotIdx, in);

    auto saved = std::filesystem::temp_directory_path()
                 / "pdfe_link_actions" / "reopen_uri.pdf";
    std::filesystem::remove(saved);
    doc->saveAs(saved);

    auto ctx2 = std::make_shared<MuPdfContext>();
    auto reopened = Document::open(ctx2, saved);
    ASSERT_NE(reopened, nullptr);
    auto p = reopened->page(0);
    AnnotationManager mgr2(ctx2);
    auto annots = mgr2.getAnnotations(p.get());
    int linkIdx = -1;
    for (const auto& a : annots) {
        if (a.type == AnnotationType::Link) { linkIdx = a.annotIndex; break; }
    }
    ASSERT_GE(linkIdx, 0) << "link annotation missing after reopen "
                             "(should come through the extended /Annots walk)";
    LinkAction out = mgr2.getLinkAction(p.get(), linkIdx);
    EXPECT_EQ(out.type, LinkActionType::Uri);
    EXPECT_EQ(out.uri, in.uri);
}

TEST_F(LinkActionTest, GoToActionSurvivesSaveAndReopen)
{
    LinkAction in;
    in.type = LinkActionType::GoTo;
    in.pageIndex = 2;
    mgr->setLinkAction(page.get(), annotIdx, in);

    auto saved = std::filesystem::temp_directory_path()
                 / "pdfe_link_actions" / "reopen_goto.pdf";
    std::filesystem::remove(saved);
    doc->saveAs(saved);

    auto ctx2 = std::make_shared<MuPdfContext>();
    auto reopened = Document::open(ctx2, saved);
    auto p = reopened->page(0);
    AnnotationManager mgr2(ctx2);
    auto annots = mgr2.getAnnotations(p.get());
    int linkIdx = -1;
    for (const auto& a : annots) {
        if (a.type == AnnotationType::Link) { linkIdx = a.annotIndex; break; }
    }
    ASSERT_GE(linkIdx, 0);
    LinkAction out = mgr2.getLinkAction(p.get(), linkIdx);
    EXPECT_EQ(out.type, LinkActionType::GoTo);
    EXPECT_EQ(out.pageIndex, 2);
}

TEST_F(LinkActionTest, NamedActionSurvivesSaveAndReopen)
{
    LinkAction in;
    in.type = LinkActionType::Named;
    in.namedAction = "Print";
    mgr->setLinkAction(page.get(), annotIdx, in);

    auto saved = std::filesystem::temp_directory_path()
                 / "pdfe_link_actions" / "reopen_named.pdf";
    std::filesystem::remove(saved);
    doc->saveAs(saved);

    auto ctx2 = std::make_shared<MuPdfContext>();
    auto reopened = Document::open(ctx2, saved);
    auto p = reopened->page(0);
    AnnotationManager mgr2(ctx2);
    auto annots = mgr2.getAnnotations(p.get());
    int linkIdx = -1;
    for (const auto& a : annots) {
        if (a.type == AnnotationType::Link) { linkIdx = a.annotIndex; break; }
    }
    ASSERT_GE(linkIdx, 0);
    LinkAction out = mgr2.getLinkAction(p.get(), linkIdx);
    EXPECT_EQ(out.type, LinkActionType::Named);
    EXPECT_EQ(out.namedAction, "Print");
}

TEST_F(LinkActionTest, SavedLaunchLandsInFileAsProperADict)
{
    LinkAction in;
    in.type = LinkActionType::Launch;
    in.launchPath = "report.docx";
    mgr->setLinkAction(page.get(), annotIdx, in);

    auto saved = std::filesystem::temp_directory_path()
                 / "pdfe_link_actions" / "saved_launch.pdf";
    std::filesystem::remove(saved);
    doc->saveAs(saved);

    std::ifstream f(saved, std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    const std::string bytes = ss.str();
    EXPECT_NE(bytes.find("/S/Launch"), std::string::npos);
    EXPECT_NE(bytes.find("report.docx"), std::string::npos);
}

TEST_F(LinkActionTest, LaunchActionSurvivesSaveAndReopen)
{
    LinkAction in;
    in.type = LinkActionType::Launch;
    in.launchPath = "external/tool.pdf";
    mgr->setLinkAction(page.get(), annotIdx, in);

    auto saved = std::filesystem::temp_directory_path()
                 / "pdfe_link_actions" / "reopen_launch.pdf";
    std::filesystem::remove(saved);
    doc->saveAs(saved);

    auto ctx2 = std::make_shared<MuPdfContext>();
    auto reopened = Document::open(ctx2, saved);
    auto p = reopened->page(0);
    AnnotationManager mgr2(ctx2);
    auto annots = mgr2.getAnnotations(p.get());
    int linkIdx = -1;
    for (const auto& a : annots) {
        if (a.type == AnnotationType::Link) { linkIdx = a.annotIndex; break; }
    }
    ASSERT_GE(linkIdx, 0);
    LinkAction out = mgr2.getLinkAction(p.get(), linkIdx);
    EXPECT_EQ(out.type, LinkActionType::Launch);
    EXPECT_EQ(out.launchPath, in.launchPath);
}

TEST_F(LinkActionTest, SavedNamedLandsInFileAsProperADict)
{
    LinkAction in;
    in.type = LinkActionType::Named;
    in.namedAction = "NextPage";
    mgr->setLinkAction(page.get(), annotIdx, in);

    auto savedPath = std::filesystem::temp_directory_path()
                     / "pdfe_link_actions" / "saved_named.pdf";
    std::filesystem::remove(savedPath);
    doc->saveAs(savedPath);

    std::ifstream f(savedPath, std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    const std::string bytes = ss.str();
    EXPECT_NE(bytes.find("/S/Named"), std::string::npos);
    EXPECT_NE(bytes.find("/NextPage"), std::string::npos);
}

TEST_F(LinkActionTest, NullPageRejected)
{
    LinkAction a;
    a.type = LinkActionType::Named;
    a.namedAction = "Print";
    EXPECT_THROW(mgr->setLinkAction(nullptr, 0, a), std::invalid_argument);
    EXPECT_THROW((void)mgr->getLinkAction(nullptr, 0), std::invalid_argument);
}

TEST(LinkActionStructTest, UriValidationRejectsEmpty)
{
    LinkAction a;
    a.type = LinkActionType::Uri;
    EXPECT_FALSE(a.isValid());
    a.uri = "https://x";
    EXPECT_TRUE(a.isValid());
}

TEST(LinkActionStructTest, NamedValidationRejectsEmpty)
{
    LinkAction a;
    a.type = LinkActionType::Named;
    EXPECT_FALSE(a.isValid());
    a.namedAction = "Print";
    EXPECT_TRUE(a.isValid());
}

TEST(LinkActionStructTest, GoToValidationAcceptsZero)
{
    LinkAction a;
    a.type = LinkActionType::GoTo;
    a.pageIndex = 0;
    EXPECT_TRUE(a.isValid());
    a.pageIndex = -1;
    EXPECT_FALSE(a.isValid());
}

TEST(LinkActionStructTest, LaunchValidationRejectsEmptyPath)
{
    LinkAction a;
    a.type = LinkActionType::Launch;
    EXPECT_FALSE(a.isValid());
    a.launchPath = "file.txt";
    EXPECT_TRUE(a.isValid());
}

// ============================================================================
// PDF-58 — /GoToR and /Launch are inert at the document layer
// ============================================================================
//
// The register's rationale for PDF-58 is that `tests/test_link_actions.cpp`
// covers the round-tripping of the action types PDFE WRITES, and nothing
// covers what happens when it READS the two that can leave the document:
// /GoToR (open another file, possibly across a share) and /Launch (run a
// program). Both are safe today. Neither was guarded, so a future commit that
// implemented /GoToR — an obvious feature request — would have no red test
// standing in its way.
//
// The oracles are the positive-neutralization ones from `security_oracles.h`,
// not EXPECT_NO_THROW: a /Launch that actually fired would spawn a process,
// and a /GoToR that actually resolved would touch the filesystem. Asserting
// "it did not throw" is satisfied by both of those succeeding quietly.

namespace {

// A one-page PDF whose single Link annotation carries `actionDict` verbatim,
// so a raw action can be handed to the reader exactly as an attacker would
// write it. Built by hand rather than through AnnotationManager because
// AnnotationManager cannot WRITE a /GoToR — which is the point.
std::string buildPdfWithRawLinkAction(const std::string& actionDict)
{
    return
        "%PDF-1.7\n"
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
        "/Annots [4 0 R] >>\nendobj\n"
        "4 0 obj\n<< /Type /Annot /Subtype /Link /Rect [100 100 300 130] "
        "/P 3 0 R /A " + actionDict + " >>\nendobj\n"
        "trailer\n<< /Size 5 /Root 1 0 R >>\n%%EOF\n";
}

std::filesystem::path writeRawLinkPdf(const char* name,
                                      const std::string& actionDict)
{
    auto d = std::filesystem::temp_directory_path() / "pdfe_link_actions";
    std::filesystem::create_directories(d);
    auto p = d / name;
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    const std::string bytes = buildPdfWithRawLinkAction(actionDict);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return p;
}

} // namespace

// /GoToR names a file — often on a share — and a viewer that implemented it
// would open that file. PDFE's reader does not parse the subtype at all, so it
// falls through to the LinkAction default. This test pins that: the action
// must arrive INERT, and it must arrive inert for every /F spelling an
// attacker can choose, including the UNC one that is the whole point of the
// path-security gate.
TEST(LinkActionRemoteRefusal, GoToRResolvesOrRefusesSafely)
{
    struct Row { const char* name; const char* action; const char* why; };
    const Row rows[] = {
        {"gotor_unc.pdf",
         "<< /S /GoToR /F (\\\\\\\\attacker.example\\\\share\\\\x.pdf) "
         "/D [0 /Fit] >>",
         "UNC target"},
        {"gotor_local.pdf",
         "<< /S /GoToR /F (sibling.pdf) /D [0 /Fit] >>",
         "relative local target"},
        {"gotor_filespec.pdf",
         "<< /S /GoToR /F << /Type /Filespec /F (x.pdf) /UF (x.pdf) >> "
         "/D [0 /Fit] >>",
         "filespec-dict target"},
        {"gotor_newwindow.pdf",
         "<< /S /GoToR /F (x.pdf) /D [0 /Fit] /NewWindow true >>",
         "NewWindow variant"},
    };

    auto ctx = std::make_shared<MuPdfContext>();
    for (const auto& r : rows) {
        const auto path = writeRawLinkPdf(r.name, r.action);

        // Nothing may be spawned or written while the document is opened and
        // its actions are read.
        pdfe_test::ProcessSpawnGuard spawnGuard;

        auto doc = Document::open(ctx, path);
        ASSERT_NE(doc, nullptr) << r.why;
        auto page = doc->page(0);
        ASSERT_NE(page, nullptr) << r.why;

        AnnotationManager mgr(ctx);
        const auto annots = mgr.getAnnotations(page.get());
        ASSERT_FALSE(annots.empty()) << r.why;

        const LinkAction act = mgr.getLinkAction(page.get(), annots[0].annotIndex);

        // INERT means: not reported as any actionable type. /GoToR is not a
        // subtype the reader knows, so it must land on the default — a Uri
        // action with an empty URI, which MainWindow::followLinkAt refuses
        // with `if (act.uri.empty()) return false;`.
        EXPECT_EQ(act.type, LinkActionType::Uri) << r.why;
        EXPECT_TRUE(act.uri.empty())
            << "a /GoToR target leaked into the URI field (" << r.why
            << "): [" << act.uri << "] — it would be dispatched";
        EXPECT_TRUE(act.launchPath.empty()) << r.why;

        EXPECT_EQ(spawnGuard.newlySpawned(), "")
            << "reading a /GoToR action spawned a process (" << r.why << ")";
    }
}

// If /GoToR is ever implemented, the remote target must be refused rather
// than followed. This states the policy the implementation would have to
// satisfy, against the same predicate the /Launch site already uses — so the
// commit that adds /GoToR support inherits a test instead of a blank sheet.
TEST(LinkActionRemoteRefusal, AGoToRRemoteTargetWouldBeRefusedByThePathGate)
{
    EXPECT_TRUE(pdfe::core::path_security::is_unc_or_remote(
        "\\\\attacker.example\\share\\x.pdf"));
    EXPECT_TRUE(pdfe::core::path_security::is_unc_or_remote(
        "//attacker.example/share/x.pdf"));
    EXPECT_FALSE(pdfe::core::path_security::is_unc_or_remote("sibling.pdf"));
}

// /Launch IS parsed — MainWindow prompts on every one and refuses UNC targets
// outright. What is asserted here is the layer below that: opening, reading,
// saving and REOPENING a document containing a /Launch action must not run
// anything. `test_link_types.pdf` is the fixture the register names, and it
// already carries a /Launch; the executable-target variants are built here
// because a fixture pointing at cmd.exe is not something to keep on disk.
TEST(LinkActionRemoteRefusal, LaunchActionNeverExecutes)
{
    auto ctx = std::make_shared<MuPdfContext>();

    // 1) The named fixture, through a full open → read → save → reopen cycle.
    if (std::filesystem::exists(fixturesDir() / "test_link_types.pdf")) {
        const auto path = workCopy("launch_never_executes.pdf",
                                   "test_link_types.pdf");
        pdfe_test::ProcessSpawnGuard spawnGuard;

        auto doc = Document::open(ctx, path);
        ASSERT_NE(doc, nullptr);
        AnnotationManager mgr(ctx);

        bool sawLaunch = false;
        for (int p = 0; p < doc->pageCount(); ++p) {
            auto page = doc->page(p);
            if (!page) continue;
            for (const auto& a : mgr.getAnnotations(page.get())) {
                if (a.type != AnnotationType::Link) continue;
                const LinkAction act = mgr.getLinkAction(page.get(), a.annotIndex);
                if (act.type == LinkActionType::Launch) sawLaunch = true;
            }
        }
        EXPECT_TRUE(sawLaunch)
            << "vacuity guard: test_link_types.pdf no longer contains a "
               "/Launch action, so this case proves nothing about /Launch";

        EXPECT_EQ(spawnGuard.newlySpawned(), "")
            << "opening and reading a document with a /Launch action "
               "executed something";
    }

    // 2) Targets an attacker would actually choose. Reading these must be as
    //    inert as reading (notes.txt) is.
    const char* const hostileTargets[] = {
        "cmd.exe",
        "C:\\\\Windows\\\\System32\\\\calc.exe",
        "\\\\\\\\attacker.example\\\\share\\\\payload.exe",
        "powershell.exe -c whoami",
    };
    int i = 0;
    for (const char* target : hostileTargets) {
        const std::string name = "launch_hostile_" + std::to_string(i++) + ".pdf";
        const auto path = writeRawLinkPdf(
            name.c_str(),
            std::string("<< /S /Launch /F (") + target + ") >>");

        pdfe_test::ProcessSpawnGuard spawnGuard;

        auto doc = Document::open(ctx, path);
        ASSERT_NE(doc, nullptr) << target;
        auto page = doc->page(0);
        ASSERT_NE(page, nullptr) << target;

        AnnotationManager mgr(ctx);
        const auto annots = mgr.getAnnotations(page.get());
        ASSERT_FALSE(annots.empty()) << target;
        const LinkAction act = mgr.getLinkAction(page.get(), annots[0].annotIndex);

        // The reader is expected to SURFACE the path — that is what lets the
        // dispatcher refuse it. It must not act on it.
        EXPECT_EQ(act.type, LinkActionType::Launch) << target;
        EXPECT_FALSE(act.launchPath.empty()) << target;

        EXPECT_EQ(spawnGuard.newlySpawned(), "")
            << "reading a /Launch action for [" << target << "] ran it";
    }

    // 3) The refusal policy the dispatcher applies to what the reader
    //    surfaced. MainWindow::followLinkAt's Launch case refuses a UNC
    //    target before Qt's URL parsing can touch the resolver; every other
    //    target reaches the always-on user prompt. Dispatch-level coverage of
    //    that prompt lives in tests/test_malicious_pdf_security.cpp
    //    (sLaunchPromptCount_ / setLaunchHandlerForTesting); this row states
    //    the predicate that gate depends on.
    EXPECT_TRUE(pdfe::core::path_security::is_unc_or_remote(
        std::string_view{"\\\\attacker.example\\share\\payload.exe"}));
    EXPECT_FALSE(pdfe::core::path_security::is_unc_or_remote(
        std::string_view{"notes.txt"}));
}
