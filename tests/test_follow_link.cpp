// -*- coding: utf-8 -*-
// Follow-link viewer UX: MainWindow::followLinkAt hit-tests link
// annotations and dispatches by /A action type. URI opens via the OS
// shell (not exercised here), GoTo navigates the canvas, Named maps
// to MainWindow slots (NextPage/PrevPage/FirstPage/LastPage/ZoomIn/
// ZoomOut/Print), Launch prompts before invoking the shell.
//
// Tests verify the GoTo and Named paths, which have observable
// effects in-session. URI/Launch require external processes and are
// covered structurally (the action type is surfaced) rather than
// end-to-end.
#include <gtest/gtest.h>
#include "ui/MainWindow.h"
#include "ui/PdfCanvas.h"
#include "annotations/AnnotationManager.h"
#include "annotations/LinkAction.h"
#include "annotations/SelectTool.h"
#include "annotations/AnnotationType.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "textedit/TextEditTool.h"

#include <QApplication>
#include <QKeyEvent>
#include <QTemporaryDir>

#include <filesystem>
#include <memory>

namespace {
std::filesystem::path fixturesDir() {
    return std::filesystem::path(TEST_FIXTURES_DIR);
}
} // namespace

class FollowLinkTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!std::filesystem::exists(fixturesDir() / "test_3page.pdf")) {
            GTEST_SKIP() << "test_3page.pdf not available";
        }
        tempDir = std::make_unique<QTemporaryDir>();
        ASSERT_TRUE(tempDir->isValid());
        workPdf = std::filesystem::path(tempDir->path().toStdString())
                  / "follow_link_work.pdf";
        std::filesystem::copy_file(fixturesDir() / "test_3page.pdf", workPdf);

        // Plant a GoTo link and a NextPage named link on page 0 using
        // a standalone AnnotationManager before MainWindow opens the
        // file, so the document is in a known state.
        {
            auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
            auto doc = pdfe::core::Document::open(ctx, workPdf);
            auto page = doc->page(0);
            pdfe::annotations::AnnotationManager mgr(ctx);

            auto gotoLink = mgr.createAnnotation(
                page.get(), pdfe::annotations::AnnotationType::Link,
                QRectF(50, 50, 100, 50));
            pdfe::annotations::LinkAction gotoAct;
            gotoAct.type = pdfe::annotations::LinkActionType::GoTo;
            gotoAct.pageIndex = 2;
            mgr.setLinkAction(page.get(), gotoLink.annotIndex, gotoAct);

            auto namedLink = mgr.createAnnotation(
                page.get(), pdfe::annotations::AnnotationType::Link,
                QRectF(200, 50, 100, 50));
            pdfe::annotations::LinkAction namedAct;
            namedAct.type = pdfe::annotations::LinkActionType::Named;
            namedAct.namedAction = "NextPage";
            mgr.setLinkAction(page.get(), namedLink.annotIndex, namedAct);

            auto uriLink = mgr.createAnnotation(
                page.get(), pdfe::annotations::AnnotationType::Link,
                QRectF(350, 50, 100, 50));
            pdfe::annotations::LinkAction uriAct;
            uriAct.type = pdfe::annotations::LinkActionType::Uri;
            uriAct.uri = "https://example.test/no-network";
            mgr.setLinkAction(page.get(), uriLink.annotIndex, uriAct);

            auto savedPath = std::filesystem::path(tempDir->path().toStdString())
                             / "follow_link_with_links.pdf";
            doc->saveAs(savedPath);
            workPdf = savedPath;
        }

        mw = std::make_unique<pdfe::ui::MainWindow>();
        mw->openFile(QString::fromStdString(workPdf.string()));
        QApplication::processEvents();
    }
    void TearDown() override {
        mw.reset();
        QApplication::processEvents();
        tempDir.reset();
    }
    std::unique_ptr<QTemporaryDir> tempDir;
    std::filesystem::path workPdf;
    std::unique_ptr<pdfe::ui::MainWindow> mw;
};

TEST_F(FollowLinkTest, GoToLinkNavigatesCanvasToTargetPage)
{
    auto* canvas = mw->findChild<pdfe::ui::PdfCanvas*>("pdfCanvas");
    ASSERT_NE(canvas, nullptr);
    // Start on page 0.
    canvas->goToPage(0);
    QApplication::processEvents();
    ASSERT_EQ(canvas->currentPage(), 0);

    // Click inside the GoTo rect (50, 50, 100, 50).
    const bool handled = mw->followLinkAt(0, QPointF(80.0, 70.0));
    EXPECT_TRUE(handled);
    QApplication::processEvents();
    EXPECT_EQ(canvas->currentPage(), 2);
}

TEST_F(FollowLinkTest, NamedNextPageLinkAdvancesPage)
{
    auto* canvas = mw->findChild<pdfe::ui::PdfCanvas*>("pdfCanvas");
    canvas->goToPage(0);
    QApplication::processEvents();

    // Named link rect: (200, 50, 100, 50).
    const bool handled = mw->followLinkAt(0, QPointF(230.0, 70.0));
    EXPECT_TRUE(handled);
    QApplication::processEvents();
    EXPECT_EQ(canvas->currentPage(), 1);
}

TEST_F(FollowLinkTest, ClickOutsideAnyLinkReturnsFalse)
{
    // (10, 10) is outside all three link rects planted above.
    const bool handled = mw->followLinkAt(0, QPointF(10.0, 10.0));
    EXPECT_FALSE(handled);
}

