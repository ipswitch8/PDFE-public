// Rotating a text box must rotate the TEXT, not just the box.
//
// Field-reported 2026-08-28 against a scanned manual: the rotate handle turned
// the selection outline while the text inside stayed upright. Diagnosis in
// docs/audit/field-report-2026-08-28-scanned-manual.md (defect 2). The angle
// lived only as UI state in SelectTool; on commit the handler stored a widened
// axis-aligned rect and threw the angle away, so nothing downstream could have
// drawn rotated text even in principle.
//
// NOT to be confused with tests/test_annotation_rotation.cpp, which is about
// PAGE /Rotate. This file is about rotation of an annotation's own content.
//
// HOW THE FIX WORKS, because the oracle only makes sense alongside it:
// MuPDF synthesises a FreeText appearance with /Matrix identity and /BBox
// equal to the rect, and pdf_annot_transform implements ISO 32000-1 12.5.5 --
// it transforms the BBox by the Matrix and maps the result onto /Rect. So
// setting the appearance stream's /Matrix to a rotation and /Rect to the
// rotated bounds renders the whole appearance (text, border, background)
// rotated in place, at any angle, without reimplementing text layout.
//
// Therefore the load-bearing assertion is on the APPEARANCE STREAM'S MATRIX,
// not on the rect. A test that only checked the rect had grown would have
// passed against the reported bug, which widened the rect and rotated nothing.

#include <gtest/gtest.h>

#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationCommands.h"
#include "annotations/AnnotationData.h"
#include "annotations/AnnotationType.h"
#include "core/UndoManager.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <cmath>
#include <filesystem>
#include <memory>
#include <string>

using namespace pdfe::annotations;
using namespace pdfe::core;

namespace {

/// What a saved file says about one annotation's rotation.
struct SavedRotation {
    bool found = false;
    bool hasAp = false;
    bool matrixIsIdentity = true;
    double a = 1, b = 0, c = 0, d = 1;   ///< AP /Matrix linear part
    double storedDegrees = 0.0;          ///< PDFERotationDegrees
    bool hasStoredDegrees = false;
    double rectW = 0, rectH = 0;
};

/// Reopen `path` with a bare MuPDF context and read annotation 0's rotation
/// state. Deliberately does NOT go through AnnotationManager: the question is
/// what landed in the FILE, and asking the same code that wrote it would only
/// confirm it is self-consistent.
SavedRotation ReadSavedRotation(const std::filesystem::path& path) {
    SavedRotation out;
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx) return out;

    pdf_document* doc = nullptr;
    fz_page* fzPage = nullptr;

    fz_try(ctx) {
        fz_register_document_handlers(ctx);
        doc = pdf_open_document(ctx, path.string().c_str());
        fzPage = fz_load_page(ctx, (fz_document*)doc, 0);
        pdf_page* page = pdf_page_from_fz_page(ctx, fzPage);

        pdf_annot* a = pdf_first_annot(ctx, page);
        if (a) {
            out.found = true;
            pdf_obj* obj = pdf_annot_obj(ctx, a);

            pdf_obj* deg = pdf_dict_gets(ctx, obj, "PDFERotationDegrees");
            if (deg) {
                out.hasStoredDegrees = true;
                out.storedDegrees = pdf_to_real(ctx, deg);
            }

            fz_rect r = pdf_dict_get_rect(ctx, obj, PDF_NAME(Rect));
            out.rectW = static_cast<double>(r.x1 - r.x0);
            out.rectH = static_cast<double>(r.y1 - r.y0);

            pdf_obj* ap = pdf_dict_get(ctx, obj, PDF_NAME(AP));
            pdf_obj* n = ap ? pdf_dict_get(ctx, ap, PDF_NAME(N)) : nullptr;
            if (n) {
                out.hasAp = true;
                fz_matrix m = pdf_dict_get_matrix(ctx, n, PDF_NAME(Matrix));
                out.a = m.a; out.b = m.b; out.c = m.c; out.d = m.d;
                out.matrixIsIdentity =
                    std::fabs(m.a - 1.0f) < 1e-4f && std::fabs(m.b) < 1e-4f &&
                    std::fabs(m.c) < 1e-4f && std::fabs(m.d - 1.0f) < 1e-4f;
            }
        }
    }
    fz_always(ctx) {
        if (fzPage) fz_drop_page(ctx, fzPage);
        if (doc) pdf_drop_document(ctx, doc);
    }
    fz_catch(ctx) {
        // leave `out` as far as it got; callers assert on `found`
    }
    fz_drop_context(ctx);
    return out;
}

