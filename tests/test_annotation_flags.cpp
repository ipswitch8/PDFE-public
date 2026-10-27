// F15 / gap PDF-51 — annotation flags (/F).
//
// WHAT WAS BROKEN
// ---------------
// /F travelled through PDFE as a bare `int` (AnnotationData::flags) with ZERO
// named constants anywhere in src/ and ZERO tests. Consequences the suite
// could not see:
//   * a /Hidden annotation carrying sensitive /Contents was invisible on
//     screen yet survived every round-trip untouched,
//   * a /Locked or /ReadOnly annotation could be moved, resized and its text
//     rewritten by PDFE — a spec violation (ISO 32000-1 table 165).
//
// F15 adds src/annotations/AnnotationFlags.h (named constants + a typed mask),
// the AnnotationManager get/set flag API, and enforcement of Locked /
// ReadOnly / LockedContents on the geometry and contents edit paths. This file
// is the oracle.
//
// MUTATIONS KILLED (each confirmed RED then reverted):
//   * change AnnotationFlag::Locked to the wrong bit (1 << 6)
//     -> NamedConstantsMatchMuPdfEnumerators fails.
//   * remove the isGeometryLocked() guard from AnnotationManager::setRect
//     -> LockedAnnotRejectsGeometryEdit fails.
//   * remove the isContentsLocked() guard from AnnotationManager::setContents
//     -> LockedContentsAnnotRejectsTextEdit fails.
//   * make setAnnotationFlags a no-op
//     -> EachFlagRoundTripsIndividually fails on the first flag.

#include <gtest/gtest.h>

#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationData.h"
#include "annotations/AnnotationFlags.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include "helpers/RoundTripHelper.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <QImage>

#include <filesystem>
#include <memory>
#include <string>

using pdfe::annotations::AnnotationData;
using pdfe::annotations::AnnotationFlag;
using pdfe::annotations::AnnotationFlags;
using pdfe::annotations::AnnotationManager;
using pdfe::annotations::AnnotationType;
using pdfe::annotations::annotationFlagName;
using pdfe::annotations::kAllAnnotationFlags;

namespace {

// Raw /F integer of annotation 0 on page 0 of `path`, read with a fresh,
// independent MuPDF context. -1 when unreadable.
int ReadRawAnnotFlags(const std::filesystem::path& path) {
    int out = -1;
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx) return out;
    pdf_document* doc = nullptr;
    pdf_page* page = nullptr;
    fz_try(ctx) {
        fz_register_document_handlers(ctx);
        doc = pdf_open_document(ctx, path.string().c_str());
        page = pdf_load_page(ctx, doc, 0);
        pdf_annot* a = pdf_first_annot(ctx, page);
        if (a) out = pdf_annot_flags(ctx, a);
    }
    fz_always(ctx) {
        fz_drop_page(ctx, reinterpret_cast<fz_page*>(page));
        pdf_drop_document(ctx, doc);
    }
    fz_catch(ctx) {
        out = -1;
    }
    fz_drop_context(ctx);
    return out;
}

// Count RED pixels — the colour the test paints its annotation with. A
// whole-page "non-white" count is useless here: the render includes an alpha
// channel, so untouched background pixels are not white either.
long CountRed(const QImage& img) {
    long n = 0;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            const QRgb p = img.pixel(x, y);
            if (qRed(p) > 150 && qGreen(p) < 120 && qBlue(p) < 120) ++n;
        }
    }
    return n;
}

class AnnotationFlagsTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<pdfe::core::MuPdfContext>();
        // A blank WHITE page (from the F15 rotation fixture, page 0 is
        // /Rotate 0) so a coloured annotation is unambiguous ink.
        fixture_ = std::filesystem::path(TEST_FIXTURES_DIR) / "test_3page.pdf";
        whitePage_ = std::filesystem::path(TEST_FIXTURES_DIR)
                     / "f15_annot_rotation.pdf";
        ASSERT_TRUE(std::filesystem::exists(whitePage_));
        ASSERT_TRUE(std::filesystem::exists(fixture_));
        manager_ = std::make_unique<AnnotationManager>(ctx_);
        const ::testing::TestInfo* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        tempDir_ = std::filesystem::temp_directory_path()
                   / "pdfe_f15_flags" / info->name();
        std::filesystem::create_directories(tempDir_);
    }
    void TearDown() override {
        manager_.reset();
        ctx_.reset();
    }

    std::shared_ptr<pdfe::core::MuPdfContext> ctx_;
    std::filesystem::path fixture_;
    std::filesystem::path whitePage_;
    std::filesystem::path tempDir_;
    std::unique_ptr<AnnotationManager> manager_;
};

