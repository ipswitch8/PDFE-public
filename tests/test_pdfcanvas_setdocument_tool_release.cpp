// -*- coding: utf-8 -*-
//
// Regression for #47: SelectTool (and any AnnotationTool caching a raw Page*)
// dangles when the document's Page objects are recreated under it, and a queued
// repaint then dereferences the freed page in SelectTool::onPaintOverlay →
// AnnotationManager::getAnnotation(page_) → Page::fzPage → 0xC0000005 (signature
// B, windows-followups P2, docs/audit/teardown-av-confirmation-2026-07-29.md).
//
// TWO code paths recreate/replace the document's Pages:
//   1. In-session full-rewrite SAVE — the ACTUAL crash path. saveAsWithCommandPageRebind
//      → MainWindow::onDocumentStructureChanged() recreates the Pages while the
//      document stays open, but historically dropped only toolPage_/commandPages_,
//      NOT the active tool's own SelectTool::page_. Fixed by having
//      onDocumentStructureChanged call PdfCanvas::releaseActiveToolPageCache().
//   2. Save→REOPEN via setDocument() — already covered on close by clearDocument()
//      (openFile() calls closeDocument()→clearDocument() before setDocument), so
//      this was defensive; setDocument() now also releases on a document swap.
// These tests exercise BOTH: releaseActiveToolPageCache() (path 1, the real fix)
// and setDocument() (path 2, defensive hardening).

#include <gtest/gtest.h>

#include "ui/PdfCanvas.h"
#include "annotations/AnnotationTool.h"
#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationType.h"
#include "annotations/SelectTool.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"
#include "rendering/TileCache.h"
#include "rendering/TileId.h"

#include <QApplication>
#include <QCoreApplication>
#include <QImage>
#include <QPainter>

#include <filesystem>
#include <fstream>
#include <memory>
#include <ostream>
#include <string>

using namespace pdfe;

namespace {

// A minimal spy tool that records how many times the document-closing
// notification fired. The other AnnotationTool virtuals are pure, so give them
// trivial bodies.
//
// GitHub #236: this counts onDocumentClosingImpl(), not onDocumentClosing().
// The latter is now NON-VIRTUAL -- it releases the base-owned `page_` and then
// calls this hook -- specifically so that a tool cannot skip the release by
// overriding it, which is how ten tools ended up holding freed pages. The
// assertion below is unchanged in meaning: the hook fires exactly once per
// notification, so every count in this file still measures what it did.
class SpyTool : public annotations::AnnotationTool {
public:
    int closingCalls = 0;
    annotations::AnnotationType toolType() const override {
        return annotations::AnnotationType::Highlight;
    }
    void onMousePress(QMouseEvent*, int, QPointF) override {}
    void onMouseMove(QMouseEvent*, int, QPointF) override {}
    void onMouseRelease(QMouseEvent*, int, QPointF) override {}

protected:
    void onDocumentClosingImpl() override { ++closingCalls; }
};

std::filesystem::path fixture(const char* name) {
    return std::filesystem::path(TEST_FIXTURES_DIR) / name;
}

} // namespace

// The core wiring fix: swapping the document via setDocument() must notify the
// active tool that its cached document is going away (so it releases any raw
// Page*), exactly as clearDocument() does. Pre-fix this count stayed 0.
TEST(PdfCanvasSetDocumentToolRelease, SetDocumentNotifiesActiveToolOnSwap)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    auto ctx = std::make_shared<core::MuPdfContext>();
    if (!std::filesystem::exists(fixture("test_single.pdf")))
        GTEST_SKIP() << "fixture missing";

    auto docA = core::Document::open(ctx, fixture("test_single.pdf"));
    auto docB = core::Document::open(ctx, fixture("test_single.pdf"));
    ASSERT_NE(docA, nullptr);
    ASSERT_NE(docB, nullptr);

    ui::PdfCanvas canvas;
    canvas.setDocument(docA.get(), nullptr, nullptr);

    auto spy = std::make_unique<SpyTool>();
    SpyTool* spyRaw = spy.get();
    canvas.setCurrentTool(std::move(spy));
    ASSERT_EQ(spyRaw->closingCalls, 0);

    // The save→reopen swap.
    canvas.setDocument(docB.get(), nullptr, nullptr);
    EXPECT_EQ(spyRaw->closingCalls, 1)
        << "setDocument() must release the active tool's cached page state on a "
           "document swap (reopen), like clearDocument() does on close";

    // Re-setting the SAME document must NOT re-fire (no spurious release).
    canvas.setDocument(docB.get(), nullptr, nullptr);
    EXPECT_EQ(spyRaw->closingCalls, 1)
        << "setDocument() with the same document must not re-release";
}