/// Angle, in degrees, recovered from a rotation matrix's linear part.
double AngleOf(const SavedRotation& s) {
    return std::atan2(s.b, s.a) * 180.0 / 3.14159265358979323846;
}

} // namespace

class AnnotationContentRotationTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<MuPdfContext>();
        const std::filesystem::path fixtures = TEST_FIXTURES_DIR;
        srcPdf_ = fixtures / "test_single.pdf";
        // No skip when the fixture is absent: a silent skip reports "did not
        // run" as success, and this fixture is committed.
        ASSERT_TRUE(std::filesystem::exists(srcPdf_))
            << "missing committed fixture: " << srcPdf_.string();

        document_ = Document::open(context_, srcPdf_);
        ASSERT_NE(document_, nullptr);
        manager_ = std::make_unique<AnnotationManager>(context_);

        outDir_ = std::filesystem::temp_directory_path() / "pdfe_rot_tests";
        std::filesystem::create_directories(outDir_);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(outDir_, ec);
    }

    /// Create a FreeText box with some text on page 0 and return its index.
    int MakeTextBox(Page* page, const QRectF& rect) {
        AnnotationData data =
            manager_->createAnnotation(page, AnnotationType::FreeText, rect);
        const int index = data.annotIndex >= 0 ? data.annotIndex : 0;
        manager_->setContents(page, index, "ROTATE ME");
        return index;
    }

    std::shared_ptr<MuPdfContext> context_;
    std::filesystem::path srcPdf_;
    std::filesystem::path outDir_;
    std::unique_ptr<Document> document_;
    std::unique_ptr<AnnotationManager> manager_;
};

// ---------------------------------------------------------------------------
// The angle exists at all, and survives a round trip through the model.
// ---------------------------------------------------------------------------

TEST_F(AnnotationContentRotationTest, RotationIsReadBackFromTheAnnotation) {
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);
    const QRectF rect(100, 100, 200, 60);
    const int idx = MakeTextBox(page.get(), rect);

    manager_->setRotation(page.get(), idx, 37.5, rect);

    AnnotationData read = manager_->getAnnotation(page.get(), idx);
    EXPECT_NEAR(read.rotationDegrees, 37.5, 1e-3)
        << "the angle was not stored, which is the reported defect";
}

TEST_F(AnnotationContentRotationTest, AnUnrotatedAnnotationReportsZero) {
    // The default must stay 0 so every existing annotation is unaffected.
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);
    const int idx = MakeTextBox(page.get(), QRectF(100, 100, 200, 60));

    AnnotationData read = manager_->getAnnotation(page.get(), idx);
    EXPECT_DOUBLE_EQ(read.rotationDegrees, 0.0);
}

// ---------------------------------------------------------------------------
// The load-bearing one: the APPEARANCE is rotated, not merely the rect.
// ---------------------------------------------------------------------------

TEST_F(AnnotationContentRotationTest, TheSavedAppearanceStreamCarriesARotationMatrix) {
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);
    const QRectF rect(100, 100, 200, 60);
    const int idx = MakeTextBox(page.get(), rect);
    manager_->setRotation(page.get(), idx, 30.0, rect);

    const auto out = outDir_ / "rotated30.pdf";
    ASSERT_NO_THROW(document_->saveAs(out));
    ASSERT_TRUE(std::filesystem::exists(out));

    const SavedRotation s = ReadSavedRotation(out);
    ASSERT_TRUE(s.found) << "no annotation survived the save";
    ASSERT_TRUE(s.hasAp) << "no appearance stream to rotate";

    EXPECT_FALSE(s.matrixIsIdentity)
        << "the appearance matrix is identity -- the text is upright, which is "
           "exactly the reported bug: the box turned and the text did not";
    EXPECT_NEAR(AngleOf(s), 30.0, 0.5)
        << "matrix = [" << s.a << " " << s.b << " " << s.c << " " << s.d << "]";
}

