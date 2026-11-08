// -*- coding: utf-8 -*-
//
// GitHub #346 — inserted blank page "duplicates on first move, then merges".
//
// WHAT THE SYMPTOM ACTUALLY IS (corrected after phase-3 claim-check + a raw
// MuPDF probe — see docs/audit/many-bugs-2026-11-investigation.md #346):
//
// The original hypothesis was a DOCUMENT-level defect: that after
// insertBlankPage() (pdf_insert_page) + graftPages() (pdf_graft_mapped_page)
// MuPDF's page map would resolve two different page indices to the SAME
// pdf_obj*, so Document::permutePageTreeInPlace()'s up-front resolve would push
// one shared object into the rebuilt /Kids twice (the "duplicate", then "merge").
//
// That is NOT reproducible in MuPDF 1.26.10. A raw-MuPDF probe replayed the
// real-app ordering — WARM the page map (look up every page), THEN insert +
// graft, THEN resolve indices 0..N-1 exactly as the fast path does — and got N
// DISTINCT objects, no aliasing, no throw, both warm and cold. pdf_insert_page
// mutates /Kids directly and never drops the cached fwd/rev_page_map
// (pdf-page.c), but an out-of-range lookup throws rather than aliasing, and the
// map is reloaded fresh (at the correct post-edit count) before any reorder
// resolves it. The distinct-object guard added in permutePageTreeInPlace is
// therefore DEFENSE-IN-DEPTH (a valid page tree never aliases; it bails to the
// general path if one ever did), not the fix for a reachable state in 1.26.10.
//
// The user-visible "duplicate then merge" is a THUMBNAIL artifact: a move
// renumbers pages, and the Pages-panel thumbnail cache is keyed by page index,
// so without a cache flush the moved page's bitmap is served at BOTH its old and
// new index (the "duplicate") until a later full re-render corrects it (the
// "merge"). That reachable defect is closed by refreshPages()' full cache flush
// on any reorder (GitHub #336, PagePanel.cpp) — which the forward move path
// reaches via MainWindow::onPageMoved/onMovePage* → onDocumentStructureChanged →
// refreshPages(). This file pins BOTH facts:
//
//   1. MoveAfterInsertAndGraftProducesCorrectPermutation — the document stays a
//      correct permutation (every /Kids entry a distinct object, the uniquely
//      sized blank present exactly once at its new index). This protects
//      permutePageTreeInPlace()'s REBUILD; it is the characterization test that
//      would redden if the fast path ever regressed to emitting a duplicate.
//
//   2. InsertedPageMoveRefreshesThumbnailsNoStaleDuplicate — drives the REAL
//      MainWindow insert-blank + move path and asserts the panel re-rasterizes
//      (the reachable #346 fix). Coupled to #336's invalidate(): remove the
//      cache flush and the moved page is served stale from the cache and this
//      goes red (renderCount stays 0).

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "ui/MainWindow.h"
#include "ui/PagePanel.h"
#include "rendering/ThumbnailRenderer.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <QAction>
#include <QApplication>
#include <QEventLoop>
#include <QRectF>
#include <QString>
#include <QTimer>

#include <cmath>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

using pdfe::core::Document;
using pdfe::core::MuPdfContext;
using pdfe::ui::MainWindow;
using pdfe::ui::PagePanel;

namespace {

std::filesystem::path fixture(const std::string& name)
{
    return std::filesystem::path(TEST_FIXTURES_DIR) / name;
}

// The blank page is created at a size no other page in the document has, so it
// is a unique, save-surviving identity we can follow across the reorder.
constexpr float kBlankW = 200.0f;
constexpr float kBlankH = 400.0f;

bool isBlankSize(const QRectF& box)
{
    return std::abs(box.width()  - kBlankW) < 1.0 &&
           std::abs(box.height() - kBlankH) < 1.0;
}

void settle()
{
    QApplication::processEvents();
    QApplication::processEvents();
}

// Pump the event loop until renderCount() has been stable for `quietMs`, or
// until `timeoutMs` elapses (thumbnail renders are async on a worker pool).
// Same oracle + drain strategy as test_page_thumbnail_refresh_undo_insert.cpp
// and the #336 cache-flush test.
void pumpUntilRendersSettle(int quietMs = 300, int timeoutMs = 15000)
{
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);