// Path 1 (the REAL crash path): after a full-rewrite Save recreates the Pages,
// MainWindow::onDocumentStructureChanged() calls releaseActiveToolPageCache().
// This must notify the active tool to drop its cached page. Pre-fix (no such
// call) the tool kept its stale page_ and a repaint crashed.
TEST(PdfCanvasSetDocumentToolRelease, ReleaseActiveToolPageCacheNotifiesTool)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    auto ctx = std::make_shared<core::MuPdfContext>();
    if (!std::filesystem::exists(fixture("test_single.pdf")))
        GTEST_SKIP() << "fixture missing";

    auto doc = core::Document::open(ctx, fixture("test_single.pdf"));
    ASSERT_NE(doc, nullptr);

    ui::PdfCanvas canvas;
    canvas.setDocument(doc.get(), nullptr, nullptr);

    auto spy = std::make_unique<SpyTool>();
    SpyTool* spyRaw = spy.get();
    canvas.setCurrentTool(std::move(spy));
    ASSERT_EQ(spyRaw->closingCalls, 0);

    canvas.releaseActiveToolPageCache();
    EXPECT_EQ(spyRaw->closingCalls, 1)
        << "releaseActiveToolPageCache() (called by onDocumentStructureChanged "
           "after a full-rewrite save recreates the Pages) must release the "
           "active tool's cached page";
}

// Path 1 end-to-end: bind a real SelectTool to a live selection, then simulate
// the save recreating the Pages (releaseActiveToolPageCache) and free the old
// page; a paint must be page-free. Pre-fix the SelectTool kept page_ pointing at
// the freed page and onPaintOverlay → getAnnotation(page_) faulted.
TEST(PdfCanvasSetDocumentToolRelease, SelectToolPaintIsPageFreeAfterReleaseCache)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    auto ctx = std::make_shared<core::MuPdfContext>();
    if (!std::filesystem::exists(fixture("test_single.pdf")))
        GTEST_SKIP() << "fixture missing";

    auto doc = core::Document::open(ctx, fixture("test_single.pdf"));
    ASSERT_NE(doc, nullptr);
    auto manager = std::make_unique<annotations::AnnotationManager>(ctx);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);
    manager->createAnnotation(page.get(), annotations::AnnotationType::Highlight,
                              QRectF(50, 50, 100, 20));
    auto annots = manager->getAnnotations(page.get());
    ASSERT_GT(annots.size(), 0u);
    const int annotIndex = static_cast<int>(annots.size()) - 1;

    ui::PdfCanvas canvas;
    canvas.setDocument(doc.get(), nullptr, nullptr);
    auto sel = std::make_unique<annotations::SelectTool>(manager.get());
    annotations::SelectTool* selRaw = sel.get();
    selRaw->setPage(page.get());
    canvas.setCurrentTool(std::move(sel));
    canvas.selectAnnotationInSelectTool(0, annotIndex);

    // The save recreates the Pages: release the tool's cached page, then free it.
    canvas.releaseActiveToolPageCache();
    page.reset();

    QImage img(200, 200, QImage::Format_ARGB32);
    img.fill(Qt::white);
    {
        QPainter painter(&img);
        EXPECT_NO_FATAL_FAILURE(
            selRaw->onPaintOverlay(painter, /*pageIndex=*/0, /*zoom=*/1.0f,
                                   QPointF(0, 0), /*pageHeightPt=*/800.0f));
    }
}

