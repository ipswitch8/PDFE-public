// -*- coding: utf-8 -*-
// GitHub #262 — RECOMPOSITE_DONE is reported for pixels the user never sees.
//
// THE DEFECT
//
// `PdfContentWriter::findPageImageXObject()` selects the image to write back
// into by ONE rule: "Find largest image XObject" (PdfContentWriter.cpp:84),
// comparing Width*Height. It never asks WHERE the edit was.
//
// On `mixed_dpi_page.pdf` that is decisive. The page carries two images, and
// the content stream draws them in this order:
//
//     q 510 0 0 600 51 141 cm  /fzImg0 Do      <- xref 5, 2125x2500 = 5.3M px
//     q 144 0 0 144 80 178 cm  /fzImg1 Do      <- xref 9,  300x300  =  90K px
//
// fzImg1 is drawn SECOND, on top, and its placement — fitz Rect(80, 470, 224,
// 614) — sits wholly inside fzImg0's Rect(51, 51, 561, 651). So an edit over
// the THUMBNAIL is composited, scaled, and written into fzImg0 (the larger),
// where fzImg1 then paints straight over it. The write genuinely happened; it
// is simply invisible.
//
// `replacePageImage()` returns true regardless, so `ImageReplacer::replace()`
// sets `result.success = true`, and MainWindow.cpp:12181 publishes
// **RECOMPOSITE_DONE** — a terminal success outcome — for a region whose pixels
// are byte-identical before and after. That is the `changed=False, sharpness
// 2.18 -> 2.18` recorded in #262.
//
// WHY THIS IS NOT DEFECT #2
//
// #262 explicitly left open whether this was the per-region-DPI defect (#2) or
// a separate write-back failure, and warned against assuming the former merely
// because it is the nearest open issue. It is the latter. #2 predicts a region
// that CHANGED but is smeared, because it was re-rendered at the page-maximum
// 300 DPI instead of its native 150. The observable here is no change at all,
// and the smearing oracle (`sharp_after >= 0.85 * sharp_before`) cannot tell
// those apart while nothing changes. Per-region DPI is already implemented and
// correct (`Page::sourceImageDpiAt`, covered by test_image_replacer_dpi) — and
// it is worth noting that that function ALREADY does the region-aware image
// selection this writer lacks, preferring the topmost/tightest image over the
// region. The two disagree about which image the thumbnail belongs to.
//
// WHY THE CONTROL BELOW IS LOAD-BEARING
//
// A test that only asserted "the thumbnail changed" could go red for a dull
// reason — a re-render that does not pick up in-memory object edits at all
// would produce an identical crop no matter what the writer did, and would look
// exactly like this defect. `AnEditOverThePhotoOnlyRegionDoesChangeItsPixels`
// performs the SAME write and crops a region covered by fzImg0 alone. It must
// go green. Green control + red subject isolates the fault to which image was
// chosen; two reds would mean the instrument is broken, not the product.
#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "fonts/EmbeddedFontAnalyzer.h"
#include "fonts/FontAnalysisCache.h"
#include "fonts/PdfFontEmbedder.h"
#include "fonts/SubstituteFontSelector.h"
#include "tre/BgColorDetector.h"
#include "tre/BitmapCompositor.h"
#include "tre/CoordMapper.h"
#include "tre/FontMatcher.h"
#include "tre/ImageMasker.h"
#include "tre/ImageReplacer.h"
#include "tre/OcrPipeline.h"
#include "tre/PdfContentWriter.h"
#include "tre/RegionReconstructor.h"

#include <QImage>
#include <QRect>

#include <cctype>
#include <filesystem>
#include <memory>
#include <string>

using namespace pdfe::core;
using namespace pdfe::tre;

namespace {

// Fixture geometry, from generate_render_oracle_fixtures.py::make_mixed_dpi_page
// and re-measured against the committed file. Fitz points; page is 612 x 792,
// so at scale 1.0 these are also pixel coordinates in the rendered page.
const QRect kThumbRegion(80, 470, 144, 144);    // fzImg1, 150 DPI, drawn LAST
const QRect kPhotoOnlyRegion(300, 150, 150, 150);  // fzImg0 only — no overlap

/// Count pixels that differ between two equally-sized crops.
int differingPixels(const QImage& a, const QImage& b)
{
    if (a.size() != b.size() || a.isNull() || b.isNull()) return -1;
    int n = 0;
    for (int y = 0; y < a.height(); ++y) {
        for (int x = 0; x < a.width(); ++x) {
            if (a.pixel(x, y) != b.pixel(x, y)) ++n;
        }
    }
    return n;
}

}  // namespace

class RecompositeVisiblePixelsTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        context_ = std::make_shared<MuPdfContext>();

        const std::filesystem::path fixture =
            std::filesystem::path(TEST_FIXTURES_DIR) / "mixed_dpi_page.pdf";
        if (!std::filesystem::exists(fixture)) {
            GTEST_SKIP() << "mixed_dpi_page.pdf not available";
        }

        document_ = Document::open(context_, fixture);
        ASSERT_NE(document_, nullptr);
        ASSERT_GT(document_->pageCount(), 0);

        fontEmbedder_ = std::make_unique<pdfe::fonts::PdfFontEmbedder>(context_.get());
        writer_ = std::make_unique<PdfContentWriter>(context_.get(), fontEmbedder_.get());
    }

    void TearDown() override
    {
        writer_.reset();
        fontEmbedder_.reset();
        document_.reset();
        context_.reset();
    }

    /// Render page 0 fresh and crop. The page is re-fetched each time so the
    /// render reflects object edits made since the last call.
    QImage renderCrop(const QRect& r)
    {
        auto page = document_->page(0);
        if (!page) return {};
        const QImage full = page->render(QRectF(), 1.0f);
        if (full.isNull()) return {};
        return full.copy(r.intersected(full.rect()));
    }

    /// A page-sized solid block, deliberately unlike anything in the fixture.
    /// Whatever image the writer selects, its pixels become this colour, so the
    /// question "did the region change?" has an unambiguous answer.
    static QImage solidBlock()
    {
        QImage img(612, 792, QImage::Format_RGB888);
        img.fill(QColor(255, 0, 255));  // magenta — absent from the fixture
        return img;
    }

    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<Document> document_;
    std::unique_ptr<pdfe::fonts::PdfFontEmbedder> fontEmbedder_;
    std::unique_ptr<PdfContentWriter> writer_;
};

// The control. Same write, a region only fzImg0 covers. This must pass both
// before and after the fix; if it ever fails, the subject test's red below says
// nothing about image selection.
TEST_F(RecompositeVisiblePixelsTest, AnEditOverThePhotoOnlyRegionDoesChangeItsPixels)
{
    const QImage before = renderCrop(kPhotoOnlyRegion);
    ASSERT_FALSE(before.isNull()) << "could not render the page at all";

    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);
    ASSERT_TRUE(writer_->replacePageImage(page.get(), document_.get(), solidBlock()))
        << "replacePageImage reported failure; the instrument cannot measure "
           "anything about targeting";

    const QImage after = renderCrop(kPhotoOnlyRegion);
    ASSERT_FALSE(after.isNull());

    EXPECT_GT(differingPixels(before, after), 0)
        << "a region covered ONLY by the image the writer selects did not "
           "change. The re-render is not observing in-memory object edits, so "
           "this whole test file measures nothing.";
}

// The defect, stated as the selection it comes from. The legacy two-argument
// form is UNCHANGED on purpose — callers such as undo/redo capture legitimately
// mean "the page's main image" — so this pins the difference between the two
// rather than asserting the old form is now magically right.
TEST_F(RecompositeVisiblePixelsTest, TheLargestImageIsNotTheOneUnderTheThumbnail)
{
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);

    const auto biggest = writer_->findPageImageXObject(page.get(), document_.get());
    ASSERT_TRUE(biggest.has_value());
    EXPECT_EQ(biggest->width, 2125)
        << "the size-ranked lookup no longer returns the 300-DPI photo; the "
           "fixture changed and the rest of this file is measuring something "
           "else";

    const auto forThumb = writer_->findImageXObjectForRegion(
        page.get(), document_.get(), QRectF(80, 470, 144, 144));
    ASSERT_TRUE(forThumb.has_value())
        << "no image resolved for a region that sits squarely on fzImg1";

    EXPECT_EQ(forThumb->width, 300)
        << "the region-aware lookup still returned the 2125px photo for a "
           "region lying wholly inside the 300px thumbnail. This is the "
           "selection half of GitHub #262.";
    EXPECT_NE(forThumb->xobjectName, biggest->xobjectName)
        << "region-aware and size-ranked selection returned the SAME XObject, "
           "so the new lookup cannot be distinguished from the old one";

    // Placement must come back too — the coordinate mapping depends on it, and
    // an empty rect silently degrades to the page-spanning assumption.
    EXPECT_NEAR(forThumb->placementFitz.x(), 80.0, 1.0);
    EXPECT_NEAR(forThumb->placementFitz.y(), 470.0, 1.0);
    EXPECT_NEAR(forThumb->placementFitz.width(), 144.0, 1.0);
}