TEST_F(FollowLinkTest, ClickOnUriLinkReturnsTrueWithoutCrashing)
{
    // (350, 50, 100, 50). We can't verify QDesktopServices::openUrl
    // was called (it would try to launch the OS browser in test), so
    // the best we can do is confirm the handler returned true and
    // didn't crash. The URI scheme is example.test to ensure any
    // actual shell dispatch is a no-op.
    const bool handled = mw->followLinkAt(0, QPointF(380.0, 70.0));
    EXPECT_TRUE(handled);
}

TEST_F(FollowLinkTest, OutOfRangePageIndexReturnsFalse)
{
    const bool handled = mw->followLinkAt(99, QPointF(80.0, 70.0));
    EXPECT_FALSE(handled);
}

TEST_F(FollowLinkTest, NullDocPathIsSafe)
{
    mw.reset();
    QApplication::processEvents();
    // After closing MainWindow, constructing a fresh one and calling
    // followLinkAt without opening a file must not crash.
    auto fresh = std::make_unique<pdfe::ui::MainWindow>();
    EXPECT_FALSE(fresh->followLinkAt(0, QPointF(50, 50)));
}

// ───────────────────────────────────────────────────────────────────────────
// GitHub #135 — CI-LIMITATION-NAMED-NEXTPAGE-OVERSHOOT.
//
// The live-UI test clicks the Named/NextPage link in `test_link_types.pdf`
// and reports landing on "Page 3 of 3" instead of "Page 2 of 3". Two
// candidate root causes were recorded and never separated:
//
//   (a) the click resolves to the WRONG LINK (the fixture also carries a
//       GoTo link whose target IS page index 2 = "Page 3 of 3"), or
//   (b) the Named/NextPage HANDLER advances by two.
//
// Every test above plants its own links with AnnotationManager and then
// clicks the rect it just planted, so the click coordinate and the
// annotation rect come from the SAME value — a self-consistent pair that
// cannot see a convention mismatch or a resolution error. That is the
// "assert against an input, never another output" trap, and it is why the
// suite was green while the live test was red.
//
// These tests use the COMMITTED fixture at the EXACT coordinate the live
// test dispatches, and separate resolution from dispatch:
//   * `NamedNextPageResolvesToTheNamedLinkNotTheGoToLink` is the (a) probe:
//     it asserts WHICH action the point resolves to, without navigating.
//   * `NamedNextPageOnTheFixtureAdvancesExactlyOnePage` is the (b) probe:
//     it dispatches and asserts the landing page.
// A red (a) with a green handler means (a); a green (a) with a wrong
// landing page means (b).
//
// Coordinates: the fixture's /Rect values are PDF Y-up. PDFE's
// AnnotationData::rect is MuPDF Y-down (see AnnotationManager's
// readAnnotationData), so the Named rect [50 592 150 642] on a 792pt page
// is (50,150)..(150,200) Y-down, and the live test's PDF point (100, 617)
// Y-up is (100, 175) Y-down. Both are stated here as named constants so a
// future reader does not have to re-derive the flip.
// ───────────────────────────────────────────────────────────────────────────

class FollowLinkFixtureTest : public ::testing::Test {
protected:
    // The committed fixture's page height, and the four /Rect entries from
    // tests/fixtures/generate_link_types_pdf.py, converted to Y-down.
    static constexpr double kPageHeightPt = 792.0;
    // Named /NextPage link: /Rect [50 592 150 642] -> Y-down (50,150)-(150,200)
    static constexpr double kNamedX = 100.0;
    static constexpr double kNamedYDown = 175.0;    // == 792 - 617
    // GoTo (target page index 2) link: /Rect [200 692 300 742]
    static constexpr double kGoToX = 250.0;
    static constexpr double kGoToYDown = 75.0;      // == 792 - 717
    // A point on the page that is inside no link rect at all.
    static constexpr double kNoLinkX = 400.0;
    static constexpr double kNoLinkYDown = 400.0;

    void SetUp() override {
        fixture = fixturesDir() / "test_link_types.pdf";
        if (!std::filesystem::exists(fixture)) {
            GTEST_SKIP() << "test_link_types.pdf not available";
        }
        tempDir = std::make_unique<QTemporaryDir>();
        ASSERT_TRUE(tempDir->isValid());
        workPdf = std::filesystem::path(tempDir->path().toStdString())
                  / "link_types_work.pdf";
        std::filesystem::copy_file(fixture, workPdf);

        mw = std::make_unique<pdfe::ui::MainWindow>();
        mw->openFile(QString::fromStdString(workPdf.string()));
        QApplication::processEvents();
        canvas = mw->findChild<pdfe::ui::PdfCanvas*>("pdfCanvas");
        ASSERT_NE(canvas, nullptr);
    }
    void TearDown() override {
        mw.reset();
        QApplication::processEvents();
        tempDir.reset();
    }

    // Resolve the link at a Y-down PDF point exactly the way followLinkAt
    // does — same enumeration, same hit test, same index — but WITHOUT
    // dispatching. This is the `testFollowLinkAtPdfPointPipe` the
    // known-issues row asked for, at the C++ layer where it needs no new
    // Q_INVOKABLE on a widely-included header.
    bool resolveLinkAt(double x, double yDown,
                       pdfe::annotations::LinkAction& out) const {
        auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
        auto doc = pdfe::core::Document::open(ctx, workPdf);
        auto page = doc->page(0);
        pdfe::annotations::AnnotationManager mgr(ctx);
        const auto annots = mgr.getAnnotations(page.get());
        const QPointF pt(x, yDown);
        for (const auto& a : annots) {
            if (a.type != pdfe::annotations::AnnotationType::Link) continue;
            if (!a.rect.contains(pt)) continue;
            out = mgr.getLinkAction(page.get(), a.annotIndex);
            return true;
        }
        return false;
    }

