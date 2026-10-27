// F15 / gap PDF-52 — annotation geometry and appearance under page /Rotate.
//
// WHAT WAS BROKEN
// ---------------
// "rotat" had ZERO hits across all three annotation test files. Rotation is
// the classic annotation-placement defect: an annotation lands 90 degrees off,
// or off-page entirely, and nothing notices. tests/fixtures/
// coord_rotation_page.pdf existed but carried only /Rotate 90 and no
// annotation test used it, so this phase authors a four-page fixture covering
// 0 / 90 / 180 / 270 with ONE identical, user-space-authored /Square
// annotation per page.
//
// WHY THE FIXTURE AUTHORS THE ANNOTATION (and the test does not create it)
// -----------------------------------------------------------------------
// MuPDF's annotation setters take rects in the page's *transformed* space, so
// an annotation created through AnnotationManager on a rotated page is stored
// pre-rotated: create-then-render is rotation-invariant BY CONSTRUCTION and
// proves nothing. The real question — "does an annotation authored by another
// producer land in the right place when we open a rotated page?" — can only be
// asked of a fixture whose /Rect is identical, in USER space, on all four
// pages. That is what gen_f15_annot_rotation.py writes.
//
// THE ORACLE — two independent halves per rotation, as the phase requires
// ----------------------------------------------------------------------
// 1. RECT. (a) The authored user-space /Rect must be IDENTICAL on all four
//    pages in the raw bytes, both in the fixture and after a PDFE save — a
//    product that "helpfully" pre-rotates /Rect corrupts the file for every
//    other reader. (b) The page-space rect PDFE reports must be the authored
//    rect put through the page rotation, computed here from the AUTHORED
//    transform.
// 2. APPEARANCE ORIENTATION. The annotation's rendered ink must land where the
//    rotation says. Expectations are computed from the authored transform
//    applied to the UNROTATED page's measured centroid:
//
//        R=90 (clockwise):  (x, y) -> (H0 - y, x)
//        R=180:             (x, y) -> (W0 - x, H0 - y)
//        R=270:             (x, y) -> (y, W0 - x)
//
// MUTATIONS KILLED (each confirmed RED then reverted):
//   * swap the expected transforms for R=90 and R=270 -> RED (the assertion is
//     orientation-sensitive, not a tautology).
//   * author the fixture rect pre-rotated on page 1 -> AuthoredUserSpaceRect...
//     RED.

#include <gtest/gtest.h>

#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationData.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include "helpers/RoundTripHelper.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <QImage>
#include <QPointF>

#include <array>
#include <filesystem>
#include <memory>
#include <vector>

using pdfe::annotations::AnnotationData;
using pdfe::annotations::AnnotationManager;
using pdfe::annotations::AnnotationType;

namespace {

// AUTHORED literals — must match tests/fixtures/gen_f15_annot_rotation.py.
constexpr double kPageW = 400.0;
constexpr double kPageH = 600.0;
const int kRotations[4] = {0, 90, 180, 270};
// /Rect in USER space (y up), identical on every page.
constexpr double kUserX0 = 40.0, kUserY0 = 500.0, kUserX1 = 160.0,
                 kUserY1 = 540.0;

struct InkCentroid {
    bool found = false;
    double x = 0.0;
    double y = 0.0;
    long count = 0;
};

// Centroid of RED ink (the annotation's fill), ignoring the white page.
InkCentroid CentroidOfRed(const QImage& img) {
    InkCentroid c;
    double sx = 0, sy = 0;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            const QRgb p = img.pixel(x, y);
            if (qRed(p) > 150 && qGreen(p) < 120 && qBlue(p) < 120) {
                sx += x;
                sy += y;
                ++c.count;
            }
        }
    }
    if (c.count > 0) {
        c.found = true;
        c.x = sx / c.count;
        c.y = sy / c.count;
    }
    return c;
}

