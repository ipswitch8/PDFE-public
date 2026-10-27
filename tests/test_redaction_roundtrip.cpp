// PHASE5_ROUNDTRIP_REDACTION: mark -> apply -> save -> reopen ->
// pixel + text assertions. Closes the audit's gap that
// test_redaction_manager.cpp asserts internal annotIndex / pageIndex
// state but never confirms the redacted region is actually opaque
// in the saved PDF or that the original text is non-extractable.

#include <gtest/gtest.h>

#include "redaction/RedactionManager.h"
#include "redaction/RedactionData.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/TextBlock.h"

#include "helpers/RoundTripHelper.h"

#include <QImage>

#include <filesystem>
#include <memory>
#include <string>

using pdfe::redaction::RedactionApplyOptions;
using pdfe::redaction::RedactionManager;

namespace {

class RedactionRoundTripTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<pdfe::core::MuPdfContext>();
        fixture_ = std::filesystem::path(TEST_FIXTURES_DIR) / "test_redaction.pdf";
        ASSERT_TRUE(std::filesystem::exists(fixture_));
        document_ = pdfe::core::Document::open(ctx_, fixture_);
        ASSERT_NE(document_, nullptr);
        manager_ = std::make_unique<RedactionManager>(ctx_);

        const ::testing::TestInfo* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        tempDir_ = std::filesystem::temp_directory_path()
                   / "pdfe_redact_roundtrip" / info->name();
        std::filesystem::create_directories(tempDir_);
    }

    void TearDown() override {
        manager_.reset();
        document_.reset();
        ctx_.reset();
    }

    /// Sample mean RGB inside a sub-rectangle of a QImage. Returns
    /// {r,g,b} averages. Inputs in image-local pixel coordinates.
    struct Rgb { int r; int g; int b; };
    Rgb sampleMean(const QImage& img, QRect roi) {
        roi = roi.intersected(img.rect());
        if (roi.isEmpty()) return {0, 0, 0};
        long long rs = 0, gs = 0, bs = 0;
        long long count = 0;
        // Sample on a coarse grid; redaction fills are uniform so a
        // 4-pixel stride is sufficient and keeps the test fast.
        for (int y = roi.top(); y <= roi.bottom(); y += 4) {
            for (int x = roi.left(); x <= roi.right(); x += 4) {
                const QColor c = img.pixelColor(x, y);
                rs += c.red();
                gs += c.green();
                bs += c.blue();
                ++count;
            }
        }
        if (count == 0) return {0, 0, 0};
        return {static_cast<int>(rs / count),
                static_cast<int>(gs / count),
                static_cast<int>(bs / count)};
    }

    std::shared_ptr<pdfe::core::MuPdfContext> ctx_;
    std::filesystem::path fixture_;
    std::filesystem::path tempDir_;
    std::unique_ptr<pdfe::core::Document> document_;
    std::unique_ptr<RedactionManager> manager_;
};

// ─── Pixel assertion: redacted region is opaque black ───────────────
//
// SCOPE (meta-audit 2026-07, gap PDF-69): this is a RENDERING check, not a
// removal oracle. It asserts only that the redaction fill is painted opaque
// black — a black rectangle drawn over live, still-extractable text would
// pass it. It is retained deliberately, because "the fill actually renders"
// is worth guarding, but it must never be cited as evidence that content was
// removed.
//
// The true-removal oracles live elsewhere and assert byte-absence in the
// decompressed streams of a saved-and-reopened file:
//   - tests/test_redaction_confidentiality_surfaces.cpp (all document and
//     page confidentiality surfaces)
//   - RedactedTextAbsentFromBytes                       (page text)
//   - ImagePixelsUnderRedactionAreDestroyed             (image rasters)

TEST_F(RedactionRoundTripTest,
       RedactedRegionRendersBlack_VisualOnly_NotARemovalOracle) {
    auto page = document_->page(0);
    QRectF redactRect(50.0, 50.0, 200.0, 30.0);
    manager_->createRedactionMark(page.get(), redactRect);
    bool ok = manager_->applyPageRedactions(page.get());
    ASSERT_TRUE(ok)
        << "applyPageRedactions must succeed when at least one mark exists";
    page.reset();

    auto reopened = pdfe::test::SaveAndReopen(*document_, ctx_, tempDir_);
    ASSERT_NE(reopened, nullptr);
    auto rePage = reopened->page(0);
    ASSERT_NE(rePage, nullptr);

    // Render the page at 72 DPI = 1px/pt for direct PDF-rect mapping.
    auto img = pdfe::test::RenderPage(*rePage, 72.0);
    ASSERT_FALSE(img.isNull()) << "RenderPage produced a null image";

    // Sample within the redacted rect (slight inset to avoid AA at
    // the edges). The default redaction fill is black; tolerate ±5
    // per channel for AA / dithering.
    QRect sampleRoi(static_cast<int>(redactRect.left() + 4),
                    static_cast<int>(redactRect.top() + 4),
                    static_cast<int>(redactRect.width() - 8),
                    static_cast<int>(redactRect.height() - 8));
    Rgb mean = sampleMean(img, sampleRoi);
    EXPECT_LE(mean.r, 5) << "Redacted region red mean must be near 0";
    EXPECT_LE(mean.g, 5) << "Redacted region green mean must be near 0";
    EXPECT_LE(mean.b, 5) << "Redacted region blue mean must be near 0";
}