    int linkAnnotCount() const {
        auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
        auto doc = pdfe::core::Document::open(ctx, workPdf);
        auto page = doc->page(0);
        pdfe::annotations::AnnotationManager mgr(ctx);
        int n = 0;
        for (const auto& a : mgr.getAnnotations(page.get())) {
            if (a.type == pdfe::annotations::AnnotationType::Link) ++n;
        }
        return n;
    }

    std::unique_ptr<QTemporaryDir> tempDir;
    std::filesystem::path fixture;
    std::filesystem::path workPdf;
    std::unique_ptr<pdfe::ui::MainWindow> mw;
    pdfe::ui::PdfCanvas* canvas = nullptr;
};

// NON-VACUITY GUARD for every test below. If the fixture stops carrying
// four link annotations — or the /Annots fallback walk stops surfacing
// links on a reopened document — every hit test below silently resolves
// nothing and the "no link here" control passes for the wrong reason.
TEST_F(FollowLinkFixtureTest, TheFixtureStillCarriesItsFourLinkAnnotations)
{
    EXPECT_EQ(linkAnnotCount(), 4)
        << "vacuity guard: test_link_types.pdf must expose four /Link "
           "annotations on page 0; if it does not, the hit tests below "
           "are resolving against an empty collection";
}

// (a) PROBE — resolution only, no dispatch.
TEST_F(FollowLinkFixtureTest, NamedNextPageResolvesToTheNamedLinkNotTheGoToLink)
{
    pdfe::annotations::LinkAction act{};
    ASSERT_TRUE(resolveLinkAt(kNamedX, kNamedYDown, act))
        << "PDF point (" << kNamedX << ", " << kNamedYDown << ") Y-down "
           "resolved to NO link at all; the live test's click coordinate "
           "does not land on the Named/NextPage rect";
    EXPECT_EQ(act.type, pdfe::annotations::LinkActionType::Named)
        << "the point resolved to a link, but not the Named one — this is "
           "root cause (a), the click routing to the wrong link";
    EXPECT_EQ(act.namedAction, "NextPage");

    // CONTROL — the same probe on the GoTo link must resolve to GoTo, and
    // to page index 2. Without it, a resolver that returned Named for
    // EVERY point would satisfy the assertion above.
    pdfe::annotations::LinkAction gotoAct{};
    ASSERT_TRUE(resolveLinkAt(kGoToX, kGoToYDown, gotoAct));
    EXPECT_EQ(gotoAct.type, pdfe::annotations::LinkActionType::GoTo);
    EXPECT_EQ(gotoAct.pageIndex, 2)
        << "control: the GoTo link's target is page index 2, i.e. the "
           "'Page 3 of 3' the #135 symptom reports — so a mis-resolution "
           "to THIS link is exactly what the symptom looks like";

    // CONTROL — a point on the page inside no link rect must resolve to
    // nothing. A hit test that matched everything would pass both of the
    // assertions above.
    pdfe::annotations::LinkAction none{};
    EXPECT_FALSE(resolveLinkAt(kNoLinkX, kNoLinkYDown, none))
        << "control: an empty area of the page must resolve to no link";
}

// (b) PROBE — dispatch, and assert where it lands.
TEST_F(FollowLinkFixtureTest, NamedNextPageOnTheFixtureAdvancesExactlyOnePage)
{
    canvas->goToPage(0);
    QApplication::processEvents();
    // Zero-vs-zero guard: the starting page must differ from the expected
    // landing page, otherwise "did not move" would read as a pass.
    ASSERT_EQ(canvas->currentPage(), 0);
    ASSERT_NE(canvas->currentPage(), 1);

    const bool handled = mw->followLinkAt(0, QPointF(kNamedX, kNamedYDown));
    EXPECT_TRUE(handled)
        << "the Named/NextPage link at the live test's own coordinate was "
           "not hit at all";
    QApplication::processEvents();
    EXPECT_EQ(canvas->currentPage(), 1)
        << "Named/NextPage from page 0 must land on page index 1 "
           "(\"Page 2 of 3\"); landing on 2 is the #135 symptom";
}

// CONTROL for the dispatch probe: the GoTo link on the SAME fixture must
// still land on page index 2. Without it, a canvas that refused every
// navigation would pass the test above only if it also started on page 1.
TEST_F(FollowLinkFixtureTest, GoToLinkOnTheFixtureStillLandsOnItsOwnTarget)
{
    canvas->goToPage(0);
    QApplication::processEvents();
    ASSERT_EQ(canvas->currentPage(), 0);

    const bool handled = mw->followLinkAt(0, QPointF(kGoToX, kGoToYDown));
    EXPECT_TRUE(handled);
    QApplication::processEvents();
    EXPECT_EQ(canvas->currentPage(), 2);
}