QPointF ExpectedCentroid(int rotation, const InkCentroid& base,
                         double w0, double h0) {
    switch (rotation) {
        case 0:   return QPointF(base.x, base.y);
        case 90:  return QPointF(h0 - base.y, base.x);
        case 180: return QPointF(w0 - base.x, h0 - base.y);
        case 270: return QPointF(base.y, w0 - base.x);
    }
    return QPointF(-1, -1);
}

// The page-space (y-down, post-rotation) rect the authored user-space rect
// must map to, computed from the AUTHORED transform — not from product output.
QRectF ExpectedPageSpaceRect(int rotation) {
    // Unrotated page space: x unchanged, y flipped about the page height.
    const double x0 = kUserX0, x1 = kUserX1;
    const double y0 = kPageH - kUserY1, y1 = kPageH - kUserY0;
    switch (rotation) {
        case 0:
            return QRectF(QPointF(x0, y0), QPointF(x1, y1));
        case 90:
            // (x, y) -> (H0 - y, x)
            return QRectF(QPointF(kPageH - y1, x0), QPointF(kPageH - y0, x1));
        case 180:
            return QRectF(QPointF(kPageW - x1, kPageH - y1),
                          QPointF(kPageW - x0, kPageH - y0));
        case 270:
            // (x, y) -> (y, W0 - x)
            return QRectF(QPointF(y0, kPageW - x1), QPointF(y1, kPageW - x0));
    }
    return QRectF();
}

// Raw /Rect of annotation 0 on `pageIndex`, from the file's own bytes.
std::array<double, 4> ReadRawRect(const std::filesystem::path& path,
                                  int pageIndex, bool* ok) {
    std::array<double, 4> r{0, 0, 0, 0};
    *ok = false;
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx) return r;
    pdf_document* doc = nullptr;
    pdf_page* page = nullptr;
    bool good = false;
    fz_try(ctx) {
        fz_register_document_handlers(ctx);
        doc = pdf_open_document(ctx, path.string().c_str());
        page = pdf_load_page(ctx, doc, pageIndex);
        pdf_obj* annots = pdf_dict_get(ctx, page->obj, PDF_NAME(Annots));
        pdf_obj* a = pdf_array_get(ctx, annots, 0);
        pdf_obj* rect = pdf_dict_get(ctx, a, PDF_NAME(Rect));
        if (pdf_array_len(ctx, rect) == 4) {
            for (int i = 0; i < 4; ++i) {
                r[i] = pdf_to_real(ctx, pdf_array_get(ctx, rect, i));
            }
            good = true;
        }
    }
    fz_always(ctx) {
        fz_drop_page(ctx, reinterpret_cast<fz_page*>(page));
        pdf_drop_document(ctx, doc);
    }
    fz_catch(ctx) {
        good = false;
    }
    fz_drop_context(ctx);
    *ok = good;
    return r;
}

class AnnotationRotationTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<pdfe::core::MuPdfContext>();
        fixture_ = std::filesystem::path(TEST_FIXTURES_DIR)
                   / "f15_annot_rotation.pdf";
        ASSERT_TRUE(std::filesystem::exists(fixture_))
            << "regenerate with python tests/fixtures/gen_f15_annot_rotation.py";
        manager_ = std::make_unique<AnnotationManager>(ctx_);
        const ::testing::TestInfo* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        tempDir_ = std::filesystem::temp_directory_path()
                   / "pdfe_f15_rotation" / info->name();
        std::filesystem::create_directories(tempDir_);
    }
    void TearDown() override {
        manager_.reset();
        ctx_.reset();
    }

    std::shared_ptr<pdfe::core::MuPdfContext> ctx_;
    std::filesystem::path fixture_;
    std::filesystem::path tempDir_;
    std::unique_ptr<AnnotationManager> manager_;
};

TEST_F(AnnotationRotationTest, FixtureCarriesTheAuthoredRotations) {
    auto doc = pdfe::core::Document::open(ctx_, fixture_);
    ASSERT_NE(doc, nullptr);
    ASSERT_EQ(doc->pageCount(), 4);
    for (int i = 0; i < 4; ++i) {
        auto page = doc->page(i);
        ASSERT_NE(page, nullptr);
        EXPECT_EQ(page->rotation(), kRotations[i])
            << "fixture page " << i << " lost its authored /Rotate";
        auto annots = manager_->getAnnotations(page.get());
        ASSERT_EQ(annots.size(), 1u) << "page " << i;
        EXPECT_EQ(annots[0].contents, "F15-rot-marker");
    }
}