TEST_F(AnnotationContentRotationTest, ZeroRotationLeavesTheAppearanceUpright) {
    // The complement of the test above. Without this, a mutation that always
    // wrote a rotation matrix would pass everything else.
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);
    const QRectF rect(100, 100, 200, 60);
    const int idx = MakeTextBox(page.get(), rect);
    manager_->setRotation(page.get(), idx, 0.0, rect);

    const auto out = outDir_ / "rotated0.pdf";
    ASSERT_NO_THROW(document_->saveAs(out));
    ASSERT_TRUE(std::filesystem::exists(out));

    const SavedRotation s = ReadSavedRotation(out);
    ASSERT_TRUE(s.found);
    EXPECT_TRUE(s.matrixIsIdentity)
        << "an unrotated box must not acquire a rotation matrix; matrix = ["
        << s.a << " " << s.b << " " << s.c << " " << s.d << "]";
}

TEST_F(AnnotationContentRotationTest, TheRectWidensToTheRotatedBounds) {
    // /Rect is axis-aligned by definition, so it must grow to contain the
    // rotated appearance. This is the half the old code already got right.
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);
    const QRectF rect(100, 100, 200, 60);   // 200 x 60
    const int idx = MakeTextBox(page.get(), rect);
    manager_->setRotation(page.get(), idx, 90.0, rect);

    const auto out = outDir_ / "rotated90.pdf";
    ASSERT_NO_THROW(document_->saveAs(out));
    ASSERT_TRUE(std::filesystem::exists(out));

    const SavedRotation s = ReadSavedRotation(out);
    ASSERT_TRUE(s.found);
    // At 90 degrees the bounds are the box with width and height swapped.
    EXPECT_NEAR(s.rectW, 60.0, 1.0);
    EXPECT_NEAR(s.rectH, 200.0, 1.0);
}

// ---------------------------------------------------------------------------
// The hazard that would otherwise bite silently, months later.
// ---------------------------------------------------------------------------

TEST_F(AnnotationContentRotationTest, EditingContentsDoesNotSilentlyLoseTheRotation) {
    // MuPDF's pdf-appearance.c carves out Stamps carrying a custom appearance
    // from resynthesis, and has NO such carve-out for FreeText. So ANY later
    // property edit sets needs_new_ap, MuPDF regenerates the appearance with
    // fz_identity, and the rotation vanishes -- with no error, no warning, and
    // nothing in the UI to suggest it happened.
    //
    // This is the test that catches the whole class. If it is ever deleted
    // because "the rotation tests already pass", read this comment first.
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);
    const QRectF rect(100, 100, 200, 60);
    const int idx = MakeTextBox(page.get(), rect);
    manager_->setRotation(page.get(), idx, 45.0, rect);

    manager_->setContents(page.get(), idx, "EDITED AFTER ROTATING");

    const auto out = outDir_ / "rotated_then_edited.pdf";
    ASSERT_NO_THROW(document_->saveAs(out));
    ASSERT_TRUE(std::filesystem::exists(out));

    const SavedRotation s = ReadSavedRotation(out);
    ASSERT_TRUE(s.found);
    ASSERT_TRUE(s.hasAp);
    EXPECT_FALSE(s.matrixIsIdentity)
        << "editing the text reset the appearance to upright";
    EXPECT_NEAR(AngleOf(s), 45.0, 0.5);
    EXPECT_NEAR(s.storedDegrees, 45.0, 1e-3);
}

TEST_F(AnnotationContentRotationTest, ChangingColourDoesNotSilentlyLoseTheRotation) {
    // Same class, a different setter -- because fixing one instance of a
    // pattern is not fixing the pattern.
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);
    const QRectF rect(120, 140, 180, 50);
    const int idx = MakeTextBox(page.get(), rect);
    manager_->setRotation(page.get(), idx, 20.0, rect);

    manager_->setColor(page.get(), idx, QColor(255, 0, 0));

    const auto out = outDir_ / "rotated_then_recoloured.pdf";
    ASSERT_NO_THROW(document_->saveAs(out));
    ASSERT_TRUE(std::filesystem::exists(out));

    const SavedRotation s = ReadSavedRotation(out);
    ASSERT_TRUE(s.found);
    EXPECT_FALSE(s.matrixIsIdentity) << "recolouring reset the appearance";
    EXPECT_NEAR(AngleOf(s), 20.0, 0.5);
}

