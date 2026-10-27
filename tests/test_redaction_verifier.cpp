#include <gtest/gtest.h>
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"
#include "redaction/RedactionManager.h"
#include "redaction/RedactionVerifier.h"
#include "redaction/RedactionData.h"
#include <filesystem>
#include <memory>
#include <fstream>

class RedactionVerifierTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<pdfe::core::MuPdfContext>();
        verifier_ = std::make_unique<pdfe::redaction::RedactionVerifier>(context_);

        pdfPath_ = std::filesystem::path(TEST_FIXTURES_DIR) / "test_redaction.pdf";
        if (!std::filesystem::exists(pdfPath_)) {
            GTEST_SKIP() << "test_redaction.pdf not found";
        }
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    std::unique_ptr<pdfe::redaction::RedactionVerifier> verifier_;
    std::filesystem::path pdfPath_;

    // Helper to create a working copy for destructive tests.
    // The copy is named per TEST (gap #49 lesson: a shared temp path makes the
    // suite non-repeat-safe and lets a leaked handle from one case break the next).
    std::filesystem::path createWorkingCopy() {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        const std::string tag = info ? info->name() : "unknown";
        auto tmpPath = std::filesystem::temp_directory_path() /
                       ("test_redaction_verify_" + tag + ".pdf");
        std::filesystem::copy_file(pdfPath_, tmpPath,
            std::filesystem::copy_options::overwrite_existing);
        return tmpPath;
    }
};

TEST_F(RedactionVerifierTest, ConstructDestroy) {
    auto testVerifier = std::make_unique<pdfe::redaction::RedactionVerifier>(context_);
    ASSERT_NE(testVerifier, nullptr);
    testVerifier.reset();
    SUCCEED();
}

TEST_F(RedactionVerifierTest, VerifyUnmodifiedPdf) {
    auto workingCopy = createWorkingCopy();
    std::vector<pdfe::redaction::RedactionData> redactions;

    auto result = verifier_->verify(workingCopy, redactions);

    EXPECT_TRUE(result.success) << "Error: " << result.errorMessage;
    EXPECT_TRUE(result.textRemoved);
    EXPECT_TRUE(result.noOrphanStreams);
    EXPECT_TRUE(result.singleRevision);
    EXPECT_TRUE(result.errorMessage.empty());

    std::filesystem::remove(workingCopy);
}

TEST_F(RedactionVerifierTest, CheckSingleRevision_UnmodifiedPdf) {
    auto workingCopy = createWorkingCopy();

    bool hasOnlyOneRevision = verifier_->checkSingleRevision(workingCopy);

    EXPECT_TRUE(hasOnlyOneRevision) << "Simple PDF should have single revision";

    std::filesystem::remove(workingCopy);
}

TEST_F(RedactionVerifierTest, CheckNoOrphanStreams_UnmodifiedPdf) {
    auto workingCopy = createWorkingCopy();

    bool noOrphans = verifier_->checkNoOrphanStreams(workingCopy);

    EXPECT_TRUE(noOrphans) << "Clean PDF should have no orphan streams";

    std::filesystem::remove(workingCopy);
}

TEST_F(RedactionVerifierTest, CheckTextRemoved_EmptyRegions) {
    auto workingCopy = createWorkingCopy();
    std::vector<pdfe::redaction::RedactionData> emptyRedactions;

    bool textRemoved = verifier_->checkTextRemoved(workingCopy, emptyRedactions);

    EXPECT_TRUE(textRemoved) << "Empty region list should return true";

    std::filesystem::remove(workingCopy);
}

TEST_F(RedactionVerifierTest, CheckTextRemoved_NoTextInRegion) {
    auto workingCopy = createWorkingCopy();
    std::vector<pdfe::redaction::RedactionData> redactions;

    // Create a redaction region in an area with no text (far off-page)
    pdfe::redaction::RedactionData emptyRegion;
    emptyRegion.pageIndex = 0;
    emptyRegion.rect = QRectF(10000.0, 10000.0, 10.0, 10.0);
    redactions.push_back(emptyRegion);

    bool textRemoved = verifier_->checkTextRemoved(workingCopy, redactions);

    EXPECT_TRUE(textRemoved) << "Region with no text should return true";

    std::filesystem::remove(workingCopy);
}