// The LIVE path, in-process. The live pywinauto test does not call
// followLinkAt directly — it writes to testClickByPdfCoordPipe, which calls
// PdfCanvas::clickByPdfCoord, which maps the PDF point to a canvas pixel and
// synthesises press+release. Everything above bypasses that mapping, so it
// cannot see a fault in it. The geometry here is the geometry MEASURED from a
// live pdfe.exe run on this fixture (canvas 1048x885, page 0 drawn at
// (221,10) 605x784, i.e. zoom ~0.9886) rather than invented, so the test
// exercises the arithmetic the live run actually performs.
TEST_F(FollowLinkFixtureTest, TheClickByPdfCoordPipePathLandsOnTheNamedLink)
{
    canvas->resize(1048, 885);
    canvas->setZoom(605.0f / 612.0f);
    QApplication::processEvents();
    canvas->goToPage(0);
    QApplication::processEvents();
    ASSERT_EQ(canvas->currentPage(), 0);

    // THE PRECONDITION THAT MAKES THIS TEST MEAN ANYTHING. Opening a
    // document installs the Select tool, and mousePressEvent used to hand
    // every left-click to the active tool and return — which put the link
    // hit-test below it out of reach for the entire time a document was
    // open. If this assertion ever reads null, the click is taking the
    // no-tool path and the test is no longer covering the defect it exists
    // for, so it is an ASSERT, not a comment.
    ASSERT_NE(canvas->currentTool(), nullptr)
        << "vacuity guard: no tool is active, so this test would exercise "
           "the already-working no-tool path instead of the #135 defect";
    ASSERT_EQ(canvas->textEditTool(), nullptr);
    ASSERT_EQ(canvas->imageTool(), nullptr);
    ASSERT_EQ(canvas->scrollOffsetY(), 0);

    const bool dispatched =
        canvas->clickByPdfCoord(0, kNamedX, kNamedYDown);
    EXPECT_TRUE(dispatched)
        << "clickByPdfCoord refused to map the point at all";
    QApplication::processEvents();
    EXPECT_EQ(canvas->currentPage(), 1)
        << "the synthetic click reached the canvas but did not follow the "
           "Named/NextPage link";
}

// CONTROL for the fix above, in the direction it could over-reach. A tool
// the user explicitly chose in order to CREATE something must keep its own
// press: a click that happens to land on a link while the Ink tool is armed
// is a stroke, not a navigation. Without this, "follow links from any tool"
// would pass the test above and quietly break every creation tool.
TEST_F(FollowLinkFixtureTest, ACreationToolKeepsItsClickEvenOverALink)
{
    canvas->resize(1048, 885);
    canvas->setZoom(605.0f / 612.0f);
    QApplication::processEvents();
    canvas->goToPage(0);
    QApplication::processEvents();
    ASSERT_EQ(canvas->currentPage(), 0);

    // Highlight (TextMarkupTool) is used rather than Ink or StickyNote
    // because when this control was written, pressing with either of those
    // on THIS fixture threw "MuPDF error: Annotation index out of range" out
    // of the Qt event handler — at a NON-link point too, so an unrelated
    // defect that would have contaminated a test about links. That was
    // GitHub #170, fixed in pipeline p83-outstanding phase-4; the Ink and
    // StickyNote presses now have their own tests below. Highlight is kept
    // here so this control keeps covering a third, independent tool.
    ASSERT_TRUE(mw->activateToolByObjectName("highlightToolAction"))
        << "vacuity guard: the Highlight tool did not activate, so this "
           "control would be measuring the Select tool again";
    QApplication::processEvents();
    ASSERT_NE(canvas->currentTool(), nullptr);

    EXPECT_TRUE(canvas->clickByPdfCoord(0, kNamedX, kNamedYDown));
    QApplication::processEvents();
    EXPECT_EQ(canvas->currentPage(), 0)
        << "the Highlight tool's press was stolen by the link hit-test";
}


// CONTROL: with the Select tool active — the fixed path — a click that
// lands on NO link must still not navigate. Separates "links work" from
// "the canvas navigates on any click".
TEST_F(FollowLinkFixtureTest, TheClickPipeOnAnEmptyPointDoesNotNavigate)
{
    canvas->resize(1048, 885);
    canvas->setZoom(605.0f / 612.0f);
    QApplication::processEvents();
    canvas->goToPage(0);
    QApplication::processEvents();
    ASSERT_NE(canvas->currentTool(), nullptr);

    EXPECT_TRUE(canvas->clickByPdfCoord(0, kNoLinkX, kNoLinkYDown));
    QApplication::processEvents();
    EXPECT_EQ(canvas->currentPage(), 0);
}

// CONTROL: clicking an empty part of the page must not navigate at all.
// This is what separates "NextPage works" from "any click advances".
TEST_F(FollowLinkFixtureTest, AClickOnNoLinkDoesNotNavigate)
{
    canvas->goToPage(0);
    QApplication::processEvents();
    ASSERT_EQ(canvas->currentPage(), 0);

    EXPECT_FALSE(mw->followLinkAt(0, QPointF(kNoLinkX, kNoLinkYDown)));
    QApplication::processEvents();
    EXPECT_EQ(canvas->currentPage(), 0);
}

// ───────────────────────────────────────────────────────────────────────────
// GitHub #170 — an Ink/StickyNote press on a page that carries link
// annotations threw "Annotation index out of range" out of a Qt event
// handler.
//
// DIAGNOSIS (measured, not guessed). The message is PDFE's own fz_throw, not
// MuPDF's — searching for it finds 23 sites in AnnotationManager.cpp and
// RedactionManager.cpp and none anywhere in the MuPDF 1.26.10 source tree.
// Every one of them fires when getNthAnnot() cannot resolve the index it was
// handed.
//
// AnnotationManager carries TWO different annotation numberings and they are
// not interchangeable:
//
//   * getNthAnnot() — used by getAnnotation / setColor / setContents /
//     setInkStrokes / setQuadPoints / ... — walks pdf_first_annot. MuPDF's
//     pdf_load_annots SKIPS the /Link (and /Popup) subtypes when it builds
//     that list for a freshly loaded page, so links read from disk are simply
//     not in it.
//   * getAnnotations() — the list the UI reads — runs that same walk and then
//     appends the link entries the walk skipped, numbering them upward from
//     the walk's count, so the UI can see and edit links on a reopened
//     document.
//
// getAnnotations() is therefore LONGER than the addressable range whenever a
// loaded page carries links. MainWindow::onAnnotationCreatedByTool addressed
// the newly created annotation as `getAnnotations().size() - 1` — a list
// POSITION — and on this fixture that is 4 while the new annotation is index
// 0. The next setColor() threw, straight out of PdfCanvas::mousePressEvent.
//
// That also accounts for the two properties the issue reported but could not
// explain: it is independent of where the press lands (the index comes from a
// COUNT, not from the point), and it needs a document whose links came from
// disk rather than from the current session.
//
// The two tests below pin the mechanism; the ones after them drive the
// product's click route.
// ───────────────────────────────────────────────────────────────────────────