    std::size_t last = pdfe::rendering::ThumbnailRenderer::renderCount();
    int stableFor = 0;

    QTimer poll;
    QObject::connect(&poll, &QTimer::timeout, [&]() {
        const std::size_t now = pdfe::rendering::ThumbnailRenderer::renderCount();
        if (now != last) {
            last = now;
            stableFor = 0;
        } else {
            stableFor += 25;
            if (stableFor >= quietMs) loop.quit();
        }
    });
    poll.start(25);
    deadline.start(timeoutMs);
    loop.exec();
}

} // namespace

// Document-level characterization: insert a blank page, graft a page from a
// second document, move the blank once, and require a correct permutation with
// no shared /Kids objects. (The aliasing the distinct-object guard defends
// against is not reproducible here in 1.26.10 — see the file header — so this
// pins permutePageTreeInPlace's rebuild correctness, not the guard's bail.)
TEST(PageInsertGraftMoveDistinct, MoveAfterInsertAndGraftProducesCorrectPermutation)
{
    const auto pdfA = fixture("test_3page.pdf");
    if (!std::filesystem::exists(pdfA))
        GTEST_SKIP() << "test_3page.pdf fixture unavailable";

    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, pdfA);
    ASSERT_NE(doc, nullptr);
    ASSERT_EQ(doc->pageCount(), 3) << "this test assumes the 3-page fixture";

    // 1) Insert a uniquely-sized blank page at index 1. Order: [o0, BLANK, o1, o2]
    doc->insertBlankPage(1, kBlankW, kBlankH, 0);
    ASSERT_EQ(doc->pageCount(), 4);

    // 2) Graft a page from a SECOND document at the end (the "merge" step).
    //    Order: [o0, BLANK, o1, o2, gB0]
    auto src = Document::open(ctx, pdfA);  // a second, independent document
    ASSERT_NE(src, nullptr);
    doc->graftPages(4, *src, 0, 0);
    ASSERT_EQ(doc->pageCount(), 5);

    // 3) Move the BLANK page (index 1) to index 3 — a single full-permutation
    //    reorder, the operation the user reported. Desired new order
    //    [o0, o1, o2, BLANK, gB0] => old indices:
    const std::vector<int> newOrder = {0, 2, 3, 1, 4};
    doc->rearrangePages(newOrder);

    ASSERT_EQ(doc->pageCount(), 5)
        << "the move changed the page count — permutation rebuild corruption";

    // (a) LIVE in-memory oracle: the uniquely-sized blank appears EXACTLY ONCE
    //     and at its expected new index.
    int blankCount = 0;
    int blankIndex = -1;
    for (int i = 0; i < doc->pageCount(); ++i) {
        auto page = doc->page(i);
        ASSERT_NE(page, nullptr);
        if (isBlankSize(page->boundingBox())) {
            ++blankCount;
            blankIndex = i;
        }
    }
    EXPECT_EQ(blankCount, 1)
        << "the inserted blank page is " << (blankCount == 0 ? "MISSING (merged)"
                                                             : "DUPLICATED")
        << " after the move (GitHub #346)";
    EXPECT_EQ(blankIndex, 3)
        << "the blank page did not land at its expected post-move index";

    // (b) save + reopen and confirm every /Kids entry is a distinct page object.
    const auto out = std::filesystem::temp_directory_path() /
                     "pdfe_test_346_insert_graft_move.pdf";
    std::error_code ec;
    std::filesystem::remove(out, ec);
    doc->saveAs(out);
    ASSERT_TRUE(std::filesystem::exists(out));

    const std::string outStr = out.string();
    std::vector<int> pageObjNums(5, 0);  // sized/owned OUTSIDE the fz_try frame
    int reopenedCount = 0;
    ctx->execute([&](fz_context* c) {
        pdf_document* d = pdf_open_document(c, outStr.c_str());
        fz_try(c) {
            reopenedCount = pdf_count_pages(c, d);
            const int n = std::min(reopenedCount, 5);
            for (int i = 0; i < n; ++i) {
                pageObjNums[i] = pdf_to_num(c, pdf_lookup_page_obj(c, d, i));
            }
        }
        fz_always(c) {
            pdf_drop_document(c, d);
        }
        fz_catch(c) {
            fz_rethrow(c);
        }
    });

    EXPECT_EQ(reopenedCount, 5) << "saved document has the wrong page count";
    std::set<int> distinctObjs(pageObjNums.begin(), pageObjNums.end());
    EXPECT_EQ(distinctObjs.size(), 5u)
        << "two /Kids entries reference the SAME page object after the move — "
           "the permutation rebuild duplicated/merged a page (GitHub #346). "
           "Distinct page objects: " << distinctObjs.size() << " of 5.";

    std::filesystem::remove(out, ec);
}