// End-to-end reproduction of signature B: bind a SelectTool to docA's page with
// a live selection, perform the reopen swap (setDocument(docB)), then FREE docA.
// A subsequent paint must not touch docA's freed page. Pre-fix, page_ still
// pointed into freed docA and onPaintOverlay → getAnnotation(page_) →
// Page::fzPage faulted (or tripped ASan); post-fix the swap nulled page_ so the
// multi-selection paint branch is skipped and the paint is page-free.
TEST(PdfCanvasSetDocumentToolRelease, SelectToolPaintIsPageFreeAfterReopenSwap)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    auto ctx = std::make_shared<core::MuPdfContext>();
    if (!std::filesystem::exists(fixture("test_single.pdf")))
        GTEST_SKIP() << "fixture missing";

    auto docA = core::Document::open(ctx, fixture("test_single.pdf"));
    auto docB = core::Document::open(ctx, fixture("test_single.pdf"));
    ASSERT_NE(docA, nullptr);
    ASSERT_NE(docB, nullptr);

    auto manager = std::make_unique<annotations::AnnotationManager>(ctx);
    auto pageA = docA->page(0);
    ASSERT_NE(pageA, nullptr);

    // Give docA an annotation so SelectTool can hold a live multi-selection.
    manager->createAnnotation(pageA.get(), annotations::AnnotationType::Highlight,
                              QRectF(50, 50, 100, 20));
    auto annots = manager->getAnnotations(pageA.get());
    ASSERT_GT(annots.size(), 0u);
    const int annotIndex = static_cast<int>(annots.size()) - 1;

    ui::PdfCanvas canvas;
    canvas.setDocument(docA.get(), nullptr, nullptr);

    auto sel = std::make_unique<annotations::SelectTool>(manager.get());
    annotations::SelectTool* selRaw = sel.get();
    selRaw->setPage(pageA.get());
    canvas.setCurrentTool(std::move(sel));
    // Bind a live selection on page 0 (populates page_ + selectedAnnotIndices_).
    canvas.selectAnnotationInSelectTool(0, annotIndex);

    // The reopen swap — must release the tool's cached docA page.
    canvas.setDocument(docB.get(), nullptr, nullptr);

    // Now docA (and pageA) go away entirely.
    pageA.reset();
    docA.reset();

    // A paint after the swap+free must be page-free (no deref of freed docA).
    // Without the fix this dereferences pageA's freed fz_page.
    QImage img(200, 200, QImage::Format_ARGB32);
    img.fill(Qt::white);
    {
        QPainter painter(&img);
        EXPECT_NO_FATAL_FAILURE(
            selRaw->onPaintOverlay(painter, /*pageIndex=*/0, /*zoom=*/1.0f,
                                   QPointF(0, 0), /*pageHeightPt=*/800.0f));
    }
}

// ═════════════════════════════════════════════════════════════════════
// CLASS-LEVEL TEST for DCA 0008 (follow-up phase 42).
//
// The four INSTANCE tests above each hard-code ONE code path. That is exactly
// the shape of the defect they document: DCA 0008's own causal analysis names
// the class as *"a resource-release invariant enforced on only SOME of the code
// paths that invalidate the resource"*, and the earlier 8f3a497 fix shipped
// having covered clearDocument() alone. A per-path test is a per-path fix
// written twice; it cannot tell you a path is missing, because a path nobody
// thought of is also a test nobody wrote.
//
// THE CLASS-LEVEL FORM is to enumerate the invalidating paths as DATA and
// assert the invariant over the enumeration. Every PdfCanvas entry point that
// invalidates the document's Page objects is a row here, and each row must:
//
//   1. notify the active tool (onDocumentClosing fires exactly once), AND
//   2. leave a real SelectTool with a live selection PAGE-FREE, so a repaint
//      after the underlying Page is destroyed touches nothing freed.
//
// The row count is asserted, so a fourth invalidating path added to PdfCanvas
// without a row here fails the guard instead of silently inheriting coverage.
//
// RED-ness: remove the `currentTool_->onDocumentClosing()` call from any ONE of
// PdfCanvas::setDocument / releaseActiveToolPageCache / clearDocument and
// exactly that row fails, in both halves — while the sibling rows stay green,
// which is precisely the asymmetry the instance tests could not see.