TEST_F(FollowLinkFixtureTest, TheAnnotationListIsLongerThanTheIndexableRange)
{
    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, workPdf);
    auto page = doc->page(0);
    pdfe::annotations::AnnotationManager mgr(ctx);

    // NON-VACUITY: the fixture must actually carry its four loaded-from-disk
    // links, otherwise the two numberings coincide and this proves nothing.
    const auto before = mgr.getAnnotations(page.get());
    ASSERT_EQ(before.size(), 4u)
        << "vacuity guard: test_link_types.pdf must present four link "
           "annotations before anything is created on the page";

    const auto created = mgr.createAnnotation(
        page.get(), pdfe::annotations::AnnotationType::Text,
        QRectF(kNoLinkX, kNoLinkYDown, 20, 20));
    EXPECT_EQ(created.annotIndex, 0)
        << "createAnnotation derives its index by matching the pdf_annot "
           "pointer it just made against the pdf_first_annot walk; the four "
           "links are not in that walk, so the new annotation is index 0";

    const auto after = mgr.getAnnotations(page.get());
    ASSERT_EQ(after.size(), 5u);
    const int lastListPosition = static_cast<int>(after.size()) - 1;
    EXPECT_EQ(lastListPosition, 4);
    EXPECT_NE(lastListPosition, created.annotIndex)
        << "THE DEFECT: the last POSITION in the list the UI reads is not "
           "the INDEX of the annotation just created";

    // The consequence, stated as the product hit it.
    EXPECT_ANY_THROW(mgr.getAnnotation(page.get(), lastListPosition))
        << "addressing the list's last position must be out of range — if "
           "this ever stops throwing, the two numberings have been unified "
           "and MainWindow's index choice no longer matters";
    // CONTROL: the index createAnnotation actually returned resolves fine.
    EXPECT_NO_THROW(mgr.getAnnotation(page.get(), created.annotIndex));
}

// CONTROL for the diagnosis: on a page with NO loaded links the two
// numberings agree, which is why `size() - 1` looked correct for years and
// why every pre-existing annotation test stayed green.
TEST_F(FollowLinkFixtureTest, OnALinkFreePageThePositionAndTheIndexAgree)
{
    const auto plain = fixturesDir() / "test_3page.pdf";
    if (!std::filesystem::exists(plain)) GTEST_SKIP() << "test_3page.pdf missing";

    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, plain);
    auto page = doc->page(0);
    pdfe::annotations::AnnotationManager mgr(ctx);

    ASSERT_EQ(mgr.getAnnotations(page.get()).size(), 0u)
        << "vacuity guard: this control needs a page with no annotations";

    const auto created = mgr.createAnnotation(
        page.get(), pdfe::annotations::AnnotationType::Text,
        QRectF(100, 100, 20, 20));
    const auto after = mgr.getAnnotations(page.get());
    ASSERT_EQ(after.size(), 1u);
    EXPECT_EQ(static_cast<int>(after.size()) - 1, created.annotIndex)
        << "with no links present the list position and the addressable "
           "index coincide — the defect is invisible here";
}

// The product path. Each test drives the CLICK ROUTE (clickByPdfCoord →
// QApplication::sendEvent → PdfCanvas::mousePressEvent → the tool →
// MainWindow::onAnnotationCreatedByTool), which is where the throw escaped;
// calling AnnotationManager directly would never have seen it.
class AnnotationToolOnLinkedPageTest : public FollowLinkFixtureTest {
protected:
    void SetUp() override {
        FollowLinkFixtureTest::SetUp();
        if (IsSkipped() || HasFatalFailure()) return;
        canvas->resize(1048, 885);
        canvas->setZoom(605.0f / 612.0f);
        QApplication::processEvents();
        canvas->goToPage(0);
        QApplication::processEvents();
    }

    // Annotations on the LIVE, in-memory document that are not links, i.e.
    // the ones a creation tool just made. Counting through getAnnotations()
    // rather than probing indices keeps this guard independent of the
    // numbering the fix is about.
    int nonLinkAnnotCount() const {
        auto* doc = mw->documentForTesting();
        if (!doc) return -1;
        auto page = doc->page(0);
        if (!page) return -1;
        pdfe::annotations::AnnotationManager mgr(doc->context());
        int n = 0;
        for (const auto& a : mgr.getAnnotations(page.get())) {
            if (a.type != pdfe::annotations::AnnotationType::Link) ++n;
        }
        return n;
    }

    // Press with `actionName` at a PDF point and require the press to survive.
    void pressWithToolAt(const char* actionName, double x, double yDown) {
        ASSERT_EQ(nonLinkAnnotCount(), 0)
            << "vacuity guard: the page must start with no non-link "
               "annotations, or the post-click count proves nothing";
        ASSERT_TRUE(mw->activateToolByObjectName(QString::fromLatin1(actionName)))
            << "vacuity guard: " << actionName << " did not activate, so this "
               "test would be pressing with whatever tool was already armed";
        QApplication::processEvents();
        ASSERT_NE(canvas->currentTool(), nullptr)
            << "vacuity guard: no annotation tool is installed on the canvas";

        ASSERT_NO_THROW(canvas->clickByPdfCoord(0, x, yDown))
            << "GitHub #170: the press threw out of PdfCanvas::mousePressEvent";
        QApplication::processEvents();

        // NON-VACUITY: the press must actually have created something. A tool
        // that silently did nothing would also "not throw".
        EXPECT_EQ(nonLinkAnnotCount(), 1)
            << "the press survived but created no annotation, so the "
               "no-throw assertion above covered an inert code path";
    }