TEST_F(AnnotationContentRotationTest,
       TouchingAnotherAnnotationDoesNotLoseTheRotation) {
    // The window this closes, and why it needs a SECOND annotation to open
    // deterministically.
    //
    // Setting /Rect is the last thing the rotation code does, and it dirties
    // the annotation -- so it ends up requesting resynthesis with the rotation
    // matrix already installed. MuPDF's resynthesis is document-wide:
    // pdf_annot_request_resynthesis sets doc->resynth_required, and the next
    // pdf_update_annot from ANY annotation runs update_all_appearances over
    // the whole page, regenerating every annotation still marked needs_new_ap
    // -- including the rotated one, with fz_identity.
    //
    // So whether the rotation survived depended on whether anything else
    // happened to touch an annotation afterwards. That is why an earlier
    // version of this file passed once and then failed: ordering luck, not a
    // fix. Editing a SECOND annotation makes the window open every time.
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);

    const QRectF rect(100, 100, 200, 60);
    const int rotated = MakeTextBox(page.get(), rect);
    const int other = MakeTextBox(page.get(), QRectF(320, 300, 150, 40));
    ASSERT_NE(rotated, other) << "needed two distinct annotations";

    manager_->setRotation(page.get(), rotated, 45.0, rect);

    // Touch the OTHER annotation. Nothing about this should reach the first.
    manager_->setContents(page.get(), other, "an unrelated edit");

    const auto out = outDir_ / "rotated_then_other_touched.pdf";
    ASSERT_NO_THROW(document_->saveAs(out));
    ASSERT_TRUE(std::filesystem::exists(out));

    const SavedRotation s = ReadSavedRotation(out);
    ASSERT_TRUE(s.found);
    ASSERT_TRUE(s.hasAp);
    EXPECT_FALSE(s.matrixIsIdentity)
        << "editing a DIFFERENT annotation reset this one's appearance to "
           "upright -- the rotated annotation was left requesting resynthesis";
    EXPECT_NEAR(AngleOf(s), 45.0, 0.5);
}

TEST_F(AnnotationContentRotationTest, ChangingFlagsDoesNotSilentlyLoseTheRotation) {
    // The SIXTH setter, and the one that was missed.
    //
    // pdf_set_annot_flags -> pdf_dirty_annot -> pdf_annot_request_resynthesis,
    // so setting /F reaches the appearance exactly like setContents and
    // setColor do. The first pass of this fix covered five setters and the
    // commit message claimed it covered "every setter that can dirty a
    // FreeText". A gate agent read the MuPDF call chain instead of the claim
    // and found the sixth.
    //
    // Worth stating plainly because it explains how it was missed: no UI path
    // calls setAnnotationFlags today. It is a public AnnotationManager method
    // with tested /Locked and /ReadOnly semantics, clearly heading for UI
    // wiring, so it would have sat quietly broken until the day something
    // called it -- at which point the rotation would vanish on a flag toggle
    // and the cause would be nowhere near the symptom.
    //
    // NOTE the ordering: the flag is set AFTER rotating. RotatingALockedAnnot-
    // ationIsRefused sets it BEFORE, so it never crosses this path -- which is
    // why an existing flags test did not catch this.
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);
    const QRectF rect(110, 130, 190, 55);
    const int idx = MakeTextBox(page.get(), rect);
    manager_->setRotation(page.get(), idx, 25.0, rect);

    // A flag with no geometry-lock semantics, so the call is not refused.
    AnnotationFlags flags{};
    flags.set(AnnotationFlag::Print);
    manager_->setAnnotationFlags(page.get(), idx, flags);

    const auto out = outDir_ / "rotated_then_flagged.pdf";
    ASSERT_NO_THROW(document_->saveAs(out));
    ASSERT_TRUE(std::filesystem::exists(out));

    const SavedRotation s = ReadSavedRotation(out);
    ASSERT_TRUE(s.found);
    ASSERT_TRUE(s.hasAp);
    EXPECT_FALSE(s.matrixIsIdentity)
        << "toggling a flag reset the appearance to upright";
    EXPECT_NEAR(AngleOf(s), 25.0, 0.5);
    EXPECT_NEAR(s.storedDegrees, 25.0, 1e-3);
}