// The subject. #262 itself: writing through the region-aware target must change
// what the user can actually see.
TEST_F(RecompositeVisiblePixelsTest, AnEditOverTheThumbnailChangesTheThumbnailsPixels)
{
    const QImage before = renderCrop(kThumbRegion);
    ASSERT_FALSE(before.isNull());

    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);

    const auto target = writer_->findImageXObjectForRegion(
        page.get(), document_.get(), QRectF(80, 470, 144, 144));
    ASSERT_TRUE(target.has_value());

    const bool reportedSuccess = writer_->replacePageImage(
        page.get(), document_.get(), solidBlock(), *target);

    const QImage after = renderCrop(kThumbRegion);
    ASSERT_FALSE(after.isNull());

    const int changed = differingPixels(before, after);

    // The two halves of the defect, asserted separately so a failure names
    // which one broke.
    EXPECT_GT(changed, 0)
        << "the thumbnail region is byte-identical after a write-back that "
           "reported success. The write went to an image that fzImg1 — drawn "
           "second — paints over. This is GitHub #262: RECOMPOSITE_DONE for "
           "pixels that never changed.";

    EXPECT_FALSE(reportedSuccess && changed == 0)
        << "replacePageImage returned true while changing nothing visible. A "
           "success that the user cannot see is the reason #262 reports a "
           "terminal RECOMPOSITE_DONE with sharpness 2.18 -> 2.18.";
}

// ---------------------------------------------------------------------------
// CLASS-LEVEL (DCA 0105)
//
// The family is not "the thumbnail was missed". It is: **a write-back reports
// success without checking the observable it claims to have produced.** Any
// page arrangement where the image chosen is not the image seen produces the
// same false DONE — a small image over a large one, an image that does not span
// the page, a page whose images do not overlap the edit at all.
//
// So the invariant is stated once and run over arrangements, rather than pinning
// the one arrangement that happened to be reported:
//
//     for any region on any page, writing through the region-resolved target
//     must change what the page RENDERS at that region.
//
// The rendered form is the oracle deliberately. Asserting on the XObject's own
// pixels would go green for the exact defect #262 describes, because those
// pixels did change — they were simply painted over.
// ---------------------------------------------------------------------------

struct Arrangement {
    const char* fixture;
    const char* what;
    QRectF region;           // fitz points; empty => derive a central region
    bool expectLargest;      // does the region-resolved target equal the largest?
};

class RecompositeArrangementTest
    : public ::testing::TestWithParam<Arrangement> {
protected:
    void SetUp() override
    {
        context_ = std::make_shared<MuPdfContext>();
        const std::filesystem::path f =
            std::filesystem::path(TEST_FIXTURES_DIR) / GetParam().fixture;
        if (!std::filesystem::exists(f)) {
            GTEST_SKIP() << GetParam().fixture << " not available";
        }
        document_ = Document::open(context_, f);
        ASSERT_NE(document_, nullptr);
        ASSERT_GT(document_->pageCount(), 0);
        fontEmbedder_ = std::make_unique<pdfe::fonts::PdfFontEmbedder>(context_.get());
        writer_ = std::make_unique<PdfContentWriter>(context_.get(), fontEmbedder_.get());
    }

    void TearDown() override
    {
        writer_.reset();
        fontEmbedder_.reset();
        document_.reset();
        context_.reset();
    }

    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<Document> document_;
    std::unique_ptr<pdfe::fonts::PdfFontEmbedder> fontEmbedder_;
    std::unique_ptr<PdfContentWriter> writer_;
};