namespace {

// How the canvas is told the document's Pages are going away.
enum class InvalidationPath {
    ClearDocument,             // close  — MainWindow::closeDocument()
    SetDocumentSwap,           // reopen — MainWindow::openFile() → setDocument()
    ReleaseActiveToolPageCache // in-session full-rewrite Save (the real #47 path)
};

struct InvalidationPathParam {
    InvalidationPath path;
    const char* name;
};

std::ostream& operator<<(std::ostream& os, const InvalidationPathParam& p) {
    return os << p.name;
}

class PdfCanvasPageInvalidationPaths
    : public ::testing::TestWithParam<InvalidationPathParam> {};

} // namespace

TEST_P(PdfCanvasPageInvalidationPaths, ReleasesActiveToolPageCache)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    auto ctx = std::make_shared<core::MuPdfContext>();
    if (!std::filesystem::exists(fixture("test_single.pdf")))
        GTEST_SKIP() << "fixture missing";

    const InvalidationPath path = GetParam().path;
    const char* pathName = GetParam().name;

    auto docA = core::Document::open(ctx, fixture("test_single.pdf"));
    auto docB = core::Document::open(ctx, fixture("test_single.pdf"));
    ASSERT_NE(docA, nullptr);
    ASSERT_NE(docB, nullptr);

    // Applies `path` to a canvas whose Pages are about to be invalidated.
    auto applyPath = [&](ui::PdfCanvas& canvas) {
        switch (path) {
        case InvalidationPath::ClearDocument:
            canvas.clearDocument();
            break;
        case InvalidationPath::SetDocumentSwap:
            canvas.setDocument(docB.get(), nullptr, nullptr);
            break;
        case InvalidationPath::ReleaseActiveToolPageCache:
            canvas.releaseActiveToolPageCache();
            break;
        }
    };

    // ---- 1. The wiring half: the active tool is notified, exactly once. ----
    {
        ui::PdfCanvas canvas;
        canvas.setDocument(docA.get(), nullptr, nullptr);
        auto spy = std::make_unique<SpyTool>();
        SpyTool* spyRaw = spy.get();
        canvas.setCurrentTool(std::move(spy));
        ASSERT_EQ(spyRaw->closingCalls, 0);

        applyPath(canvas);

        EXPECT_EQ(spyRaw->closingCalls, 1)
            << pathName << " invalidates the document's Page objects but did "
               "NOT notify the active tool to release its cached raw Page*. "
               "The tool keeps a dangling page_ and the next repaint faults "
               "(DCA 0008 — the invariant must hold on EVERY invalidating "
               "path, not just the ones someone wrote a test for).";
    }

    // ---- 2. The end-to-end half: a live selection survives the free. ----
    {
        auto manager = std::make_unique<annotations::AnnotationManager>(ctx);
        auto pageA = docA->page(0);
        ASSERT_NE(pageA, nullptr);
        manager->createAnnotation(pageA.get(), annotations::AnnotationType::Highlight,
                                  QRectF(50, 50, 100, 20));
        auto annots = manager->getAnnotations(pageA.get());
        ASSERT_GT(annots.size(), 0u);
        const int annotIndex = static_cast<int>(annots.size()) - 1;

        ui::PdfCanvas canvas;
        canvas.setDocument(docA.get(), nullptr, nullptr);
        auto sel = std::make_unique<annotations::SelectTool>(manager.get());
        annotations::SelectTool* selRaw = sel.get();
        selRaw->setPage(pageA.get());
        canvas.setCurrentTool(std::move(sel));
        canvas.selectAnnotationInSelectTool(0, annotIndex);

        applyPath(canvas);

        // The Page the tool had cached is now destroyed for real.
        pageA.reset();

        QImage img(200, 200, QImage::Format_ARGB32);
        img.fill(Qt::white);
        {
            QPainter painter(&img);
            EXPECT_NO_FATAL_FAILURE(
                selRaw->onPaintOverlay(painter, /*pageIndex=*/0, /*zoom=*/1.0f,
                                       QPointF(0, 0), /*pageHeightPt=*/800.0f))
                << pathName << ": repaint after this path dereferenced the "
                   "freed Page (DCA 0008)";
        }
    }
}