// ─── the vocabulary itself ───────────────────────────────────────────────

TEST_F(AnnotationFlagsTest, NamedConstantsMatchMuPdfEnumerators) {
    EXPECT_EQ(static_cast<int>(AnnotationFlag::Invisible), PDF_ANNOT_IS_INVISIBLE);
    EXPECT_EQ(static_cast<int>(AnnotationFlag::Hidden), PDF_ANNOT_IS_HIDDEN);
    EXPECT_EQ(static_cast<int>(AnnotationFlag::Print), PDF_ANNOT_IS_PRINT);
    EXPECT_EQ(static_cast<int>(AnnotationFlag::NoZoom), PDF_ANNOT_IS_NO_ZOOM);
    EXPECT_EQ(static_cast<int>(AnnotationFlag::NoRotate), PDF_ANNOT_IS_NO_ROTATE);
    EXPECT_EQ(static_cast<int>(AnnotationFlag::NoView), PDF_ANNOT_IS_NO_VIEW);
    EXPECT_EQ(static_cast<int>(AnnotationFlag::ReadOnly), PDF_ANNOT_IS_READ_ONLY);
    EXPECT_EQ(static_cast<int>(AnnotationFlag::Locked), PDF_ANNOT_IS_LOCKED);
    EXPECT_EQ(static_cast<int>(AnnotationFlag::ToggleNoView),
              PDF_ANNOT_IS_TOGGLE_NO_VIEW);
    EXPECT_EQ(static_cast<int>(AnnotationFlag::LockedContents),
              PDF_ANNOT_IS_LOCKED_CONTENTS);
    EXPECT_EQ(kAllAnnotationFlags.size(), 10u);
}

// ─── PDF-51 core: EVERY flag round-trips INDIVIDUALLY ────────────────────

TEST_F(AnnotationFlagsTest, EachFlagRoundTripsIndividually) {
    for (AnnotationFlag flag : kAllAnnotationFlags) {
        SCOPED_TRACE(annotationFlagName(flag));

        auto doc = pdfe::core::Document::open(ctx_, fixture_);
        ASSERT_NE(doc, nullptr);
        auto page = doc->page(0);
        ASSERT_NE(page, nullptr);

        AnnotationData created = manager_->createAnnotation(
            page.get(), AnnotationType::Square, QRectF(100, 100, 80, 40));
        ASSERT_GE(created.annotIndex, 0);

        AnnotationFlags want;
        want.set(flag, true);
        manager_->setAnnotationFlags(page.get(), created.annotIndex, want);
        EXPECT_EQ(manager_->getAnnotationFlags(page.get(), created.annotIndex),
                  want);
        page.reset();

        auto r = pdfe::test::SaveAndReopenWithPath(*doc, ctx_, tempDir_,
                                                   annotationFlagName(flag));
        ASSERT_NE(r.doc, nullptr);

        // Raw bytes carry exactly this one bit...
        EXPECT_EQ(ReadRawAnnotFlags(r.path), static_cast<int>(flag))
            << "saved /F is not exactly " << annotationFlagName(flag);

        // ...and PDFE reads it back as exactly this one named flag.
        auto rpage = r.doc->page(0);
        ASSERT_NE(rpage, nullptr);
        const AnnotationFlags got = manager_->getAnnotationFlags(rpage.get(), 0);
        EXPECT_TRUE(got.has(flag));
        for (AnnotationFlag other : kAllAnnotationFlags) {
            if (other == flag) continue;
            EXPECT_FALSE(got.has(other))
                << "unrelated flag " << annotationFlagName(other)
                << " appeared; got=" << describeAnnotationFlags(got);
        }
        // And through the data struct the rest of the product uses.
        auto annots = manager_->getAnnotations(rpage.get());
        ASSERT_FALSE(annots.empty());
        EXPECT_TRUE(annots[0].flags.has(flag));
    }
}