// A3-19. This test used to read:
//     EXPECT_TRUE(result.success || !result.success);
//     EXPECT_GE(result.errorMessage.length(), 0u);
// Four tautologies of the form `X || !X` and two of the form `unsigned >= 0`.
// Not one of them could fail for any implementation of verify(), including one
// that returned a default-constructed struct.
//
// The verifier's real contract has two poles, and the suite only had the
// positive one. These give it the NEGATIVE pole: a region that still contains
// glyphs must be reported as NOT text-removed, and that must propagate into
// success == false with a warning — otherwise a redaction that removed nothing
// would be verified as successful, which is the whole point of the verifier.
TEST_F(RedactionVerifierTest, VerifyFailsWhenTheRegionStillContainsText) {
    auto workingCopy = createWorkingCopy();

    // The working copy is the ORIGINAL, unredacted fixture: nothing has been
    // removed from it. A region covering the whole first page therefore still
    // contains every glyph on that page.
    pdfe::redaction::RedactionData wholePage;
    wholePage.pageIndex = 0;
    wholePage.rect = QRectF(0.0, 0.0, 5000.0, 5000.0);
    std::vector<pdfe::redaction::RedactionData> redactions{wholePage};

    // Guard the guard: if the fixture had no text on page 0 the check below
    // would pass vacuously.
    {
        auto doc = pdfe::core::Document::open(context_, workingCopy);
        ASSERT_NE(doc, nullptr);
        ASSERT_GT(doc->pageCount(), 0);
        auto page = doc->page(0);
        ASSERT_NE(page, nullptr);
        ASSERT_FALSE(page->extractPlainText().empty())
            << "test_redaction.pdf page 0 must carry text for this oracle to mean "
               "anything";
    }

    EXPECT_FALSE(verifier_->checkTextRemoved(workingCopy, redactions))
        << "text that is demonstrably still on the page must not be reported as "
           "removed";

    const auto result = verifier_->verify(workingCopy, redactions);
    EXPECT_FALSE(result.textRemoved);
    EXPECT_FALSE(result.success)
        << "verify() must not report success while text survives in a redacted "
           "region";
    EXPECT_FALSE(result.warnings.empty())
        << "a surviving-text verdict must carry a warning the caller can show";

    std::filesystem::remove(workingCopy);
}

// The positive pole of the same contract, stated as equalities rather than as
// `X || !X`: on an untouched document with no regions claimed, every field is
// affirmatively true and the error message is empty.
TEST_F(RedactionVerifierTest, VerifyResultFieldsAreAffirmativeOnACleanFile) {
    auto workingCopy = createWorkingCopy();
    std::vector<pdfe::redaction::RedactionData> redactions;

    auto result = verifier_->verify(workingCopy, redactions);

    EXPECT_TRUE(result.success) << "error was: " << result.errorMessage;
    EXPECT_TRUE(result.textRemoved);
    EXPECT_TRUE(result.noOrphanStreams);
    EXPECT_TRUE(result.singleRevision);
    EXPECT_EQ(result.errorMessage, "");
    EXPECT_TRUE(result.warnings.empty())
        << "a clean verdict must not carry warnings";

    std::filesystem::remove(workingCopy);
}

TEST_F(RedactionVerifierTest, CheckSingleRevision_Invalid) {
    std::filesystem::path nonExistentPath = "C:\\nonexistent\\fake_file.pdf";

    bool hasOnlyOneRevision = verifier_->checkSingleRevision(nonExistentPath);

    EXPECT_FALSE(hasOnlyOneRevision) << "Non-existent file should return false";
}

TEST_F(RedactionVerifierTest, CheckNoOrphanStreams_Invalid) {
    std::filesystem::path nonExistentPath = "C:\\nonexistent\\fake_file.pdf";

    bool noOrphans = verifier_->checkNoOrphanStreams(nonExistentPath);

    EXPECT_FALSE(noOrphans) << "Non-existent file should return false";
}

