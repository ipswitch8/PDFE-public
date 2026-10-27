// Defect #6 — per-run rotation decomposition in CoordMapper.
//
// The page-level transform applies the page /Rotate to every region because
// that rotation is baked into the rendered bitmap OCR sees. But a text run
// whose OWN text matrix (Tm) already encodes a rotation would be rotated twice
// if the full page /Rotate were re-applied on top. CoordMapper::computeRunTransform
// fixes this by decomposing the run's rotation from its Tm*CTM linear part and
// applying only the residual page rotation.
//
// These tests lock the decomposition + residual math (pure, page-free) and the
// integration through computeRunTransform on a real page.

#include <gtest/gtest.h>

#include "tre/CoordMapper.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"

#include <filesystem>
#include <memory>

using namespace pdfe::tre;
using namespace pdfe::core;

// ---------------------------------------------------------------------------
// Pure decomposition: which rotation does a run's Tm*CTM linear part encode?
// fitz (y-down) rotation matrices: R(theta) = [cos, sin, -sin, cos].
//   0   deg -> [ 1,  0,  0,  1]
//   90  deg -> [ 0,  1, -1,  0]
//   180 deg -> [-1,  0,  0, -1]
//   270 deg -> [ 0, -1,  1,  0]
// ---------------------------------------------------------------------------

TEST(CoordMapperRotation, DecomposeIdentityIsZero) {
    EXPECT_EQ(CoordMapper::decomposeRotationDegrees(1, 0, 0, 1), 0);
}

TEST(CoordMapperRotation, DecomposeQuarterTurn) {
    EXPECT_EQ(CoordMapper::decomposeRotationDegrees(0, 1, -1, 0), 90);
}

TEST(CoordMapperRotation, DecomposeHalfTurn) {
    EXPECT_EQ(CoordMapper::decomposeRotationDegrees(-1, 0, 0, -1), 180);
}

TEST(CoordMapperRotation, DecomposeThreeQuarterTurn) {
    EXPECT_EQ(CoordMapper::decomposeRotationDegrees(0, -1, 1, 0), 270);
}

TEST(CoordMapperRotation, DecomposeScaledRotationStillSnaps) {
    // A run scaled 12pt and rotated 90deg keeps the rotation angle; scale
    // must not change the decomposed rotation.
    EXPECT_EQ(CoordMapper::decomposeRotationDegrees(0, 12, -12, 0), 90);
}

TEST(CoordMapperRotation, DecomposeDegenerateMatrixIsZero) {
    EXPECT_EQ(CoordMapper::decomposeRotationDegrees(0, 0, 0, 0), 0);
}

// ---------------------------------------------------------------------------
// Residual: residual = normalize(pageRotation - runRotation). The four cases
// the defect enumerates.
// ---------------------------------------------------------------------------

// Case 1: Tm-rotation matches page rotation -> NO double-rotate (residual 0).
TEST(CoordMapperRotation, ResidualRunMatchesPage_NoDoubleRotate) {
    EXPECT_EQ(CoordMapper::residualRotationDegrees(/*page=*/90, /*run=*/90), 0);
    EXPECT_EQ(CoordMapper::residualRotationDegrees(270, 270), 0);
}

// Case 2: Tm-rotation zero on a rotated page -> page rotate applies once.
TEST(CoordMapperRotation, ResidualRunZeroOnRotatedPage_PageRotateOnce) {
    EXPECT_EQ(CoordMapper::residualRotationDegrees(/*page=*/90, /*run=*/0), 90);
    EXPECT_EQ(CoordMapper::residualRotationDegrees(270, 0), 270);
}

// Case 3: Tm-rotation orthogonal to page rotation -> combined as the residual.
TEST(CoordMapperRotation, ResidualRunOrthogonalToPage_Combined) {
    // page 180, run 90 -> residual 90.
    EXPECT_EQ(CoordMapper::residualRotationDegrees(180, 90), 90);
    // page 90, run 180 -> residual -90 -> 270.
    EXPECT_EQ(CoordMapper::residualRotationDegrees(90, 180), 270);
}

// Case 4: zero-rotation page with non-zero Tm rotation -> Tm rotation only
// (residual subtracts the run's rotation so the page transform does not
// re-rotate; the run already carries it).
TEST(CoordMapperRotation, ResidualZeroPageNonZeroRun_RunRotationOnly) {
    EXPECT_EQ(CoordMapper::residualRotationDegrees(/*page=*/0, /*run=*/90), 270);
    EXPECT_EQ(CoordMapper::residualRotationDegrees(0, 270), 90);
}

// ---------------------------------------------------------------------------
// Integration on a real page: computeRunTransform threads decompose+residual.
// ---------------------------------------------------------------------------

class CoordMapperRunTransformTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<MuPdfContext>();
        std::filesystem::path testPdf =
            std::filesystem::path(TEST_FIXTURES_DIR) / "test_single.pdf";
        if (!std::filesystem::exists(testPdf)) {
            GTEST_SKIP() << "Test fixture not available: " << testPdf;
        }
        document_ = Document::open(context_, testPdf);
        if (!document_ || document_->pageCount() == 0) {
            GTEST_SKIP() << "Failed to open test_single.pdf";
        }
    }

    std::shared_ptr<MuPdfContext> context_;
    std::shared_ptr<Document> document_;
};

TEST_F(CoordMapperRunTransformTest, NonRotatedRunMatchesPageTransform) {
    CoordMapper mapper(context_.get(), document_.get());
    const AffineTransform& pageTf = mapper.getTransform(0);

    // A run with an identity-rotation Tm on a (rotation 0) page must produce the
    // same effective rotation as the plain page transform — no behavior change.
    AffineTransform runTf = mapper.computeRunTransform(0, 1, 0, 0, 1);
    EXPECT_EQ(runTf.rotation, pageTf.rotation);
    EXPECT_DOUBLE_EQ(runTf.scaleX, pageTf.scaleX);
    EXPECT_DOUBLE_EQ(runTf.scaleY, pageTf.scaleY);
}

TEST_F(CoordMapperRunTransformTest, SelfRotatedRunSubtractsItsOwnRotation) {
    CoordMapper mapper(context_.get(), document_.get());
    // test_single.pdf is a /Rotate 0 page. A run whose Tm encodes a 90deg turn
    // already carries that rotation, so the residual page rotation must back it
    // out: residual = 0 - 90 -> 270. (Plain getTransform would leave it at 0,
    // double-counting the run's own rotation when the run box is mapped.)
    AffineTransform runTf = mapper.computeRunTransform(0, 0, 1, -1, 0);
    EXPECT_EQ(runTf.rotation, 270);
}