TEST_P(RecompositeArrangementTest, AWriteThroughTheResolvedTargetIsVisible)
{
    const Arrangement& a = GetParam();

    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);

    // Never guess a region: where the parameter leaves it empty, derive a
    // central quarter from the page's own bounds.
    QRectF region = a.region;
    if (region.isEmpty()) {
        const QRectF b = page->boundingBox();
        ASSERT_FALSE(b.isEmpty()) << "page reports no bounds";
        region = QRectF(b.x() + b.width() * 0.375, b.y() + b.height() * 0.375,
                        b.width() * 0.25, b.height() * 0.25);
    }

    const auto largest = writer_->findPageImageXObject(page.get(), document_.get());
    if (!largest.has_value()) {
        GTEST_SKIP() << a.what << ": page has no image XObject to write into";
    }

    const auto target =
        writer_->findImageXObjectForRegion(page.get(), document_.get(), region);
    ASSERT_TRUE(target.has_value())
        << a.what << ": no image resolved for the region";

    EXPECT_EQ(target->xobjectName == largest->xobjectName, a.expectLargest)
        << a.what << ": region-resolved target "
        << (a.expectLargest ? "should" : "should NOT")
        << " be the largest image on the page, and is not. Selection that is "
           "always-largest or always-smallest passes some rows and fails "
           "others; only genuinely region-aware selection passes all.";

    // The invariant.
    const QRect crop = region.toAlignedRect();
    const QImage full0 = page->render(QRectF(), 1.0f);
    ASSERT_FALSE(full0.isNull());
    const QImage before = full0.copy(crop.intersected(full0.rect()));
    ASSERT_FALSE(before.isNull());

    QImage block(std::max(1, full0.width()), std::max(1, full0.height()),
                 QImage::Format_RGB888);
    block.fill(QColor(255, 0, 255));
    ASSERT_TRUE(writer_->replacePageImage(page.get(), document_.get(), block, *target))
        << a.what << ": the write itself failed";

    auto page2 = document_->page(0);
    ASSERT_NE(page2, nullptr);
    const QImage full1 = page2->render(QRectF(), 1.0f);
    ASSERT_FALSE(full1.isNull());
    const QImage after = full1.copy(crop.intersected(full1.rect()));

    EXPECT_GT(differingPixels(before, after), 0)
        << a.what << ": the write reported success and the RENDERED region did "
           "not change. This is the whole family GitHub #262 belongs to — a "
           "commit to an image nobody sees, reported as RECOMPOSITE_DONE.";
}

INSTANTIATE_TEST_SUITE_P(
    Arrangements, RecompositeArrangementTest,
    ::testing::Values(
        // Small image drawn OVER a large one — the #262 arrangement.
        Arrangement{"mixed_dpi_page.pdf", "150-DPI thumbnail over 300-DPI photo",
                    QRectF(80, 470, 144, 144), false},
        // Same page, a region the small image does not cover.
        Arrangement{"mixed_dpi_page.pdf", "300-DPI photo alone",
                    QRectF(300, 150, 150, 150), true},
        // An ordinary single-image scan: the arrangement that always worked,
        // kept here so a "fix" that only handles multi-image pages is caught.
        Arrangement{"test_scanned.pdf", "single full-page scan", QRectF(), true}),
    [](const ::testing::TestParamInfo<Arrangement>& i) {
        std::string s(i.param.what);
        for (char& c : s) {
            if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
        }
        return s;
    });

// ---------------------------------------------------------------------------
// THE VISIBILITY CHECK ITSELF (DCA 0105, standing control 1)
//
// Everything above drives PdfContentWriter and so proves the TARGETING half.
// It cannot reach the check that makes the fix safe for causes not yet
// imagined: `ImageReplacer::replace()` comparing the rendered region before and
// after, and refusing to report success when nothing visible changed.
//
// A mutation sweep made that gap concrete. Neutralising the check killed no
// gtest at all (`P3-M3` in tests/scripts/mutations/p94_phase3.json), because on
// every existing fixture the targeting is correct, the write is visible, and a
// check that never fires is indistinguishable from a check that is absent.
//
// `occluded_write_target.pdf` exists to fire it, and every step is forced
// rather than incidental — see make_occluded_write_target_page() for the full
// argument. In short: the edited region sits on a 300x300 image drawn LAST over
// a 1200x1200 one, and a second 300x300 image elsewhere on the page makes the
// dimension match ambiguous. So selection bails out (by design, rather than
// guess between two same-sized images), the fallback lands on the 1200x1200
// base, and the top image paints over the result. The write is committed and
// invisible — which must be reported as FAILURE.
// ---------------------------------------------------------------------------

