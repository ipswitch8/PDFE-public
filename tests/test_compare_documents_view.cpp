// -*- coding: utf-8 -*-
// Pipeline E phase-8 / Bucket 3f: side-by-side visual diff UI.
//
// Verifies the CompareDialog drives a real DiffEngine and surfaces
// the diff result through its public API: highlight count matches
// DiffEngine output, next/prev change navigation jumps to the right
// page, page count reflects the underlying diff result.
//
// Also includes a performance smoke-test that diffs a 100-page
// fixture in under 5 seconds (the perf-benchmarks gate target).
#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "diff/DiffEngine.h"
#include "diff/DiffTypes.h"
#include "ui/CompareDialog.h"

#include <QApplication>
#include <QColor>
#include <QImage>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>

using namespace pdfe::core;
using namespace pdfe::diff;
using namespace pdfe::ui;

namespace {

// Ensure a QApplication exists for any QWidget construction. gtest
// runs from a process with a Qt event loop already, but be defensive.
QApplication* ensureQApp() {
    if (!QCoreApplication::instance()) {
        static int argc = 1;
        static const char* argv0 = "test_compare_documents_view";
        static char* argv[] = { const_cast<char*>(argv0), nullptr };
        return new QApplication(argc, argv);
    }
    return nullptr;
}

} // namespace

class CompareDocumentsViewTest : public ::testing::Test {
protected:
    void SetUp() override {
        ensureQApp();
        ctx = std::make_shared<MuPdfContext>();
        auto fixtures = std::filesystem::path(TEST_FIXTURES_DIR);
        auto srcA = fixtures / "test_3page.pdf";
        auto srcB = fixtures / "test_3page.pdf";
        if (!std::filesystem::exists(srcA)) {
            GTEST_SKIP() << "test_3page.pdf not available";
        }
        docA = Document::open(ctx, srcA);
        docB = Document::open(ctx, srcB);
        ASSERT_NE(docA, nullptr);
        ASSERT_NE(docB, nullptr);
    }

    std::shared_ptr<MuPdfContext> ctx;
    std::unique_ptr<Document> docA;
    std::unique_ptr<Document> docB;
};

// (1) "open two PDFs, run diff, verify highlight count matches DiffEngine output"
TEST_F(CompareDocumentsViewTest, HighlightCountMatchesDiffEngineOutput)
{
    // Reference: run the engine independently to get the change-region
    // count we expect the dialog to surface.
    DiffEngine engine(ctx);
    DiffResult ref = engine.compare(docA.get(), docB.get());
    int expectedRegions = 0;
    for (const auto& p : ref.pages) {
        expectedRegions += static_cast<int>(p.changeRegions.size());
    }

    CompareDialog dlg(ctx);
    int changedPages = dlg.compareLoadedDocuments(docA.get(), docB.get());
    EXPECT_EQ(changedPages, ref.changedPages);
    EXPECT_EQ(dlg.totalChangeRegions(), expectedRegions);
    EXPECT_EQ(dlg.changedPageCount(), ref.changedPages);
}

// Karen-recommended: cross-check region count for two genuinely
// different documents (not the trivial identical-doc path).
TEST_F(CompareDocumentsViewTest, HighlightCountMatchesForDifferentDocuments)
{
    auto fixtures = std::filesystem::path(TEST_FIXTURES_DIR);
    auto src3 = fixtures / "test_3page.pdf";
    auto src1 = fixtures / "test_single.pdf";
    if (!std::filesystem::exists(src3) || !std::filesystem::exists(src1)) {
        GTEST_SKIP() << "fixtures not available";
    }
    auto a = Document::open(ctx, src3);
    auto b = Document::open(ctx, src1);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);

    DiffEngine engine(ctx);
    DiffResult ref = engine.compare(a.get(), b.get());
    int expectedRegions = 0;
    for (const auto& p : ref.pages) {
        expectedRegions += static_cast<int>(p.changeRegions.size());
    }

    CompareDialog dlg(ctx);
    int changedPages = dlg.compareLoadedDocuments(a.get(), b.get());
    EXPECT_EQ(changedPages, ref.changedPages);
    EXPECT_EQ(dlg.totalChangeRegions(), expectedRegions);
    EXPECT_EQ(dlg.changedPageCount(), ref.changedPages);
}