// ─── Gap #18 (pipeline b3f19d72 phase-15): fail-path tests for each ─────────
// checkXxx method using a non-PDF stub file (a real, existing file that is
// not a PDF). This exercises the file-open-failure branches that the
// previous tests (which used non-existent paths) did not cover, because
// std::filesystem::exists() returns true and the QPDF/MuPDF open call fails
// on corrupt-magic content.
// ─────────────────────────────────────────────────────────────────────────────

class RedactionVerifierFailPathTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<pdfe::core::MuPdfContext>();
        verifier_ = std::make_unique<pdfe::redaction::RedactionVerifier>(context_);

        // Create a stub file that exists on disk but is not a valid PDF.
        stubPath_ = std::filesystem::temp_directory_path()
                    / "test_redaction_verifier_stub_not_a_pdf.txt";
        std::ofstream f(stubPath_, std::ios::out | std::ios::binary);
        ASSERT_TRUE(f.is_open()) << "Could not create stub file";
        f << "This is not a PDF file. %PDF- header is missing intentionally.\n";
        f.close();
        ASSERT_TRUE(std::filesystem::exists(stubPath_))
            << "Stub file was not created";
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove(stubPath_, ec);
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    std::unique_ptr<pdfe::redaction::RedactionVerifier> verifier_;
    std::filesystem::path stubPath_;
};

TEST_F(RedactionVerifierFailPathTest, CheckSingleRevision_NonPdfFile_ReturnsFalse) {
    // The stub file exists but is not a PDF.  checkSingleRevision must
    // return false (not crash) when QPDF or MuPDF cannot parse it.
    ASSERT_TRUE(std::filesystem::exists(stubPath_));
    bool result = verifier_->checkSingleRevision(stubPath_);
    EXPECT_FALSE(result)
        << "checkSingleRevision must return false for a non-PDF stub file";
}

TEST_F(RedactionVerifierFailPathTest, CheckNoOrphanStreams_NonPdfFile_ReturnsFalse) {
    ASSERT_TRUE(std::filesystem::exists(stubPath_));
    bool result = verifier_->checkNoOrphanStreams(stubPath_);
    EXPECT_FALSE(result)
        << "checkNoOrphanStreams must return false for a non-PDF stub file";
}

TEST_F(RedactionVerifierFailPathTest, CheckTextRemoved_EmptyRegions_NonPdfFile_ReturnsTrue) {
    // checkTextRemoved with empty regions: the implementation has an early
    // return `if (redactedRegions.empty()) return true;` which fires before
    // any file-open attempt.  The non-PDF stub exercises the path where the
    // fast-path guard is the only code reached; the method must return true
    // without attempting to open the file.
    ASSERT_TRUE(std::filesystem::exists(stubPath_));
    std::vector<pdfe::redaction::RedactionData> emptyRegions;
    bool result = verifier_->checkTextRemoved(stubPath_, emptyRegions);
    EXPECT_TRUE(result)
        << "checkTextRemoved must return true when regions are empty "
           "(fast-path guard fires before any file-open attempt)";
}

TEST_F(RedactionVerifierFailPathTest, CheckTextRemoved_NonEmptyRegions_NonPdfFile_ThrowsOrReturnsFalse) {
    // checkTextRemoved with a non-empty region on a non-PDF file: MuPDF
    // throws when attempting to repair the corrupt xref.  The test verifies
    // that calling this method with a non-PDF file either (a) returns false
    // or (b) propagates a MuPDF exception — either behavior is acceptable
    // because the caller (verify()) catches the exception and sets
    // result.success = false.  This test uses EXPECT_THROW or EXPECT_FALSE
    // to lock whichever branch the current implementation takes.
    ASSERT_TRUE(std::filesystem::exists(stubPath_));
    pdfe::redaction::RedactionData d;
    d.pageIndex = 0;
    d.rect = QRectF(0.0, 0.0, 100.0, 100.0);
    bool threw = false;
    bool result = false;
    try {
        result = verifier_->checkTextRemoved(stubPath_, {d});
    } catch (...) {
        threw = true;
    }
    // Either the method threw (which verify() handles) or returned false.
    // In both cases the non-PDF file is correctly not treated as "clean."
    if (!threw) {
        EXPECT_FALSE(result)
            << "If checkTextRemoved does not throw for a non-PDF file with "
               "non-empty regions, it must return false to signal failure.";
    }
    // threw == true is also acceptable (covered by FullVerify test above).
}