TEST_F(AnnotationFlagsTest, CombinedFlagsRoundTripAndClearIndividually) {
    auto doc = pdfe::core::Document::open(ctx_, fixture_);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    AnnotationData a = manager_->createAnnotation(
        page.get(), AnnotationType::Square, QRectF(60, 60, 50, 50));

    AnnotationFlags want;
    want.set(AnnotationFlag::Print).set(AnnotationFlag::NoZoom)
        .set(AnnotationFlag::LockedContents);
    manager_->setAnnotationFlags(page.get(), a.annotIndex, want);
    page.reset();

    auto r = pdfe::test::SaveAndReopenWithPath(*doc, ctx_, tempDir_, "combo");
    ASSERT_NE(r.doc, nullptr);
    EXPECT_EQ(ReadRawAnnotFlags(r.path),
              PDF_ANNOT_IS_PRINT | PDF_ANNOT_IS_NO_ZOOM |
              PDF_ANNOT_IS_LOCKED_CONTENTS);

    auto rpage = r.doc->page(0);
    AnnotationFlags got = manager_->getAnnotationFlags(rpage.get(), 0);
    EXPECT_EQ(describeAnnotationFlags(got), "Print|NoZoom|LockedContents");

    // Clearing ONE flag must leave the others alone.
    manager_->setAnnotationFlag(rpage.get(), 0, AnnotationFlag::NoZoom, false);
    got = manager_->getAnnotationFlags(rpage.get(), 0);
    EXPECT_TRUE(got.has(AnnotationFlag::Print));
    EXPECT_FALSE(got.has(AnnotationFlag::NoZoom));
    EXPECT_TRUE(got.has(AnnotationFlag::LockedContents));
}

// ─── PDF-51 security half: /Hidden really is not rendered ────────────────

TEST_F(AnnotationFlagsTest, HiddenAnnotIsNotRendered) {
    // f15_annot_rotation.pdf page 0 already carries ONE red /Square annotation
    // authored in the fixture (see gen_f15_annot_rotation.py) on a white page.
    auto doc = pdfe::core::Document::open(ctx_, whitePage_);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    const long inkVisible = CountRed(pdfe::test::RenderPage(*page));
    ASSERT_GT(inkVisible, 100)
        << "the fixture annotation drew no red ink, so this test cannot "
           "detect whether hiding it works";

    manager_->setAnnotationFlag(page.get(), 0, AnnotationFlag::Hidden, true);
    const long inkHidden = CountRed(pdfe::test::RenderPage(*page));
    EXPECT_EQ(inkHidden, 0)
        << "a /Hidden annotation still painted " << inkHidden << " red pixels";

    // ...and the annotation is still IN the file — hidden is not deleted.
    page.reset();
    auto r = pdfe::test::SaveAndReopenWithPath(*doc, ctx_, tempDir_, "hidden");
    ASSERT_NE(r.doc, nullptr);
    EXPECT_EQ(ReadRawAnnotFlags(r.path) & PDF_ANNOT_IS_HIDDEN,
              PDF_ANNOT_IS_HIDDEN);
    auto rpage = r.doc->page(0);
    EXPECT_EQ(CountRed(pdfe::test::RenderPage(*rpage)), 0);
    auto annots = manager_->getAnnotations(rpage.get());
    ASSERT_EQ(annots.size(), 1u)
        << "the hidden annotation was dropped from the saved file";
    EXPECT_EQ(annots[0].contents, "F15-rot-marker");
}