// (2) Next-change navigation advances to the correct page when the
// fixture has page-level differences.
TEST_F(CompareDocumentsViewTest, NextChangeNavigationFindsChangedPage)
{
    auto fixtures = std::filesystem::path(TEST_FIXTURES_DIR);
    auto src3 = fixtures / "test_3page.pdf";
    auto src1 = fixtures / "test_single.pdf";
    if (!std::filesystem::exists(src3) || !std::filesystem::exists(src1)) {
        GTEST_SKIP() << "fixtures not available";
    }
    // test_single.pdf vs test_3page.pdf: page counts differ, so the
    // engine sees changes on every page beyond page 0.
    auto a = Document::open(ctx, src3);
    auto b = Document::open(ctx, src1);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);

    CompareDialog dlg(ctx);
    int changed = dlg.compareLoadedDocuments(a.get(), b.get());

    if (changed == 0) {
        // The test fixtures happen to be identical at the visual
        // level — skip rather than assert false success.
        GTEST_SKIP() << "diff produced no changed pages on this fixture";
    }
    int target = dlg.gotoNextChangedPage();
    EXPECT_GE(target, 0);
    EXPECT_EQ(dlg.currentPageIndex(), target);

    // GitHub #353: walk forward to the LAST changed page; Next must then be a
    // no-op (clamp at the end), NOT a modular wrap back to the first change.
    int last = target;
    for (int n = dlg.gotoNextChangedPage(); n >= 0; n = dlg.gotoNextChangedPage()) {
        EXPECT_GT(n, last) << "Next change must advance monotonically (no wrap)";
        last = n;
    }
    EXPECT_EQ(dlg.currentPageIndex(), last)
        << "Next at the last change must leave the current page unchanged";
    EXPECT_EQ(dlg.gotoNextChangedPage(), -1)
        << "Next at the last change must be a no-op, not wrap to the first";
}

TEST_F(CompareDocumentsViewTest, IdenticalDocumentsHaveZeroChangedPages)
{
    CompareDialog dlg(ctx);
    int changed = dlg.compareLoadedDocuments(docA.get(), docB.get());
    EXPECT_EQ(changed, 0);
    EXPECT_EQ(dlg.totalChangeRegions(), 0);
    // No changed pages: gotoNextChangedPage returns -1 sentinel.
    EXPECT_EQ(dlg.gotoNextChangedPage(), -1);
}

TEST_F(CompareDocumentsViewTest, NullDocumentsReturnZeroSafely)
{
    CompareDialog dlg(ctx);
    EXPECT_EQ(dlg.compareLoadedDocuments(nullptr, docB.get()), 0);
    EXPECT_EQ(dlg.compareLoadedDocuments(docA.get(), nullptr), 0);
    EXPECT_EQ(dlg.compareLoadedDocuments(nullptr, nullptr), 0);
}

// Perf benchmark: per the phase-8 perf-benchmarks gate, a 100-page
// diff should complete in < 5s on the dev machine in Release.
// Per the acceptance criterion ("measured by the benchmark, not
// asserted in the unit test"), this test only measures and logs —
// the perf-benchmarks gate is responsible for asserting the budget.
// The test verifies the engine completes the work and produces a
// well-formed result; the elapsed time is reported for visibility.
TEST_F(CompareDocumentsViewTest, OneHundredPageDiffMeasurement)
{
    auto fixtures = std::filesystem::path(TEST_FIXTURES_DIR);
    auto src = fixtures / "test_100page.pdf";
    if (!std::filesystem::exists(src)) {
        GTEST_SKIP() << "test_100page.pdf not available";
    }
    auto a = Document::open(ctx, src);
    auto b = Document::open(ctx, src);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);

    DiffEngine engine(ctx);
    auto t0 = std::chrono::steady_clock::now();
    DiffResult r = engine.compare(a.get(), b.get());
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    EXPECT_EQ(r.totalPages, 100);
    GTEST_LOG_(INFO) << "100-page diff elapsed: " << elapsed
                     << " ms (Debug build; <5s budget is for Release "
                     << "and is asserted by the perf-benchmarks gate)";
}

// ===================================================================
// Pipeline many-bugs-2026-11 phase-5: Compare Documents defects
// GitHub #351 (highlight colour), #352 (overlay alignment), #353
// (nav wrap + solid one-sided page).
// ===================================================================

namespace {

// A one-sided (added/removed) page: present in one document only. The
// engine flags it with similarityScore == 0 and a non-null overlay.
int findOneSidedPage(const DiffResult* r) {
    if (!r) return -1;
    for (size_t i = 0; i < r->pages.size(); ++i) {
        const auto& p = r->pages[i];
        if (!p.diffImage.isNull() && p.similarityScore < 1e-6)
            return static_cast<int>(i);
    }
    return -1;
}

// Max alpha sampled on a grid — detects an opaque fill without scanning
// every pixel of a 150-dpi page image.
int maxAlphaSampled(const QImage& img) {
    if (img.isNull()) return -1;
    int maxA = 0;
    const int steps = 7;
    for (int iy = 0; iy <= steps; ++iy) {
        for (int ix = 0; ix <= steps; ++ix) {
            int x = (img.width() - 1) * ix / steps;
            int y = (img.height() - 1) * iy / steps;
            maxA = std::max(maxA, img.pixelColor(x, y).alpha());
        }
    }
    return maxA;
}

} // namespace