// ---------------------------------------------------------------------------
// Persistence and repeated rotation.
// ---------------------------------------------------------------------------

TEST_F(AnnotationContentRotationTest, TheAngleSurvivesSaveAndReopen) {
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);
    const QRectF rect(100, 100, 200, 60);
    const int idx = MakeTextBox(page.get(), rect);
    manager_->setRotation(page.get(), idx, 17.25, rect);

    const auto out = outDir_ / "persist.pdf";
    ASSERT_NO_THROW(document_->saveAs(out));
    ASSERT_TRUE(std::filesystem::exists(out));

    auto ctx2 = std::make_shared<MuPdfContext>();
    auto reopened = Document::open(ctx2, out);
    ASSERT_NE(reopened, nullptr);
    AnnotationManager mgr2(ctx2);
    auto page2 = reopened->page(0);
    ASSERT_NE(page2, nullptr);

    AnnotationData read = mgr2.getAnnotation(page2.get(), 0);
    EXPECT_NEAR(read.rotationDegrees, 17.25, 1e-3);
}

TEST_F(AnnotationContentRotationTest, RotatingTwiceDoesNotCompoundTheBounds) {
    // The rect widens on every rotation. If the second rotation laid the text
    // out in the ALREADY-WIDENED box, the annotation would grow without limit
    // as the user dragged the handle back and forth. The content box is stored
    // separately for exactly this reason.
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);
    const QRectF rect(100, 100, 200, 60);
    const int idx = MakeTextBox(page.get(), rect);

    manager_->setRotation(page.get(), idx, 30.0, rect);
    AnnotationData afterFirst = manager_->getAnnotation(page.get(), idx);

    // Back to upright: the bounds must return to the original box, not to
    // something larger.
    manager_->setRotation(page.get(), idx, 0.0, afterFirst.rect);

    const auto out = outDir_ / "twice.pdf";
    ASSERT_NO_THROW(document_->saveAs(out));
    ASSERT_TRUE(std::filesystem::exists(out));
    const SavedRotation s = ReadSavedRotation(out);
    ASSERT_TRUE(s.found);
    EXPECT_NEAR(s.rectW, 200.0, 1.0)
        << "the box grew across rotations (compounding widening)";
    EXPECT_NEAR(s.rectH, 60.0, 1.0);
}

// ---------------------------------------------------------------------------
// Moving a rotated annotation.
// ---------------------------------------------------------------------------

TEST_F(AnnotationContentRotationTest, MovingARotatedAnnotationKeepsItRotated) {
    // setRect on a rotated annotation cannot just write the caller's rect into
    // /Rect: re-asserting the rotation afterwards recomputes /Rect from the
    // stored content box, which would snap the annotation straight back and
    // silently discard the move. The content box is translated instead.
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);
    const QRectF rect(100, 100, 200, 60);
    const int idx = MakeTextBox(page.get(), rect);
    manager_->setRotation(page.get(), idx, 30.0, rect);

    const AnnotationData rotated = manager_->getAnnotation(page.get(), idx);
    const QPointF before = rotated.rect.center();

    // Drag it 150 right and 90 down, preserving the bounds' size as a move does.
    const QRectF movedRect = rotated.rect.translated(150.0, 90.0);
    manager_->setRect(page.get(), idx, movedRect);

    const auto out = outDir_ / "rotated_then_moved.pdf";
    ASSERT_NO_THROW(document_->saveAs(out));
    ASSERT_TRUE(std::filesystem::exists(out));

    const AnnotationData after = manager_->getAnnotation(page.get(), idx);
    EXPECT_NEAR(after.rect.center().x(), before.x() + 150.0, 1.0)
        << "the move was discarded";
    EXPECT_NEAR(after.rect.center().y(), before.y() + 90.0, 1.0)
        << "the move was discarded";

    const SavedRotation s = ReadSavedRotation(out);
    ASSERT_TRUE(s.found);
    ASSERT_TRUE(s.hasAp);
    EXPECT_FALSE(s.matrixIsIdentity) << "moving it lost the rotation";
    EXPECT_NEAR(AngleOf(s), 30.0, 0.5);
    // The bounds must not grow: a move is not a rotation.
    EXPECT_NEAR(s.rectW, rotated.rect.width(), 1.0);
    EXPECT_NEAR(s.rectH, rotated.rect.height(), 1.0);
}