// The REACHABLE #346 fix: moving an inserted page must refresh the Pages-panel
// thumbnails so the moved page is not served stale from the index-keyed cache
// at its old position (the "duplicate" the user saw). Drives the real
// MainWindow insert-blank + move actions and measures a re-rasterization with
// the content-agnostic renderCount() oracle (a cache HIT does not increment it).
// Pre-#336, refreshPages() did not flush the image cache on a reorder and this
// would stay at 0.
TEST(PageInsertGraftMoveDistinct, InsertedPageMoveRefreshesThumbnailsNoStaleDuplicate)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    const auto pdf = fixture("test_3page.pdf");
    if (!std::filesystem::exists(pdf)) GTEST_SKIP() << "test_3page.pdf unavailable";

    MainWindow mw;
    mw.openFile(QString::fromStdString(pdf.string()));
    settle();
    ASSERT_NE(mw.documentForTesting(), nullptr) << "fixture failed to open";

    auto* panel = mw.findChild<PagePanel*>();
    auto* insertBlank =
        mw.findChild<QAction*>(QStringLiteral("insertBlankPageAction"));
    auto* moveDown =
        mw.findChild<QAction*>(QStringLiteral("movePageDownAction"));
    ASSERT_NE(panel, nullptr) << "MainWindow has no PagePanel";
    ASSERT_NE(insertBlank, nullptr);
    ASSERT_NE(moveDown, nullptr);

    pumpUntilRendersSettle();  // let the initial thumbnail burst drain

    // Insert a blank page after the current page (the real "inserted page").
    ASSERT_TRUE(insertBlank->isEnabled()) << "insert-blank disabled after open";
    insertBlank->trigger();
    settle();
    pumpUntilRendersSettle();
    const int afterInsertCount = mw.documentForTesting()->pageCount();
    ASSERT_EQ(afterInsertCount, 4) << "insert-blank did not add a page";

    // Now MOVE a page and measure whether the panel re-rasterized. A move
    // renumbers pages, so a correct refresh flushes the index-keyed cache and
    // re-renders; a stale panel serves cached bitmaps and renderCount stays 0.
    ASSERT_TRUE(moveDown->isEnabled()) << "move-down disabled";
    pdfe::rendering::ThumbnailRenderer::resetRenderCount();
    moveDown->trigger();
    settle();
    pumpUntilRendersSettle();

    EXPECT_GT(pdfe::rendering::ThumbnailRenderer::renderCount(), 0u)
        << "moving a page did not refresh the Pages-panel thumbnails (GitHub "
           "#346): the index-keyed thumbnail cache was not flushed on the "
           "reorder, so the moved page is served its stale bitmap at the old "
           "index — the 'duplicate that then merges' the user reported.";
}