/// Shared rig for the two tests that drive `ImageReplacer::replace()` for real.
/// Both need the full TRE component graph and tessdata; only the fixture and the
/// expected outcome differ, and the outcome is the point of each.
class RecompositeReplaceTestBase : public ::testing::Test {
protected:
    virtual const char* fixtureName() const = 0;

    void SetUp() override
    {
        const std::filesystem::path fixture =
            std::filesystem::path(TEST_FIXTURES_DIR) / fixtureName();
        const std::filesystem::path tessdata =
            std::filesystem::path(TEST_FIXTURES_DIR) / "tessdata" / "eng.traineddata";
        if (!std::filesystem::exists(fixture)) {
            GTEST_SKIP() << fixtureName() << " not available";
        }
        if (!std::filesystem::exists(tessdata)) {
            GTEST_SKIP() << "tessdata not available; replace() cannot run OCR";
        }

        context_ = std::make_shared<MuPdfContext>();
        document_ = Document::open(context_, fixture);
        ASSERT_NE(document_, nullptr);
        ASSERT_GT(document_->pageCount(), 0);
        page_ = document_->page(0);
        ASSERT_NE(page_, nullptr);

        ocrPipeline_ = std::make_unique<OcrPipeline>(context_.get(), document_.get());
        OcrConfig cfg;
        cfg.tessdataPath =
            (std::filesystem::path(TEST_FIXTURES_DIR) / "tessdata").string();
        ocrPipeline_->setConfig(cfg);

        coordMapper_ = std::make_unique<CoordMapper>(context_.get(), document_.get());
        fontEmbedder_ = std::make_unique<pdfe::fonts::PdfFontEmbedder>(context_.get());
        contentWriter_ = std::make_unique<PdfContentWriter>(context_.get(), fontEmbedder_.get());
        fontAnalysisCache_ = std::make_unique<pdfe::fonts::FontAnalysisCache>();
        embeddedFontAnalyzer_ = std::make_unique<pdfe::fonts::EmbeddedFontAnalyzer>(context_.get());
        substituteFontSelector_ = std::make_unique<pdfe::fonts::SubstituteFontSelector>(
            embeddedFontAnalyzer_.get(), fontAnalysisCache_.get());
        fontMatcher_ = std::make_unique<FontMatcher>(
            substituteFontSelector_.get(), embeddedFontAnalyzer_.get());
        bgDetector_ = std::make_unique<BgColorDetector>();
        imageMasker_ = std::make_unique<ImageMasker>();
        bitmapCompositor_ = std::make_unique<BitmapCompositor>();
        replacer_ = std::make_unique<ImageReplacer>(
            ocrPipeline_.get(), coordMapper_.get(), bgDetector_.get(),
            fontMatcher_.get(), imageMasker_.get(), bitmapCompositor_.get(),
            contentWriter_.get());
    }

    void TearDown() override
    {
        replacer_.reset();
        bitmapCompositor_.reset();
        imageMasker_.reset();
        bgDetector_.reset();
        fontMatcher_.reset();
        substituteFontSelector_.reset();
        embeddedFontAnalyzer_.reset();
        fontAnalysisCache_.reset();
        contentWriter_.reset();
        fontEmbedder_.reset();
        coordMapper_.reset();
        ocrPipeline_.reset();
        page_.reset();
        document_.reset();
        context_.reset();
    }

    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<Document> document_;
    std::unique_ptr<Page> page_;
    std::unique_ptr<OcrPipeline> ocrPipeline_;
    std::unique_ptr<CoordMapper> coordMapper_;
    std::unique_ptr<BgColorDetector> bgDetector_;
    std::unique_ptr<FontMatcher> fontMatcher_;
    std::unique_ptr<ImageMasker> imageMasker_;
    std::unique_ptr<BitmapCompositor> bitmapCompositor_;
    std::unique_ptr<PdfContentWriter> contentWriter_;
    std::unique_ptr<pdfe::fonts::PdfFontEmbedder> fontEmbedder_;
    std::unique_ptr<pdfe::fonts::SubstituteFontSelector> substituteFontSelector_;
    std::unique_ptr<pdfe::fonts::EmbeddedFontAnalyzer> embeddedFontAnalyzer_;
    std::unique_ptr<pdfe::fonts::FontAnalysisCache> fontAnalysisCache_;
    std::unique_ptr<ImageReplacer> replacer_;
};