// A /NoView annotation is invisible on screen but still PRINTS: the two flags
// are not interchangeable, and only /Hidden suppresses both.
TEST_F(AnnotationFlagsTest, NoViewAndHiddenAreDistinctFlags) {
    auto doc = pdfe::core::Document::open(ctx_, whitePage_);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    manager_->setAnnotationFlag(page.get(), 0, AnnotationFlag::NoView, true);
    const AnnotationFlags f = manager_->getAnnotationFlags(page.get(), 0);
    EXPECT_TRUE(f.isNoView());
    EXPECT_FALSE(f.isHidden())
        << "setting NoView must not set Hidden — they are different bits";
    EXPECT_TRUE(f.isPrintable())
        << "the fixture authors /F 4 (Print); NoView must leave Print alone";
}

// ─── PDF-51 spec half: Locked / ReadOnly / LockedContents are enforced ───

TEST_F(AnnotationFlagsTest, LockedAnnotRejectsGeometryEdit) {
    auto doc = pdfe::core::Document::open(ctx_, fixture_);
    auto page = doc->page(0);
    AnnotationData a = manager_->createAnnotation(
        page.get(), AnnotationType::Square, QRectF(100, 100, 80, 40));
    const QRectF locked(100, 100, 80, 40);

    manager_->setAnnotationFlag(page.get(), a.annotIndex,
                                AnnotationFlag::Locked, true);
    EXPECT_THROW(manager_->setRect(page.get(), a.annotIndex,
                                   QRectF(300, 300, 80, 40)),
                 std::exception);

    auto annots = manager_->getAnnotations(page.get());
    ASSERT_FALSE(annots.empty());
    // Tolerance 1.5pt: MuPDF grows the rect by the 1pt border when it bakes the
    // appearance. The point of the assert is that the annotation did not move
    // to (300, 300).
    EXPECT_NEAR(annots[0].rect.left(), locked.left(), 1.5);
    EXPECT_NEAR(annots[0].rect.top(), locked.top(), 1.5);

    // Unlocking restores the ability to move it (so the guard is the flag,
    // not a blanket refusal).
    manager_->setAnnotationFlag(page.get(), a.annotIndex,
                                AnnotationFlag::Locked, false);
    EXPECT_NO_THROW(manager_->setRect(page.get(), a.annotIndex,
                                      QRectF(300, 300, 80, 40)));
}

TEST_F(AnnotationFlagsTest, ReadOnlyAnnotRejectsGeometryAndTextEdits) {
    auto doc = pdfe::core::Document::open(ctx_, fixture_);
    auto page = doc->page(0);
    AnnotationData a = manager_->createAnnotation(
        page.get(), AnnotationType::Text, QRectF(50, 50, 30, 30));
    manager_->setContents(page.get(), a.annotIndex, "original");
    manager_->setAnnotationFlag(page.get(), a.annotIndex,
                                AnnotationFlag::ReadOnly, true);

    EXPECT_THROW(manager_->setRect(page.get(), a.annotIndex,
                                   QRectF(200, 200, 30, 30)),
                 std::exception);
    EXPECT_THROW(manager_->setContents(page.get(), a.annotIndex, "tampered"),
                 std::exception);

    auto annots = manager_->getAnnotations(page.get());
    ASSERT_FALSE(annots.empty());
    EXPECT_EQ(annots[0].contents, "original");
}

TEST_F(AnnotationFlagsTest, LockedContentsAnnotRejectsTextEdit) {
    auto doc = pdfe::core::Document::open(ctx_, fixture_);
    auto page = doc->page(0);
    AnnotationData a = manager_->createAnnotation(
        page.get(), AnnotationType::Text, QRectF(50, 50, 30, 30));
    manager_->setContents(page.get(), a.annotIndex, "third-party comment");
    manager_->setAnnotationFlag(page.get(), a.annotIndex,
                                AnnotationFlag::LockedContents, true);

    EXPECT_THROW(manager_->setContents(page.get(), a.annotIndex, "tampered"),
                 std::exception);
    auto annots = manager_->getAnnotations(page.get());
    ASSERT_FALSE(annots.empty());
    EXPECT_EQ(annots[0].contents, "third-party comment");

    // /LockedContents must NOT freeze geometry — only the text.
    EXPECT_NO_THROW(manager_->setRect(page.get(), a.annotIndex,
                                      QRectF(120, 120, 30, 30)));
}

} // namespace