TEST_F(AnnotationContentRotationTest, MovingAnUnrotatedAnnotationIsUnaffected) {
    // The rotated branch in setRect must not change what a plain move does.
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);
    const QRectF rect(100, 100, 200, 60);
    const int idx = MakeTextBox(page.get(), rect);

    const QRectF target(250, 300, 200, 60);
    manager_->setRect(page.get(), idx, target);

    const AnnotationData after = manager_->getAnnotation(page.get(), idx);
    EXPECT_NEAR(after.rect.left(), target.left(), 1.0);
    EXPECT_NEAR(after.rect.top(), target.top(), 1.0);
    EXPECT_NEAR(after.rect.width(), target.width(), 1.0);
    EXPECT_NEAR(after.rect.height(), target.height(), 1.0);
}

// ---------------------------------------------------------------------------
// Delete then restore (the undo-after-delete path).
// ---------------------------------------------------------------------------

TEST_F(AnnotationContentRotationTest, DeletingAndRestoringPreservesTheRotation) {
    // restoreAnnotation REBUILDS an annotation from AnnotationData rather than
    // mutating one, so it never goes through setRotation and the private keys
    // are not written unless it writes them. Delete a rotated text box, press
    // Ctrl+Z, and the text came back upright while the box kept its widened
    // bounds -- found by a gate agent, not by me.
    //
    // It also needs the pre-rotation content box, which is why AnnotationData
    // carries it: the box cannot be recomputed from the restored /Rect and the
    // angle, because that inversion is singular at 45 degrees. The angle here
    // is deliberately 45.
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);
    const QRectF rect(100, 100, 200, 60);
    const int idx = MakeTextBox(page.get(), rect);
    manager_->setRotation(page.get(), idx, 45.0, rect);

    const AnnotationData captured = manager_->deleteAnnotation(page.get(), idx);
    EXPECT_NEAR(captured.rotationDegrees, 45.0, 1e-3)
        << "the delete path did not capture the angle, so no restore could "
           "put it back";
    ASSERT_TRUE(captured.rotationContentRect.isValid())
        << "the delete path did not capture the pre-rotation content box";

    const int restored = manager_->restoreAnnotation(page.get(), captured);

    const AnnotationData after = manager_->getAnnotation(page.get(), restored);
    EXPECT_NEAR(after.rotationDegrees, 45.0, 1e-3)
        << "the restored annotation lost its angle";

    const auto out = outDir_ / "deleted_then_restored.pdf";
    ASSERT_NO_THROW(document_->saveAs(out));
    ASSERT_TRUE(std::filesystem::exists(out));

    const SavedRotation s = ReadSavedRotation(out);
    ASSERT_TRUE(s.found);
    ASSERT_TRUE(s.hasAp);
    EXPECT_FALSE(s.matrixIsIdentity)
        << "the restored annotation renders upright -- the box came back, the "
           "rotation did not";
    EXPECT_NEAR(AngleOf(s), 45.0, 0.5);
}

TEST_F(AnnotationContentRotationTest, RestoringAnUnrotatedAnnotationAddsNoRotation) {
    // The complement: the restore path must not invent a rotation for
    // annotations that never had one.
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);
    const QRectF rect(120, 140, 180, 50);
    const int idx = MakeTextBox(page.get(), rect);

    const AnnotationData captured = manager_->deleteAnnotation(page.get(), idx);
    EXPECT_DOUBLE_EQ(captured.rotationDegrees, 0.0);

    const int restored = manager_->restoreAnnotation(page.get(), captured);
    EXPECT_DOUBLE_EQ(
        manager_->getAnnotation(page.get(), restored).rotationDegrees, 0.0);

    const auto out = outDir_ / "restored_unrotated.pdf";
    ASSERT_NO_THROW(document_->saveAs(out));
    const SavedRotation s = ReadSavedRotation(out);
    ASSERT_TRUE(s.found);
    EXPECT_TRUE(s.matrixIsIdentity)
        << "an annotation that was never rotated acquired a rotation matrix";
}