// ─── Text extraction: redacted text is gone ──────────────────────────

TEST_F(RedactionRoundTripTest, RedactedTextIsNotExtractableAfterApply) {
#ifndef _WIN32
    // text_redaction.pdf's "Confidential" string isn't fully
    // extractable on Linux — some glyphs drop because the font
    // ToUnicode CMap path relies on Windows-side font resolution
    // (SystemFontDiscovery). Pre-condition fails with
    // "got: ocument" instead of "Confidential Document".
    // Track under linux_port_plan.md font-extraction follow-up.
    GTEST_SKIP() << "Text extraction font-decode path needs Linux impl";
#endif
    auto page = document_->page(0);

    // Capture the original extracted text on page 0 so we can check
    // a substring disappears after redaction.
    std::string preText = page->extractPlainText();
    ASSERT_FALSE(preText.empty())
        << "test_redaction.pdf page 0 must have extractable text";

    // Phase-16 (pipeline a7c2e541) closeout: previously this test
    // only asserted `postText.size() < preText.size()`. We now lock
    // exact substring absence: the fixture's first line "Confidential
    // Document" sits at PDF y=66.2-99.2 (verified via PyMuPDF
    // get_text('words')); the redaction rect (50, 50)-(350, 100)
    // covers both words. The pre-text must contain "Confidential"
    // and the post-text must NOT.
    const std::string kTarget = "Confidential";
    ASSERT_NE(preText.find(kTarget), std::string::npos)
        << "Pre-condition: fixture must contain 'Confidential' in "
           "page 0 extracted text; got: " << preText;

    manager_->createRedactionMark(page.get(),
                                   QRectF(50.0, 50.0, 300.0, 50.0));
    manager_->applyPageRedactions(page.get());
    page.reset();

    auto reopened = pdfe::test::SaveAndReopen(*document_, ctx_, tempDir_);
    ASSERT_NE(reopened, nullptr);
    auto rePage = reopened->page(0);
    std::string postText = rePage->extractPlainText();

    EXPECT_EQ(postText.find(kTarget), std::string::npos)
        << "Redacted region covers the 'Confidential' word but the "
           "string is still present in extracted text after apply+save+"
           "reopen; got: " << postText;
    // Keep the byte-count delta as a defense-in-depth check.
    EXPECT_LT(postText.size(), preText.size())
        << "Redacted text must also reduce page extracted-text length";
}

// ─── Multi-region redaction round-trip ─────────────────────────────

TEST_F(RedactionRoundTripTest, MultipleRedactionsAllSurviveSave) {
    auto page = document_->page(0);
    manager_->createRedactionMark(page.get(), QRectF(50, 50, 100, 30));
    manager_->createRedactionMark(page.get(), QRectF(200, 50, 100, 30));
    manager_->createRedactionMark(page.get(), QRectF(50, 100, 100, 30));
    manager_->applyPageRedactions(page.get());
    page.reset();

    auto reopened = pdfe::test::SaveAndReopen(*document_, ctx_, tempDir_);
    ASSERT_NE(reopened, nullptr);
    auto rePage = reopened->page(0);
    auto img = pdfe::test::RenderPage(*rePage, 72.0);
    ASSERT_FALSE(img.isNull());

    // Each of the three redacted regions must show as black.
    const std::vector<QRect> rois = {
        QRect(54,  54, 92, 22),
        QRect(204, 54, 92, 22),
        QRect(54, 104, 92, 22),
    };
    for (size_t i = 0; i < rois.size(); ++i) {
        Rgb mean = sampleMean(img, rois[i]);
        EXPECT_LE(mean.r, 5) << "ROI " << i << " red";
        EXPECT_LE(mean.g, 5) << "ROI " << i << " green";
        EXPECT_LE(mean.b, 5) << "ROI " << i << " blue";
    }
}

// ─── Partial-page redaction leaves rest of page intact ──────────────