// Half 1a: the raw user-space /Rect is identical on every rotation, in the
// fixture AND after a PDFE save.
TEST_F(AnnotationRotationTest, AuthoredUserSpaceRectIsIdenticalOnEveryRotation) {
    auto doc = pdfe::core::Document::open(ctx_, fixture_);
    ASSERT_NE(doc, nullptr);
    // A real PDFE write (unrelated edit on page 0), so this is not a byte copy.
    {
        auto page = doc->page(0);
        ASSERT_NE(page, nullptr);
        AnnotationData n = manager_->createAnnotation(
            page.get(), AnnotationType::Text, QRectF(300, 300, 20, 20));
        manager_->setContents(page.get(), n.annotIndex, "f15 unrelated edit");
    }
    auto r = pdfe::test::SaveAndReopenWithPath(*doc, ctx_, tempDir_, "rot_rect");
    ASSERT_NE(r.doc, nullptr);

    for (int i = 0; i < 4; ++i) {
        SCOPED_TRACE("/Rotate " + std::to_string(kRotations[i]));
        bool ok = false;
        const auto fix = ReadRawRect(fixture_, i, &ok);
        ASSERT_TRUE(ok);
        EXPECT_NEAR(fix[0], kUserX0, 0.01);
        EXPECT_NEAR(fix[1], kUserY0, 0.01);
        EXPECT_NEAR(fix[2], kUserX1, 0.01);
        EXPECT_NEAR(fix[3], kUserY1, 0.01);

        const auto saved = ReadRawRect(r.path, i, &ok);
        ASSERT_TRUE(ok);
        for (int k = 0; k < 4; ++k) {
            EXPECT_NEAR(saved[k], fix[k], 0.01)
                << "PDFE rewrote the annotation /Rect on the /Rotate "
                << kRotations[i] << " page (component " << k << ")";
        }
    }
}

// Half 1b: the page-space rect PDFE reports is the authored rect put through
// the page rotation.
TEST_F(AnnotationRotationTest, ReportedRectFollowsPageRotationTransform) {
    auto doc = pdfe::core::Document::open(ctx_, fixture_);
    ASSERT_NE(doc, nullptr);
    for (int i = 0; i < 4; ++i) {
        const int rot = kRotations[i];
        SCOPED_TRACE("/Rotate " + std::to_string(rot));
        auto page = doc->page(i);
        ASSERT_NE(page, nullptr);
        auto annots = manager_->getAnnotations(page.get());
        ASSERT_EQ(annots.size(), 1u);

        const QRectF want = ExpectedPageSpaceRect(rot);
        EXPECT_NEAR(annots[0].rect.left(), want.left(), 0.5);
        EXPECT_NEAR(annots[0].rect.top(), want.top(), 0.5);
        EXPECT_NEAR(annots[0].rect.width(), want.width(), 0.5);
        EXPECT_NEAR(annots[0].rect.height(), want.height(), 0.5);
    }
}