INSTANTIATE_TEST_SUITE_P(
    AllPageInvalidatingPaths, PdfCanvasPageInvalidationPaths,
    ::testing::Values(
        InvalidationPathParam{InvalidationPath::ClearDocument, "clearDocument"},
        InvalidationPathParam{InvalidationPath::SetDocumentSwap, "setDocumentSwap"},
        InvalidationPathParam{InvalidationPath::ReleaseActiveToolPageCache,
                              "releaseActiveToolPageCache"}));

// The enumeration above is the whole point of the class-level form, so its SIZE
// has to be pinned against something that can actually disagree with it.
// Comparing two literals written side by side would be decoration — no change
// to the product could ever make it red. So this reads PdfCanvas.cpp and counts
// the REAL release sites: every `currentTool_->onDocumentClosing()` call. Add a
// fourth invalidating path to PdfCanvas without adding a row to
// INSTANTIATE_TEST_SUITE_P above and this goes red, which is the only way the
// parametrised test can warn about a path nobody thought of — the precise way
// DCA 0008 escaped (8f3a497 released on clearDocument() alone).
TEST(PdfCanvasSetDocumentToolRelease, PageInvalidatingPathEnumerationIsComplete)
{
    const std::filesystem::path canvasCpp =
        std::filesystem::path(PDFE_REPO_ROOT) / "src" / "ui" / "PdfCanvas.cpp";
    ASSERT_TRUE(std::filesystem::exists(canvasCpp))
        << "cannot find " << canvasCpp.string()
        << " — this guard must fail closed, not pass silently";

    std::ifstream in(canvasCpp, std::ios::binary);
    ASSERT_TRUE(in.good()) << "cannot read " << canvasCpp.string();

    // Count non-commented `currentTool_->onDocumentClosing()` call sites.
    int releaseSites = 0;
    std::string line;
    while (std::getline(in, line)) {
        const auto firstNonSpace = line.find_first_not_of(" \t\r");
        if (firstNonSpace != std::string::npos &&
            line.compare(firstNonSpace, 2, "//") == 0) {
            continue;  // a comment line — including the explanatory ones above
        }
        if (line.find("currentTool_->onDocumentClosing()") != std::string::npos) {
            ++releaseSites;
        }
    }

    // Rows in INSTANTIATE_TEST_SUITE_P(AllPageInvalidatingPaths, ...).
    constexpr int kEnumeratedPaths = 3;
    EXPECT_EQ(releaseSites, kEnumeratedPaths)
        << "PdfCanvas.cpp has " << releaseSites << " call sites that release "
           "the active tool's cached page, but the class-level parametrised "
           "test enumerates " << kEnumeratedPaths << ". A path present in the "
           "product but absent from the enumeration is untested — that is "
           "exactly how DCA 0008 escaped. Add the missing row (and update this "
           "count), or remove the stale one.";
}

// ── Issue #125: the canvas subscribes, so markPageDirty() actually repaints ──
//
// Before this, Document::markPageDirty() inserted into a set nothing consulted.
// The only thing standing between an image edit and a stale canvas was a
// hand-written PdfCanvas::refreshPage() at each MainWindow call site — and
// "tile cache not invalidated -> content edits not visually shown" is a bug
// this project has shipped four times (docs/audit/seed-catalog-git.md
// G-02/G-03/G-10/G-11; G-03's fix is 09da7098).
//
// These tests drive the wiring WITHOUT MainWindow, which is not
// gtest-instantiable. That is the point: if the only thing that invalidates the
// cache is a MainWindow call site, these tests cannot pass.

namespace {

pdfe::rendering::TileId tileIdFor(int pageIndex) {
    pdfe::rendering::TileId id;
    id.pageIndex = pageIndex;
    id.col = 0;
    id.row = 0;
    id.zoom = 1.0f;
    return id;
}

QImage solidTile() {
    QImage img(64, 64, QImage::Format_ARGB32);
    img.fill(Qt::white);
    return img;
}

}  // namespace