TEST_F(RedactionRoundTripTest, PartialRedactionLeavesUnredactedAreaIntact) {
    // Render the page BEFORE redaction so we have a per-pixel
    // baseline. test_redaction.pdf has unknown ink density at the
    // bottom of the page; we don't assume "white" — we assume
    // "same as the unredacted baseline".
    auto preRedactedDoc = pdfe::core::Document::open(ctx_, fixture_);
    ASSERT_NE(preRedactedDoc, nullptr);
    auto prePage = preRedactedDoc->page(0);
    auto preImg = pdfe::test::RenderPage(*prePage, 72.0);
    ASSERT_FALSE(preImg.isNull());

    auto page = document_->page(0);
    manager_->createRedactionMark(page.get(),
                                   QRectF(50, 50, 100, 30));
    manager_->applyPageRedactions(page.get());
    page.reset();

    auto reopened = pdfe::test::SaveAndReopen(*document_, ctx_, tempDir_);
    ASSERT_NE(reopened, nullptr);
    auto rePage = reopened->page(0);
    auto postImg = pdfe::test::RenderPage(*rePage, 72.0);
    ASSERT_FALSE(postImg.isNull());

    // Redacted region: black after redaction.
    Rgb redacted = sampleMean(postImg, QRect(54, 54, 92, 22));
    EXPECT_LE(redacted.r, 5);
    EXPECT_LE(redacted.g, 5);
    EXPECT_LE(redacted.b, 5);

    // OUTSIDE the redaction rect (right side, past x=200), the
    // before/after pixel means must match — the redaction must NOT
    // bleed into unrelated regions. Use a sample area at (200,40,
    // 100,40): same vertical band as the redaction but well clear
    // of x=50..150.
    QRect outsideRoi(200, 40, 100, 40);
    Rgb preMean  = sampleMean(preImg,  outsideRoi);
    Rgb postMean = sampleMean(postImg, outsideRoi);
    EXPECT_LE(std::abs(preMean.r - postMean.r), 5)
        << "Outside-redaction red mean drifted: pre=" << preMean.r
        << " post=" << postMean.r;
    EXPECT_LE(std::abs(preMean.g - postMean.g), 5)
        << "Outside-redaction green drifted: pre=" << preMean.g
        << " post=" << postMean.g;
    EXPECT_LE(std::abs(preMean.b - postMean.b), 5)
        << "Outside-redaction blue drifted: pre=" << preMean.b
        << " post=" << postMean.b;
}

// ─── Mark count after applyPageRedactions is zero ───────────────────

TEST_F(RedactionRoundTripTest, ApplyClearsMarksAfterReopen) {
    auto page = document_->page(0);
    manager_->createRedactionMark(page.get(), QRectF(50, 50, 100, 30));
    manager_->createRedactionMark(page.get(), QRectF(50, 100, 100, 30));
    auto preMarks = manager_->getRedactionMarks(page.get());
    ASSERT_EQ(preMarks.size(), 2u);

    manager_->applyPageRedactions(page.get());
    page.reset();

    auto reopened = pdfe::test::SaveAndReopen(*document_, ctx_, tempDir_);
    ASSERT_NE(reopened, nullptr);
    auto rePage = reopened->page(0);
    auto postMarks = manager_->getRedactionMarks(rePage.get());
    EXPECT_EQ(postMarks.size(), 0u)
        << "After applyPageRedactions + save + reopen, no redaction "
           "marks should remain (they were applied and cleared)";
}

// ─── Real-world fixture: corrupt_xref_recoverable ────────────────────

TEST_F(RedactionRoundTripTest, RedactionOnCorruptXrefRealworldFixture) {
    auto rwPath = std::filesystem::path(TEST_FIXTURES_DIR) / "realworld"
                  / "corrupt_xref" / "corrupt_xref_recoverable.pdf";
    ASSERT_TRUE(std::filesystem::exists(rwPath));

    auto rwDoc = pdfe::core::Document::open(ctx_, rwPath);
    ASSERT_NE(rwDoc, nullptr)
        << "MuPDF must xref-repair the fixture and open it";
    ASSERT_GE(rwDoc->pageCount(), 1);

    auto page = rwDoc->page(0);
    manager_->createRedactionMark(page.get(),
                                   QRectF(50, 50, 100, 30));
    manager_->applyPageRedactions(page.get());
    page.reset();

    auto reopened = pdfe::test::SaveAndReopen(*rwDoc, ctx_, tempDir_,
                                               "rw_redact");
    ASSERT_NE(reopened, nullptr)
        << "Save+reopen must work on a redacted, repaired-xref PDF";
    EXPECT_EQ(reopened->pageCount(), rwDoc->pageCount())
        << "Page count must survive redaction on a repaired-xref PDF";
}

} // namespace