TEST_F(AnnotationContentRotationTest, ResizingARotatedAnnotationChangesItsSize) {
    // setRect receives the axis-aligned BOUNDS of the rotated content, not the
    // content box. A move translates that box; a resize has to SOLVE for a new
    // one. Before this, a resize kept the box's size and only re-centred it --
    // so dragging a corner of a rotated box moved it and changed nothing else.
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);
    const QRectF rect(100, 100, 200, 60);
    const int idx = MakeTextBox(page.get(), rect);
    manager_->setRotation(page.get(), idx, 30.0, rect);

    const AnnotationData rotated = manager_->getAnnotation(page.get(), idx);
    const double w0 = rotated.rect.width();
    const double h0 = rotated.rect.height();
    ASSERT_GT(w0, 0.0);
    ASSERT_GT(h0, 0.0);
    ASSERT_TRUE(rotated.rotationContentRect.isValid());
    const double cw0 = rotated.rotationContentRect.width();

    // Grow the bounds AND shift them. The shift is not decoration: a drag
    // centred on the old centre gives dx = dy = 0, and then a positional bug
    // (dropping the translation entirely) is invisible. This drag moves the
    // centre so position is actually under test.
    const QPointF c = rotated.rect.center();
    const QPointF cc0 = rotated.rotationContentRect.center();
    constexpr double kShiftX = 17.0;
    constexpr double kShiftY = -11.0;
    const QRectF bigger(c.x() - w0 * 0.75 + kShiftX,
                        c.y() - h0 * 0.75 + kShiftY,
                        w0 * 1.5, h0 * 1.5);
    manager_->setRect(page.get(), idx, bigger);

    const AnnotationData after = manager_->getAnnotation(page.get(), idx);
    ASSERT_TRUE(after.rotationContentRect.isValid());

    // SIZE.
    EXPECT_GT(after.rotationContentRect.width(), cw0 * 1.05)
        << "resizing a rotated annotation must GROW its content box, not just "
           "re-centre it; width stayed at " << cw0;
    EXPECT_GT(after.rect.width(), w0 * 1.05)
        << "the reported bounds should follow the resize";

    // POSITION, asserted against the REQUESTED shift -- an INPUT.
    //
    // The obvious formulation, "the content box moved by the same delta as the
    // reported bounds", is WORTHLESS here and was caught by mutation: the
    // bounds are DERIVED from the content box, so a mutant that drops the
    // translation moves neither, both deltas collapse to ~0, and the
    // comparison passes. Measured under exactly that mutant: boundsDelta.x() =
    // 7.6e-06. An assertion whose two sides share the defect cannot see it.
    //
    // kShiftX/kShiftY are what the caller ASKED for, so comparing against them
    // is independent of the code under test.
    const QPointF contentDelta = after.rotationContentRect.center() - cc0;
    EXPECT_NEAR(contentDelta.x(), kShiftX, 0.75)
        << "the content box did not follow the drag horizontally: asked for "
        << kShiftX << ", got " << contentDelta.x();
    EXPECT_NEAR(contentDelta.y(), kShiftY, 0.75)
        << "the content box did not follow the drag vertically: asked for "
        << kShiftY << ", got " << contentDelta.y();

    EXPECT_NEAR(after.rotationDegrees, 30.0, 1e-3)
        << "resizing must not lose the rotation";
}