    // The live annotation at an addressable index, or a default-constructed
    // one (annotIndex == -1) if the index does not resolve.
    pdfe::annotations::AnnotationData liveAnnotation(int index) const {
        auto* doc = mw->documentForTesting();
        if (!doc) return {};
        auto page = doc->page(0);
        if (!page) return {};
        pdfe::annotations::AnnotationManager mgr(doc->context());
        try {
            return mgr.getAnnotation(page.get(), index);
        } catch (...) {
            return {};
        }
    }
};

TEST_F(AnnotationToolOnLinkedPageTest, StickyNotePressAtANonLinkPointSurvives)
{
    pressWithToolAt("stickyNoteToolAction", kNoLinkX, kNoLinkYDown);
}

TEST_F(AnnotationToolOnLinkedPageTest, StickyNotePressAtALinkPointSurvives)
{
    pressWithToolAt("stickyNoteToolAction", kNamedX, kNamedYDown);
}

TEST_F(AnnotationToolOnLinkedPageTest, InkPressAtANonLinkPointSurvives)
{
    pressWithToolAt("inkToolAction", kNoLinkX, kNoLinkYDown);
    if (HasFatalFailure()) return;

    // The STRONGER assertion, and the one a mere "did not throw" cannot make.
    // onAnnotationCreatedByTool deletes the tool's annotation, recreates it
    // through the undo command, and then restores the ink strokes BY INDEX.
    // If that index is wrong the restore either throws (the #170 symptom) or
    // writes the strokes onto some other annotation — so a stroke list that
    // survived at index 0 is what proves the index was right.
    const auto restored = liveAnnotation(0);
    EXPECT_EQ(restored.type, pdfe::annotations::AnnotationType::Ink);
    EXPECT_FALSE(restored.inkStrokes.empty())
        << "the recreated Ink annotation carries no strokes: the "
           "property-restoration pass did not reach index 0";
}

TEST_F(AnnotationToolOnLinkedPageTest, InkPressAtALinkPointSurvives)
{
    pressWithToolAt("inkToolAction", kNamedX, kNamedYDown);
}