class RecompositeOccludedTargetTest : public RecompositeReplaceTestBase {
protected:
    const char* fixtureName() const override { return "occluded_write_target.pdf"; }
};

// ---------------------------------------------------------------------------
// The POSITIVE end-to-end case, and the only gtest that can catch a wrong
// destination offset.
//
// The occlusion test above proves replace() refuses to claim an invisible
// write. On its own that is half a control: a build that wrote to the right
// image but at the wrong PLACE inside it would also produce "nothing visible
// changed", be reported honestly as a failure, and pass that test. `P3-M4`
// makes exactly that mutation — it drops the `- placement.x()` term, restoring
// the assumption that the target image starts at the page origin — and no gtest
// killed it.
//
// This one does. The thumbnail's image is placed at fitz (80, 470), so without
// the placement subtraction the destination lands far outside a 300x300 image,
// QPainter clips it, and nothing is drawn. Requiring success AND a visible
// change makes the correct offset load-bearing.
// ---------------------------------------------------------------------------
class RecompositeMixedDpiEndToEndTest : public RecompositeReplaceTestBase {
protected:
    const char* fixtureName() const override { return "mixed_dpi_page.pdf"; }
};

TEST_F(RecompositeMixedDpiEndToEndTest, EditingTheThumbnailCommitsAVisibleChange)
{
    const QRect thumbCrop(80, 470, 144, 144);

    const QImage full0 = page_->render(QRectF(), 1.0f);
    ASSERT_FALSE(full0.isNull());
    const QImage before = full0.copy(thumbCrop.intersected(full0.rect()));
    ASSERT_FALSE(before.isNull());

    // The invisible OCR span "THUMBNAIL", measured from the fixture.
    PdfRect selection;
    selection.x0 = 84.8;
    selection.y0 = 479.6;
    selection.x1 = 171.2;
    selection.y1 = 498.8;

    const ReconstructionResult result = replacer_->replace(
        page_.get(), document_.get(), 0, selection, "REPLACED", RegionId{1});

    auto page2 = document_->page(0);
    ASSERT_NE(page2, nullptr);
    const QImage full1 = page2->render(QRectF(), 1.0f);
    ASSERT_FALSE(full1.isNull());
    const QImage after = full1.copy(thumbCrop.intersected(full1.rect()));
    const int changed = differingPixels(before, after);

    EXPECT_TRUE(result.success)
        << "editing the 150-DPI thumbnail failed outright. errorMessage="
        << result.errorMessage;
    EXPECT_GT(changed, 0)
        << "the recomposite reported success and the thumbnail region did not "
           "change. Either the write went to the wrong image (GitHub #262's "
           "selection half) or it landed at the wrong offset inside the right "
           "one (the placement half) — both produce this.";
    EXPECT_EQ(result.success, changed > 0)
        << "report and pixels disagree; see errorMessage=" << result.errorMessage;
}

// The fixture's premise, asserted rather than assumed. If any of this stops
// holding, the test below stops testing the visibility check and would quietly
// pass for the wrong reason.
TEST_F(RecompositeOccludedTargetTest, TheFallbackTargetIsTheOccludedImage)
{
    const QRectF topRegion(80, 470, 144, 144);

    const auto resolved = contentWriter_->findImageXObjectForRegion(
        page_.get(), document_.get(), topRegion);
    EXPECT_FALSE(resolved.has_value())
        << "region selection resolved a target on a page where two image "
           "resources share 300x300 dimensions. It must bail out instead of "
           "guessing, or the fallback path below is never taken and this "
           "fixture tests nothing.";

    const auto fallback = contentWriter_->findPageImageXObject(
        page_.get(), document_.get());
    ASSERT_TRUE(fallback.has_value());
    EXPECT_EQ(fallback->width, 1200)
        << "the fallback is not the 1200x1200 base image, so the write would "
           "not be occluded and the check under test would not fire";
}