// Fixture that diffs a 3-page document against a 1-page document, so
// pages 1..2 are one-sided (deterministic coloured overlays) — the
// vehicle for the colour and one-sided-overlay assertions.
class CompareDefectsTest : public ::testing::Test {
protected:
    void SetUp() override {
        ensureQApp();
        ctx = std::make_shared<MuPdfContext>();
        auto fixtures = std::filesystem::path(TEST_FIXTURES_DIR);
        auto src3 = fixtures / "test_3page.pdf";
        auto src1 = fixtures / "test_single.pdf";
        if (!std::filesystem::exists(src3) || !std::filesystem::exists(src1)) {
            GTEST_SKIP() << "fixtures not available";
        }
        docA = Document::open(ctx, src3);
        docB = Document::open(ctx, src1);
        ASSERT_NE(docA, nullptr);
        ASSERT_NE(docB, nullptr);
    }
    std::shared_ptr<MuPdfContext> ctx;
    std::unique_ptr<Document> docA;
    std::unique_ptr<Document> docB;
};

// (1) GitHub #351 — selecting a non-default highlight colour must drive the
// diff colour. Pre-fix the combo was never connected, so the overlay stayed
// the hardcoded red regardless of selection.
TEST_F(CompareDefectsTest, HighlightColorSelectionDrivesDiffColor)
{
    CompareDialog dlg(ctx);
    ASSERT_GT(dlg.compareLoadedDocuments(docA.get(), docB.get()), 0);

    // Default is red.
    EXPECT_EQ(dlg.currentDiffColorForTesting(), QColor(255, 0, 0));
    int os = findOneSidedPage(dlg.diffResultForTesting());
    ASSERT_GE(os, 0) << "fixture should yield a one-sided page";
    {
        const QImage& ov = dlg.diffResultForTesting()->pages[os].diffImage;
        QColor c = ov.pixelColor(ov.width() / 2, ov.height() / 2);
        EXPECT_EQ(c.red(), 255);
        EXPECT_EQ(c.green(), 0);
        EXPECT_EQ(c.blue(), 0);
    }

    // Pick blue: the stored colour and the rendered overlay must both change.
    ASSERT_TRUE(dlg.selectDiffColorForTesting(QColor(0, 0, 255)));
    EXPECT_EQ(dlg.currentDiffColorForTesting(), QColor(0, 0, 255));

    int os2 = findOneSidedPage(dlg.diffResultForTesting());
    ASSERT_GE(os2, 0);
    const QImage& ov2 = dlg.diffResultForTesting()->pages[os2].diffImage;
    QColor c2 = ov2.pixelColor(ov2.width() / 2, ov2.height() / 2);
    EXPECT_EQ(c2.red(), 0);
    EXPECT_EQ(c2.green(), 0);
    EXPECT_EQ(c2.blue(), 255)
        << "highlight colour did not follow the selection (still red?)";
}