// A creation tool's press must NOT navigate, whatever else it does. This is
// the #135 restriction restated for the two tools #170 unblocked: they were
// left out of that control only because they threw.
// The THIRD instance of the same defect, in SelectTool's paste handler.
// Driven through the product route — create, select, Ctrl+C, Ctrl+V — because
// the wrong index lives in MainWindow's callback, not in SelectTool.
TEST_F(AnnotationToolOnLinkedPageTest, PastingAnAnnotationOnALinkedPageSurvives)
{
    ASSERT_TRUE(mw->activateToolByObjectName("stickyNoteToolAction"));
    QApplication::processEvents();
    ASSERT_NO_THROW(canvas->clickByPdfCoord(0, kNoLinkX, kNoLinkYDown));
    QApplication::processEvents();
    ASSERT_EQ(nonLinkAnnotCount(), 1)
        << "vacuity guard: nothing was created, so there is nothing to copy";

    // Give the note some contents, the way a user typing a comment would.
    // WITHOUT this the paste carries a default-yellow, empty-contents,
    // opaque annotation and MainWindow's restore block calls NO setter at
    // all — the wrong index would never be dereferenced and this test would
    // pass against the defect. (Measured: mutant M1B survived until this
    // line existed.)
    {
        auto* doc = mw->documentForTesting();
        ASSERT_NE(doc, nullptr);
        auto page = doc->page(0);
        ASSERT_NE(page, nullptr);
        pdfe::annotations::AnnotationManager mgr(doc->context());
        ASSERT_NO_THROW(mgr.setContents(page.get(), 0, "PASTE-ME"));
    }

    // Creation auto-switches to the Select tool.
    auto* select =
        dynamic_cast<pdfe::annotations::SelectTool*>(canvas->currentTool());
    ASSERT_NE(select, nullptr) << "vacuity guard: the Select tool is not armed";

    // Select the new annotation with a click so Ctrl+C has a subject.
    ASSERT_NO_THROW(canvas->clickByPdfCoord(0, kNoLinkX, kNoLinkYDown));
    QApplication::processEvents();
    ASSERT_GE(select->selectedAnnotIndex(), 0)
        << "vacuity guard: the click did not select the annotation, so the "
           "copy below would put nothing on the clipboard";

    QKeyEvent copyEvt(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
    QApplication::sendEvent(canvas, &copyEvt);
    QKeyEvent pasteEvt(QEvent::KeyPress, Qt::Key_V, Qt::ControlModifier);
    ASSERT_NO_THROW(QApplication::sendEvent(canvas, &pasteEvt));
    QApplication::processEvents();

    ASSERT_EQ(nonLinkAnnotCount(), 2)
        << "the paste did not land: either it threw out of the key handler "
           "(GitHub #170) or it never ran";
    // The pasted copy must carry the contents, which is what proves the
    // restore addressed the annotation it had just created.
    EXPECT_EQ(liveAnnotation(1).contents, "PASTE-ME")
        << "the pasted annotation lost its contents: the restore pass did "
           "not reach the annotation the paste created";
}

TEST_F(AnnotationToolOnLinkedPageTest, AStickyNotePressOverALinkDoesNotNavigate)
{
    ASSERT_EQ(canvas->currentPage(), 0);
    ASSERT_TRUE(mw->activateToolByObjectName("stickyNoteToolAction"));
    QApplication::processEvents();
    ASSERT_NE(canvas->currentTool(), nullptr);

    ASSERT_NO_THROW(canvas->clickByPdfCoord(0, kNamedX, kNamedYDown));
    QApplication::processEvents();
    EXPECT_EQ(canvas->currentPage(), 0)
        << "the Sticky Note tool's press was stolen by the link hit-test";
}

// ───────────────────────────────────────────────────────────────────────────
// GitHub #174 — TextEditTool swallowed link clicks.
//
// #135 established that PdfCanvas::mousePressEvent hands a left-click to the
// active tool and returns, leaving the link hit-test at the bottom of the
// function unreachable; its fix consulted linkFollowRequested before the
// SelectTool press only. TextEditTool is dispatched in an EARLIER branch, so
// a link click while editing text still did nothing.
//
// THE INTERACTION RULE CHOSEN (option 1 of the three #174 lists): follow the
// link only when the click carries no live text span AND no text-edit
// interaction is in progress. It is the routing this same branch already
// performs for image blocks, so it adds no new concept; a modifier would be
// undiscoverable, and declaring it inert would keep the reading case dead.
// Editing always outranks navigation.
//
// The fixture is multipage_smalltext_edit.pdf (3 pages, native text on each)
// with a GoTo link planted in an empty part of page 0. Navigation to page 2
// is the observable effect of a followed link.
// ───────────────────────────────────────────────────────────────────────────

class TextEditLinkClickTest : public ::testing::Test {
protected:
    // Y-down PDF coordinates. The text on this fixture sits near the top of
    // the page (baselines at Y-up 700 and 640 on a 738pt-high page, i.e.
    // Y-down ~40 and ~100), so a rect at Y-down 380..440 is comfortably clear
    // of every span. The "no span here" property is asserted, not assumed.
    static constexpr double kLinkX = 300.0;
    static constexpr double kLinkYDown = 380.0;
    static constexpr double kLinkW = 120.0;
    static constexpr double kLinkH = 60.0;
    static constexpr double kLinkCx = kLinkX + kLinkW / 2.0;
    static constexpr double kLinkCy = kLinkYDown + kLinkH / 2.0;
    // A point with neither a span nor a link on it.
    static constexpr double kBareX = 120.0;
    static constexpr double kBareYDown = 600.0;
    static constexpr int kLinkTargetPage = 2;

    void SetUp() override {
        const auto src = fixturesDir() / "multipage_smalltext_edit.pdf";
        if (!std::filesystem::exists(src)) {
            GTEST_SKIP() << "multipage_smalltext_edit.pdf not available";
        }
        tempDir = std::make_unique<QTemporaryDir>();
        ASSERT_TRUE(tempDir->isValid());
        const auto dir = std::filesystem::path(tempDir->path().toStdString());
        const auto staged = dir / "textedit_link_stage.pdf";
        std::filesystem::copy_file(src, staged);

        {
            auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
            auto doc = pdfe::core::Document::open(ctx, staged);
            auto page = doc->page(0);
            pdfe::annotations::AnnotationManager mgr(ctx);
            auto link = mgr.createAnnotation(
                page.get(), pdfe::annotations::AnnotationType::Link,
                QRectF(kLinkX, kLinkYDown, kLinkW, kLinkH));
            pdfe::annotations::LinkAction act;
            act.type = pdfe::annotations::LinkActionType::GoTo;
            act.pageIndex = kLinkTargetPage;
            mgr.setLinkAction(page.get(), link.annotIndex, act);
            workPdf = dir / "textedit_link_work.pdf";
            doc->saveAs(workPdf);
        }

        mw = std::make_unique<pdfe::ui::MainWindow>();
        mw->openFile(QString::fromStdString(workPdf.string()));
        QApplication::processEvents();
        canvas = mw->findChild<pdfe::ui::PdfCanvas*>("pdfCanvas");
        ASSERT_NE(canvas, nullptr);
        canvas->resize(1048, 885);
        canvas->setZoom(1.0f);
        QApplication::processEvents();
        canvas->goToPage(0);
        QApplication::processEvents();
    }

    void TearDown() override {
        mw.reset();
        QApplication::processEvents();
        tempDir.reset();
    }

    // Arm the text tool and return it, with every precondition this suite
    // depends on asserted rather than hoped for.
    pdfe::textedit::TextEditTool* armTextEditTool() {
        if (!mw->activateToolByObjectName("textEditToolAction")) {
            ADD_FAILURE() << "textEditToolAction did not activate";
            return nullptr;
        }
        QApplication::processEvents();
        auto* tool = canvas->textEditTool();
        if (!tool) {
            ADD_FAILURE()
                << "vacuity guard: the page did not route to TextEditTool, so "
                   "no test here is exercising the branch #174 is about";
            return nullptr;
        }
        tool->setPage(0);
        return tool;
    }

    std::unique_ptr<QTemporaryDir> tempDir;
    std::filesystem::path workPdf;
    std::unique_ptr<pdfe::ui::MainWindow> mw;
    pdfe::ui::PdfCanvas* canvas = nullptr;
};

// THE DEFECT. With the text tool active, a click on a link that has no live
// text span under it must follow the link. Before the fix this click was
// consumed by TextEditTool's early-return branch and nothing happened.
TEST_F(TextEditLinkClickTest, ALinkClickWithNoSpanUnderItIsFollowedWhileEditing)
{
    auto* tool = armTextEditTool();
    ASSERT_NE(tool, nullptr);
    ASSERT_GT(tool->spanCountForTesting(), 0)
        << "vacuity guard: the fixture page must carry live text spans, "
           "otherwise this is not the text-edit branch at all";
    ASSERT_LT(tool->spanIndexAtPoint(QPointF(kLinkCx, kLinkCy)), 0)
        << "vacuity guard: the planted link must sit where NO span is, or "
           "this test is measuring the span rule instead of the link rule";
    ASSERT_FALSE(tool->hasActiveSpan());
    ASSERT_EQ(canvas->currentPage(), 0);
    ASSERT_NE(canvas->currentPage(), kLinkTargetPage);

    EXPECT_TRUE(canvas->clickByPdfCoord(0, kLinkCx, kLinkCy));
    QApplication::processEvents();
    EXPECT_EQ(canvas->currentPage(), kLinkTargetPage)
        << "GitHub #174: the text tool swallowed the link click";
}

// CONTROL — the direction the fix could over-reach in, and the one the phase
// criterion names: an in-progress text-edit interaction must WIN over a link
// click. Same point as the test above, same link, but with a span active.
TEST_F(TextEditLinkClickTest, AnInProgressEditOutranksTheSameLinkClick)
{
    auto* tool = armTextEditTool();
    ASSERT_NE(tool, nullptr);
    ASSERT_GT(tool->spanCountForTesting(), 0);
    ASSERT_TRUE(tool->selectSpanForTesting(0))
        << "vacuity guard: span 0 could not be selected, so no edit is in "
           "progress and this control would trivially pass";
    ASSERT_TRUE(tool->hasActiveSpan());
    ASSERT_EQ(canvas->currentPage(), 0);

    EXPECT_TRUE(canvas->clickByPdfCoord(0, kLinkCx, kLinkCy));
    QApplication::processEvents();
    EXPECT_EQ(canvas->currentPage(), 0)
        << "a link click stole a press that belonged to an in-progress "
           "text edit";
}

// CONTROL — a click that lands ON a live text span belongs to the editor even
// if a link covers the entire page. The stub returns true for every point, so
// only the span rule can keep this click; a fix that consulted the link first
// would navigate here.
TEST_F(TextEditLinkClickTest, AClickOnALiveSpanIsNeverStolenByALink)
{
    auto* tool = armTextEditTool();
    ASSERT_NE(tool, nullptr);
    ASSERT_GT(tool->spanCountForTesting(), 0);
    const QRectF bbox = tool->spanBboxForTesting(0);
    ASSERT_FALSE(bbox.isEmpty());
    const QPointF spanPt = bbox.center();
    ASSERT_GE(tool->spanIndexAtPoint(spanPt), 0)
        << "vacuity guard: the span bbox centre must hit-test to a span";

    int calls = 0;
    canvas->linkFollowRequested = [&calls](int, QPointF) {
        ++calls;
        return true;   // "there is a link here" — for every point on the page
    };

    EXPECT_TRUE(canvas->clickByPdfCoord(0, spanPt.x(), spanPt.y()));
    QApplication::processEvents();
    EXPECT_EQ(calls, 0)
        << "the link hit-test was consulted for a click that landed on a "
           "live text span";
    EXPECT_GE(tool->activeSpanIndexForTesting(), 0)
        << "the click did not reach the text tool either — it went nowhere";

    // CONTROL OF THE CONTROL: the same always-true stub IS consulted at a
    // point with no span under it. Without this, a stub that was simply never
    // wired up would satisfy the assertion above.
    tool->cancelEdit();
    QApplication::processEvents();
    ASSERT_FALSE(tool->hasActiveSpan());
    ASSERT_LT(tool->spanIndexAtPoint(QPointF(kBareX, kBareYDown)), 0);
    EXPECT_TRUE(canvas->clickByPdfCoord(0, kBareX, kBareYDown));
    QApplication::processEvents();
    EXPECT_EQ(calls, 1)
        << "the link hit-test was not reachable at all from this branch, so "
           "the zero above proved nothing";
}

// CONTROL — separates "links work while editing" from "any click navigates".
TEST_F(TextEditLinkClickTest, AClickOnNeitherSpanNorLinkDoesNotNavigate)
{
    auto* tool = armTextEditTool();
    ASSERT_NE(tool, nullptr);
    ASSERT_LT(tool->spanIndexAtPoint(QPointF(kBareX, kBareYDown)), 0);
    ASSERT_EQ(canvas->currentPage(), 0);

    EXPECT_TRUE(canvas->clickByPdfCoord(0, kBareX, kBareYDown));
    QApplication::processEvents();
    EXPECT_EQ(canvas->currentPage(), 0);
}

// CONTROL — the planted link is genuinely followable by the already-fixed
// SelectTool path. If this ever fails, the text-edit tests above are failing
// (or passing) for a reason that has nothing to do with #174.
TEST_F(TextEditLinkClickTest, TheSameLinkIsFollowedByTheSelectToolPath)
{
    ASSERT_NE(canvas->currentTool(), nullptr)
        << "opening a document installs the Select tool";
    ASSERT_EQ(canvas->textEditTool(), nullptr);
    ASSERT_EQ(canvas->currentPage(), 0);

    EXPECT_TRUE(canvas->clickByPdfCoord(0, kLinkCx, kLinkCy));
    QApplication::processEvents();
    EXPECT_EQ(canvas->currentPage(), kLinkTargetPage)
        << "the planted GoTo link is not followable at all — every #174 "
           "assertion above is built on it";
}