// Half 2: the rendered appearance is oriented per /Rotate, for 90, 180, 270.
TEST_F(AnnotationRotationTest, AppearanceOrientationFollowsPageRotate) {
    auto doc = pdfe::core::Document::open(ctx_, fixture_);
    ASSERT_NE(doc, nullptr);

    auto basePage = doc->page(0);
    ASSERT_NE(basePage, nullptr);
    const QImage baseImg = pdfe::test::RenderPage(*basePage);
    ASSERT_EQ(baseImg.width(), static_cast<int>(kPageW));
    ASSERT_EQ(baseImg.height(), static_cast<int>(kPageH));
    const InkCentroid base = CentroidOfRed(baseImg);
    ASSERT_TRUE(base.found)
        << "the authored red annotation painted no ink on the unrotated page";
    // Sanity-anchor the baseline to the AUTHORED geometry, so a renderer that
    // mislocates the annotation on page 0 too cannot hide behind a relative
    // comparison: user y 500..540 with y up == device y 60..100 top-down.
    EXPECT_NEAR(base.x, 0.5 * (kUserX0 + kUserX1), 3.0);
    EXPECT_NEAR(base.y, kPageH - 0.5 * (kUserY0 + kUserY1), 3.0);

    for (int i = 1; i < 4; ++i) {
        const int rot = kRotations[i];
        SCOPED_TRACE("/Rotate " + std::to_string(rot));
        auto page = doc->page(i);
        ASSERT_NE(page, nullptr);
        const QImage img = pdfe::test::RenderPage(*page);

        const int wantW = (rot == 90 || rot == 270) ? static_cast<int>(kPageH)
                                                    : static_cast<int>(kPageW);
        const int wantH = (rot == 90 || rot == 270) ? static_cast<int>(kPageW)
                                                    : static_cast<int>(kPageH);
        ASSERT_EQ(img.width(), wantW);
        ASSERT_EQ(img.height(), wantH);

        const InkCentroid got = CentroidOfRed(img);
        ASSERT_TRUE(got.found)
            << "annotation painted no ink on the /Rotate " << rot << " page";

        const QPointF want = ExpectedCentroid(rot, base, kPageW, kPageH);
        EXPECT_NEAR(got.x, want.x(), 3.0)
            << "annotation ink is at x=" << got.x << " but /Rotate " << rot
            << " puts it at x=" << want.x();
        EXPECT_NEAR(got.y, want.y(), 3.0)
            << "annotation ink is at y=" << got.y << " but /Rotate " << rot
            << " puts it at y=" << want.y();

        EXPECT_NEAR(static_cast<double>(got.count),
                    static_cast<double>(base.count),
                    0.05 * static_cast<double>(base.count))
            << "ink area changed under rotation (" << got.count << " vs "
            << base.count << ") — appearance is clipped or scaled";
    }
}

// Round-trip half: an annotation PDFE creates on a rotated page must come back
// at the same place after save/reopen (a broken inverse page transform shows up
// as a shifted or transposed rect).
TEST_F(AnnotationRotationTest, CreatedAnnotationRoundTripsOnEveryRotatedPage) {
    for (int i = 0; i < 4; ++i) {
        const int rot = kRotations[i];
        SCOPED_TRACE("/Rotate " + std::to_string(rot));
        auto doc = pdfe::core::Document::open(ctx_, fixture_);
        ASSERT_NE(doc, nullptr);
        auto page = doc->page(i);
        ASSERT_NE(page, nullptr);

        const QRectF placed(30.0, 30.0, 60.0, 20.0);
        AnnotationData a = manager_->createAnnotation(
            page.get(), AnnotationType::Square, placed);
        ASSERT_GE(a.annotIndex, 0);
        page.reset();

        auto reopened = pdfe::test::SaveAndReopen(
            *doc, ctx_, tempDir_, "created_rot" + std::to_string(rot));
        ASSERT_NE(reopened, nullptr);
        auto rpage = reopened->page(i);
        ASSERT_NE(rpage, nullptr);
        auto annots = manager_->getAnnotations(rpage.get());
        ASSERT_EQ(annots.size(), 2u);

        // The created one is the second entry (the fixture's is first).
        const QRectF got = annots[1].rect;
        // MuPDF grows the rect by the 1pt border when it bakes the appearance,
        // hence the 1.5pt tolerance — the point of the assert is that the
        // annotation did not MOVE or transpose under rotation.
        EXPECT_NEAR(got.left(), placed.left(), 1.5);
        EXPECT_NEAR(got.top(), placed.top(), 1.5);
        EXPECT_NEAR(got.width(), placed.width(), 3.0);
        EXPECT_NEAR(got.height(), placed.height(), 3.0);
    }
}

} // namespace