TEST_F(AnnotationContentRotationTest, ResizingAtFortyFiveDegreesStaysSane) {
    // 45 degrees is where the inverse is SINGULAR (its determinant is cos 2t).
    // The fallback must still produce a positive, growing box rather than a
    // degenerate or inverted one.
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);
    const QRectF rect(120, 120, 180, 70);
    const int idx = MakeTextBox(page.get(), rect);
    manager_->setRotation(page.get(), idx, 45.0, rect);

    const AnnotationData rotated = manager_->getAnnotation(page.get(), idx);
    ASSERT_TRUE(rotated.rotationContentRect.isValid());
    const double cw0 = rotated.rotationContentRect.width();
    const double ch0 = rotated.rotationContentRect.height();

    const QPointF c = rotated.rect.center();
    const QPointF cc0 = rotated.rotationContentRect.center();
    const double w0 = rotated.rect.width(), h0 = rotated.rect.height();
    // Off-centre again, for the same reason as the 30-degree case: a
    // centre-preserving drag cannot detect a lost translation.
    constexpr double kShiftX = -13.0;
    constexpr double kShiftY = 9.0;
    manager_->setRect(page.get(), idx,
                      QRectF(c.x() - w0 + kShiftX, c.y() - h0 + kShiftY,
                             w0 * 2.0, h0 * 2.0));

    const AnnotationData after = manager_->getAnnotation(page.get(), idx);
    ASSERT_TRUE(after.rotationContentRect.isValid());
    EXPECT_GT(after.rotationContentRect.width(), 0.0) << "degenerate box";
    EXPECT_GT(after.rotationContentRect.height(), 0.0) << "degenerate box";
    EXPECT_GT(after.rotationContentRect.width(), cw0)
        << "a doubling drag should grow the box even at the singular angle";
    EXPECT_GT(after.rotationContentRect.height(), ch0)
        << "a doubling drag should grow the box even at the singular angle";

    // POSITION must survive the FALLBACK path too. The closed form is unusable
    // here (determinant cos 2t is ~0 at 45 degrees), so this exercises the
    // area-ratio branch -- and an untested fallback is exactly where a fix like
    // this rots. Compared against the REQUESTED shift, not against the reported
    // bounds: the bounds are derived from the content box, so comparing the two
    // would let a dropped translation pass (see the note in the 30-degree case).
    const QPointF contentDelta = after.rotationContentRect.center() - cc0;
    EXPECT_NEAR(contentDelta.x(), kShiftX, 0.75)
        << "content box did not follow the drag horizontally on the fallback "
           "path: asked for " << kShiftX << ", got " << contentDelta.x();
    EXPECT_NEAR(contentDelta.y(), kShiftY, 0.75)
        << "content box did not follow the drag vertically on the fallback "
           "path: asked for " << kShiftY << ", got " << contentDelta.y();
}

// ---------------------------------------------------------------------------
// Undo / redo.
// ---------------------------------------------------------------------------

TEST_F(AnnotationContentRotationTest, UndoAndRedoRoundTripTheAngle) {
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);
    const QRectF rect(100, 100, 200, 60);
    const int idx = MakeTextBox(page.get(), rect);

    AnnotationData before = manager_->getAnnotation(page.get(), idx);
    AnnotationData after = before;
    after.rotationDegrees = 42.0;
    after.rect = QRectF(rect.center().x() - 120, rect.center().y() - 90,
                        240, 180);

    pdfe::commands::UndoManager undo;
    undo.execute(std::make_unique<ModifyAnnotationCommand>(
        manager_.get(), page.get(), idx,
        ModifyAnnotationCommand::Property::Rotation, before, after));

    EXPECT_NEAR(manager_->getAnnotation(page.get(), idx).rotationDegrees,
                42.0, 1e-3);

    undo.undo();
    EXPECT_NEAR(manager_->getAnnotation(page.get(), idx).rotationDegrees,
                0.0, 1e-3) << "undo did not restore the previous angle";

    undo.redo();
    EXPECT_NEAR(manager_->getAnnotation(page.get(), idx).rotationDegrees,
                42.0, 1e-3) << "redo did not reapply the angle";
}

// ---------------------------------------------------------------------------
// Geometry lock.
// ---------------------------------------------------------------------------

TEST_F(AnnotationContentRotationTest, RotatingALockedAnnotationIsRefused) {
    // Rotating is a geometry change, so /Locked must refuse it exactly as it
    // refuses a move or a resize. Without this, /Locked would be a
    // half-enforced flag: honoured for dragging, ignored for the rotate handle.
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);
    const QRectF rect(100, 100, 200, 60);
    const int idx = MakeTextBox(page.get(), rect);

    AnnotationFlags flags{};
    flags.set(AnnotationFlag::Locked);
    manager_->setAnnotationFlags(page.get(), idx, flags);

    EXPECT_ANY_THROW(manager_->setRotation(page.get(), idx, 30.0, rect));
}