TEST_F(RedactionVerifierFailPathTest, FullVerify_NonPdfFile_ReturnsFailure) {
    ASSERT_TRUE(std::filesystem::exists(stubPath_));
    std::vector<pdfe::redaction::RedactionData> emptyRegions;
    auto result = verifier_->verify(stubPath_, emptyRegions);
    EXPECT_FALSE(result.success)
        << "verify() must return success=false for a non-PDF stub file";
}

// ─── M11 kill (redaction confidentiality, R-02/R-15) ───────────────────────
// The previous unit oracles constrained checkSingleRevision() with tautologies
// (EXPECT_TRUE(x || !x)); no test asserted it returns FALSE on a genuinely
// multi-revision file, so a mutant forcing `return true;` survived. These tests
// feed committed fixtures that exercise the real fail paths:
//   * redaction_multi_revision.pdf — a chained /Prev incremental update whose
//     object 3 survives at GENERATION 1 → checkSingleRevision must be FALSE.
//   * redaction_partial_overlap.pdf — visible, un-redacted "SECRET" text with a
//     region that overlaps it → checkTextRemoved must be FALSE (residual text).
// Both run UNCONDITIONALLY (fixtures are pinned by the CMake fixture floor).
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(RedactionVerifierFailPathTest, CheckSingleRevision_MultiRevisionPdf_ReturnsFalse) {
    std::filesystem::path multiRev =
        std::filesystem::path(TEST_FIXTURES_DIR) / "redaction_multi_revision.pdf";
    ASSERT_TRUE(std::filesystem::exists(multiRev))
        << "multi-revision fixture must be present (CMake fixture floor): "
        << multiRev;

    // A genuine incremental-update file keeps a live object at generation 1, so
    // previous (pre-redaction) data may be recoverable: the verifier MUST report
    // it is NOT a single clean revision.
    EXPECT_FALSE(verifier_->checkSingleRevision(multiRev))
        << "checkSingleRevision must return false for a multi-revision PDF whose "
           "object survives at generation 1 (residual data recoverable). A mutant "
           "forcing `return true;` is killed here.";
}

TEST_F(RedactionVerifierFailPathTest, CheckTextRemoved_ResidualOverlappingText_ReturnsFalse) {
    std::filesystem::path leaky =
        std::filesystem::path(TEST_FIXTURES_DIR) / "redaction_partial_overlap.pdf";
    ASSERT_TRUE(std::filesystem::exists(leaky))
        << "leaky-text fixture must be present (CMake fixture floor): " << leaky;

    // The fixture's visible "SECRET" glyphs were never removed. A redaction
    // region that overlaps them means residual sensitive text survives, so
    // checkTextRemoved MUST report the region NOT clear.
    // Region in device space (y-down, top-left origin) covering the whole page,
    // so it overlaps the visible "SECRET" glyphs wherever they render.
    pdfe::redaction::RedactionData region;
    region.pageIndex = 0;
    region.rect = QRectF(0.0, 0.0, 612.0, 792.0);

    EXPECT_FALSE(verifier_->checkTextRemoved(leaky, {region}))
        << "checkTextRemoved must return false when un-redacted text still "
           "overlaps the redaction region (residual-secret leak).";
}

TEST_F(RedactionVerifierTest, FullVerification_UnmodifiedPdf) {
    auto workingCopy = createWorkingCopy();
    std::vector<pdfe::redaction::RedactionData> redactions;

    auto result = verifier_->verify(workingCopy, redactions);

    ASSERT_TRUE(result.success) << "Verification should succeed";

    EXPECT_TRUE(result.textRemoved) << "Text removal check should pass";
    EXPECT_TRUE(result.noOrphanStreams) << "Orphan stream check should pass";
    EXPECT_TRUE(result.singleRevision) << "Single revision check should pass";

    EXPECT_TRUE(result.errorMessage.empty())
        << "Error message should be empty, got: " << result.errorMessage;

    for (const auto& warning : result.warnings) {
        EXPECT_FALSE(warning.empty()) << "Warning messages should not be empty strings";
    }

    std::filesystem::remove(workingCopy);
}