TEST_F(RecompositeOccludedTargetTest, AnInvisibleRecompositeIsReportedAsFailure)
{
    const QRect topCrop(80, 470, 144, 144);

    const QImage full0 = page_->render(QRectF(), 1.0f);
    ASSERT_FALSE(full0.isNull());
    const QImage before = full0.copy(topCrop.intersected(full0.rect()));
    ASSERT_FALSE(before.isNull());

    // The invisible OCR span "OCCLUDED", measured from the fixture.
    PdfRect selection;
    selection.x0 = 84.8;
    selection.y0 = 479.6;
    selection.x1 = 161.6;
    selection.y1 = 498.8;

    const ReconstructionResult result = replacer_->replace(
        page_.get(), document_.get(), 0, selection, "REPLACED", RegionId{1});

    auto page2 = document_->page(0);
    ASSERT_NE(page2, nullptr);
    const QImage full1 = page2->render(QRectF(), 1.0f);
    ASSERT_FALSE(full1.isNull());
    const QImage after = full1.copy(topCrop.intersected(full1.rect()));
    const int changed = differingPixels(before, after);

    // The invariant, in the only form that catches the whole family: the report
    // and the pixels must AGREE. Asserting success==false alone would also be
    // satisfied by a recomposite that failed for some unrelated reason.
    EXPECT_EQ(result.success, changed > 0)
        << "ImageReplacer::replace() reported success=" << result.success
        << " while the rendered region changed in " << changed
        << " pixels. A recomposite must not report an outcome its own output "
           "contradicts — that disagreement IS GitHub #262, and the "
           "before/after render comparison in replace() is what forbids it. "
           "errorMessage=" << result.errorMessage;

    // And specifically, on THIS page, the honest answer is failure.
    EXPECT_FALSE(result.success)
        << "the write went to an image that another image is drawn over, so "
           "nothing the user can see changed, yet it was reported as success";
    if (!result.success) {
        EXPECT_NE(result.errorMessage.find("no visible change"), std::string::npos)
            << "the failure is real but the reason does not name it, so a "
               "person reading the log cannot tell this from an ordinary "
               "write error. Got: " << result.errorMessage;
    }
}

// The region-aware lookup must not become a synonym for the old one. A region
// over the photo ALONE has to resolve to the photo, or "always pick the small
// image" would pass the test above while being just as wrong.
TEST_F(RecompositeVisiblePixelsTest, ARegionOverThePhotoAloneStillResolvesToThePhoto)
{
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);

    const auto target = writer_->findImageXObjectForRegion(
        page.get(), document_.get(), QRectF(300, 150, 150, 150));
    ASSERT_TRUE(target.has_value());
    EXPECT_EQ(target->width, 2125)
        << "a region covered only by the 300-DPI photo resolved to something "
           "else; the selection is not region-aware, it is just biased small";
}

// The case the test above CANNOT distinguish, and a mutation sweep proved it.
//
// Mutating the selection rule to "always prefer the smallest overlapping image"
// killed nothing (`P3-M2`). The reason is that the rule already discards images
// which do not overlap the region at all (`if (frac <= 0.0) continue;`), so on
// every region tested so far the candidate set had exactly ONE member: the thumb
// region overlaps both and the smallest happens to be the right answer, and the
// photo-alone region overlaps only the photo. Always-smallest was therefore
// behaviourally identical to the real rule on the whole fixture set — an
// expectation of mine that was wrong, rather than a guard that failed.
//
// This region overlaps BOTH images and the correct answer is the LARGER one: it
// covers the photo over its whole area while clipping only the top-right corner
// of the thumbnail, so coverage-of-the-region — the actual criterion — picks the
// photo, and always-smallest picks the thumbnail.
TEST_F(RecompositeVisiblePixelsTest, ARegionMostlyOverThePhotoResolvesToThePhotoNotTheThumbnail)
{
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);

    // Thumbnail occupies (80,470)-(224,614). This rect clips its top-right
    // corner only: overlap ~24x30 pt, against ~200x100 pt of photo coverage.
    const QRectF region(200, 400, 200, 100);

    const auto target =
        writer_->findImageXObjectForRegion(page.get(), document_.get(), region);
    ASSERT_TRUE(target.has_value());
    EXPECT_EQ(target->width, 2125)
        << "a region that lies almost entirely over the 300-DPI photo, merely "
           "clipping the thumbnail's corner, resolved to the 300px thumbnail. "
           "Selection is picking the smallest overlapping image rather than the "
           "one that actually covers the region.";
}