// (2) GitHub #352 — the overlay must register to the same page-space region
// as the diffed content at the DISPLAY scale. The engine renders the overlay
// at 150 dpi; the page view renders at the display scale. Pre-fix the overlay
// was blitted 1:1 at (0,0), ~2x oversized.
TEST_F(CompareDefectsTest, OverlayRegistersToPageSpaceAtDisplayScale)
{
    CompareDialog dlg(ctx);
    ASSERT_GT(dlg.compareLoadedDocuments(docA.get(), docB.get()), 0);

    // compareLoadedDocuments displays page 0, recording the composited overlay.
    QImage composed = dlg.lastComposedOverlayForTesting();
    QSize pageSize = dlg.lastDisplayedPageSizeForTesting();
    ASSERT_FALSE(composed.isNull());
    ASSERT_FALSE(pageSize.isEmpty());

    // Alignment: the composited overlay fills exactly the page render — the
    // same page-space extent, not an oversized engine-DPI blit at (0,0).
    EXPECT_EQ(composed.size(), pageSize)
        << "overlay not registered to the page at display scale";

    // Navigation updates BOTH the displayed content and the highlight: after
    // Next Change the displayed page index follows, and the overlay recorded
    // for the new page is again registered to it.
    int nextIdx = dlg.gotoNextChangedPage();
    ASSERT_GE(nextIdx, 0);
    EXPECT_EQ(dlg.currentPageIndex(), nextIdx);
    EXPECT_EQ(dlg.lastDisplayedPageIndexForTesting(), nextIdx)
        << "Next Change did not re-render the displayed page";
    EXPECT_EQ(dlg.lastComposedOverlayForTesting().size(),
              dlg.lastDisplayedPageSizeForTesting())
        << "highlight position did not update with the page";

    // Geometry oracle (not size-only): anchor on a one-sided page, whose raw
    // engine overlay and display render both derive from the SAME source page,
    // so the ratio is exactly the dpi ratio (engineScale/displayScale). This
    // proves the overlay maps the same page-space region, down-scaled from the
    // engine DPI — pre-fix it was blitted raw (ratio ~2x, misaligned).
    int os = findOneSidedPage(dlg.diffResultForTesting());
    ASSERT_GE(os, 0) << "fixture should yield a one-sided page";
    // Navigate to the one-sided page so displayPage records its geometry.
    while (dlg.lastDisplayedPageIndexForTesting() != os) {
        int n = dlg.gotoNextChangedPage();
        if (n < 0) break;
    }
    ASSERT_EQ(dlg.lastDisplayedPageIndexForTesting(), os);

    const QImage& raw = dlg.diffResultForTesting()->pages[os].diffImage;
    QSize osPageSize = dlg.lastDisplayedPageSizeForTesting();
    QImage osComposed = dlg.lastComposedOverlayForTesting();
    ASSERT_FALSE(raw.isNull());
    ASSERT_FALSE(osPageSize.isEmpty());
    EXPECT_EQ(osComposed.size(), osPageSize)
        << "one-sided overlay not registered to the page at display scale";

    const float engineScale = dlg.lastEngineRenderScaleForTesting();
    const float displayScale = dlg.displayScaleForTesting();
    ASSERT_GT(osPageSize.width(), 0);
    const double ratio = static_cast<double>(raw.width()) / osPageSize.width();
    EXPECT_NEAR(ratio, engineScale / displayScale, 0.1)
        << "overlay/page scale ratio does not match the dpi ratio";
}

// (3) GitHub #353 — Prev/Next Change must clamp at the ends, never wrap.
TEST_F(CompareDefectsTest, NextChangeClampsAtLastWithoutWrap)
{
    CompareDialog dlg(ctx);
    ASSERT_GT(dlg.compareLoadedDocuments(docA.get(), docB.get()), 0);

    // Walk forward to the last changed page; indices must advance monotonically.
    int last = -1;
    for (int n = dlg.gotoNextChangedPage(); n >= 0; n = dlg.gotoNextChangedPage()) {
        EXPECT_GT(n, last) << "Next change must advance monotonically (no wrap)";
        last = n;
    }
    ASSERT_GE(last, 0) << "expected at least one changed page";

    // At the last change, Next is a no-op — not a wrap back to the first.
    EXPECT_EQ(dlg.currentPageIndex(), last);
    EXPECT_EQ(dlg.gotoNextChangedPage(), -1)
        << "Next at the last change wrapped instead of clamping";
    EXPECT_EQ(dlg.currentPageIndex(), last)
        << "a no-op Next must not move the current page";

    // Prev before the first change (from page 0) also clamps.
    while (dlg.gotoPrevChangedPage() >= 0) { /* walk back to the first */ }
    EXPECT_EQ(dlg.gotoPrevChangedPage(), -1)
        << "Prev at/ before the first change wrapped instead of clamping";
}

// (3b) GitHub #353 — a one-sided page must render as a transparent marker so
// the real page content shows, not an opaque solid block.
TEST_F(CompareDefectsTest, OneSidedPageRendersTransparentMarkerNotSolidBlock)
{
    CompareDialog dlg(ctx);
    ASSERT_GT(dlg.compareLoadedDocuments(docA.get(), docB.get()), 0);

    int os = findOneSidedPage(dlg.diffResultForTesting());
    ASSERT_GE(os, 0) << "fixture should yield a one-sided page";

    const QImage& overlay = dlg.diffResultForTesting()->pages[os].diffImage;
    ASSERT_FALSE(overlay.isNull());

    // Pre-fix the whole opaque page render was stored as the overlay (alpha
    // 255 everywhere) → a solid block. Post-fix it is a translucent marker,
    // so no sampled pixel is fully opaque and the page content shows through.
    int maxA = maxAlphaSampled(overlay);
    EXPECT_LT(maxA, 255)
        << "one-sided overlay is a uniform opaque fill (solid block)";
    EXPECT_GT(maxA, 0)
        << "one-sided overlay should still carry a visible marker tint";
}