TEST(PdfCanvasPageDirty, MarkPageDirtyInvalidatesThatPagesTilesThroughTheCanvas)
{
    auto ctx = std::make_shared<core::MuPdfContext>();
    ASSERT_TRUE(std::filesystem::exists(fixture("test_3page.pdf")));
    auto doc = core::Document::open(ctx, fixture("test_3page.pdf"));
    ASSERT_NE(doc, nullptr);

    pdfe::rendering::TileCache cache;
    ui::PdfCanvas canvas;
    canvas.setDocument(doc.get(), &cache, nullptr);

    const auto edited = tileIdFor(1);
    const auto untouched = tileIdFor(2);
    cache.put(edited, solidTile());
    cache.put(untouched, solidTile());
    ASSERT_TRUE(cache.contains(edited));
    ASSERT_TRUE(cache.contains(untouched));

    // The edit. No MainWindow, no explicit refreshPage() — just the document
    // announcing that a page changed, which is all a new edit path would do.
    doc->markPageDirty(1);
    QCoreApplication::processEvents();  // drain, in case the hop was queued

    EXPECT_FALSE(cache.contains(edited))
        << "markPageDirty() did not reach the tile cache — the canvas would go "
           "on painting the pre-edit tiles for this page, which is exactly the "
           "stale-canvas class this issue exists to close";
    EXPECT_TRUE(cache.contains(untouched))
        << "a dirty page invalidated an UNRELATED page's tiles — the page index "
           "is not being carried through, so every edit costs a full re-render";
}

TEST(PdfCanvasPageDirty, CanvasStopsListeningToADocumentItNoLongerShows)
{
    auto ctx = std::make_shared<core::MuPdfContext>();
    ASSERT_TRUE(std::filesystem::exists(fixture("test_3page.pdf")));
    auto docA = core::Document::open(ctx, fixture("test_3page.pdf"));
    auto docB = core::Document::open(ctx, fixture("test_3page.pdf"));
    ASSERT_NE(docA, nullptr);
    ASSERT_NE(docB, nullptr);

    pdfe::rendering::TileCache cache;
    ui::PdfCanvas canvas;
    canvas.setDocument(docA.get(), &cache, nullptr);

    // The save->reopen swap.
    canvas.setDocument(docB.get(), &cache, nullptr);

    const auto id = tileIdFor(1);
    cache.put(id, solidTile());
    ASSERT_TRUE(cache.contains(id));

    // The OLD document must no longer be able to reach this canvas. If the
    // subscription survived the swap, docA still holds a callback bound to a
    // canvas that has moved on — and once docA outlives the canvas entirely
    // that same stale token is a call into freed memory.
    docA->markPageDirty(1);
    QCoreApplication::processEvents();
    EXPECT_TRUE(cache.contains(id))
        << "a document the canvas no longer shows still invalidated its cache — "
           "the subscription was not released on the document swap";

    // ...and the NEW one must.
    docB->markPageDirty(1);
    QCoreApplication::processEvents();
    EXPECT_FALSE(cache.contains(id))
        << "the canvas did not subscribe to the document it swapped TO";
}

TEST(PdfCanvasPageDirty, DestroyedCanvasLeavesNoListenerBehind)
{
    auto ctx = std::make_shared<core::MuPdfContext>();
    ASSERT_TRUE(std::filesystem::exists(fixture("test_3page.pdf")));
    auto doc = core::Document::open(ctx, fixture("test_3page.pdf"));
    ASSERT_NE(doc, nullptr);

    pdfe::rendering::TileCache cache;
    {
        ui::PdfCanvas canvas;
        canvas.setDocument(doc.get(), &cache, nullptr);
    }  // canvas destroyed; document outlives it, which is the normal ownership order

    const auto id = tileIdFor(0);
    cache.put(id, solidTile());
    ASSERT_TRUE(cache.contains(id));

    // Under a missing unsubscribe this is a use-after-free rather than a clean
    // assertion failure — the destroyed canvas's `this` is still captured in a
    // live listener. It may crash instead of reddening; either is a fail, and
    // both are better than the silence there was before.
    EXPECT_NO_THROW(doc->markPageDirty(0));
    QCoreApplication::processEvents();
    EXPECT_TRUE(cache.contains(id))
        << "a destroyed canvas still invalidated the cache — its listener "
           "outlived it, so Document is calling into freed memory";
}
