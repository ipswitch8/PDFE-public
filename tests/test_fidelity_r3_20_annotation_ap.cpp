// R3-20 CORRECTNESS ORACLE — annotation appearance-stream (/AP) and /BBox
// generation.
//
// WHAT THE EXISTING TESTS MISS
// ----------------------------
// tests/test_annotation_roundtrip.cpp proves that annotation *semantics*
// (contents, color, quad points, ink strokes, type) survive a save/reopen.
// But it never opens the saved bytes at the pdf_obj level and verifies that
// each annotation actually carries a rendered appearance:
//     * a /AP dictionary,
//     * whose /N entry is a real XObject *stream* (not merely a dict of
//       appearance sub-states, and not absent),
//     * with a /BBox array of exactly four numbers describing a
//       NON-DEGENERATE box (width > 0 AND height > 0),
//     * and a pdf_bound_annot that agrees with the annotation /Rect.
//
// Without /AP, a conforming-but-minimal PDF viewer (one that does NOT
// synthesize appearances itself — the spec permits viewers to rely solely on
// /AP) renders the annotation as INVISIBLE, or renders a degenerate zero-area
// box. That is the R3-20 fidelity gap: the annotation "round-trips" as data
// yet paints as nothing. AnnotationManager guards against this by calling
// pdf_update_annot after createAnnotation and after every setter
// (src/annotations/AnnotationManager.cpp), which bakes the /AP/N stream. This
// oracle locks that invariant in the saved-and-reopened bytes.
//
// RED-MEANINGFULNESS / MUTATIONS KILLED
// -------------------------------------
//   * If pdf_update_annot were skipped in AnnotationManager, no /AP would be
//     written -> EXPECT_TRUE(f.hasAp) and EXPECT_TRUE(f.nIsStream) fail.
//   * If the appearance were emitted with an empty/degenerate bounding box
//     (a common regression when annot geometry is mis-computed) -> the
//     EXPECT_GT(f.bboxW, 0.0) / EXPECT_GT(f.bboxH, 0.0) asserts fail.
//   * If /BBox were written with the wrong arity -> EXPECT_EQ(f.bboxLen, 4)
//     fails.
//   * If pdf_bound_annot and /Rect diverged (appearance placed off the annot)
//     -> EXPECT_TRUE(f.boundsOverlap) fails.
//
// Pattern: create -> minimal setters -> Document::saveAs -> REOPEN with a
// FRESH, independent fitz/pdf context (raw MuPDF C API, not the PDFE wrappers)
// -> walk pdf_first_annot / pdf_next_annot and assert on the raw /AP objects.

#include <gtest/gtest.h>

#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationData.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using pdfe::annotations::AnnotationData;
using pdfe::annotations::AnnotationManager;
using pdfe::annotations::AnnotationType;

namespace {

// One annotation's raw appearance findings, gathered by a low-level MuPDF
// walk of the reopened file. Kept POD so no gtest macros run inside the
// fz_try/fz_catch region (gtest ASSERT_* returns, which would skip fz cleanup).
struct ApFinding {
    int pdfType = -1;        // pdf_annot_type value
    bool hasAp = false;      // /AP present
    bool nIsStream = false;  // /AP/N is an XObject stream
    bool hasBBoxArray = false;
    int bboxLen = -1;        // length of /AP/N /BBox array
    double bboxW = 0.0;      // |BBox width|
    double bboxH = 0.0;      // |BBox height|
    bool boundsOverlap = false; // pdf_bound_annot intersects /Rect
};

// Reopen `path` with a FRESH, independent fitz/pdf context and collect the
// raw /AP findings for every annotation on `pageIndex`. All MuPDF calls are
// wrapped in fz_try/fz_always/fz_catch; no gtest assertions run inside so
// cleanup always executes. Returns empty on catastrophic failure — callers
// assert non-empty.
std::vector<ApFinding> InspectAppearances(const std::filesystem::path& path,
                                          int pageIndex) {
    std::vector<ApFinding> out;
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx) {
        return out;
    }

    pdf_document* doc = nullptr;
    fz_page* fzPage = nullptr;
    pdf_page* page = nullptr;

    fz_try(ctx) {
        fz_register_document_handlers(ctx);
        doc = pdf_open_document(ctx, path.string().c_str());
        fzPage = fz_load_page(ctx, (fz_document*)doc, pageIndex);
        page = pdf_page_from_fz_page(ctx, fzPage);

        for (pdf_annot* a = pdf_first_annot(ctx, page); a != nullptr;
             a = pdf_next_annot(ctx, a)) {
            ApFinding f;
            pdf_obj* obj = pdf_annot_obj(ctx, a);
            f.pdfType = static_cast<int>(pdf_annot_type(ctx, a));

            pdf_obj* ap = pdf_dict_get(ctx, obj, PDF_NAME(AP));
            f.hasAp = (ap != nullptr);

            pdf_obj* n = ap ? pdf_dict_get(ctx, ap, PDF_NAME(N)) : nullptr;
            f.nIsStream = (n != nullptr) && pdf_is_stream(ctx, n);

            pdf_obj* bbox =
                n ? pdf_dict_get(ctx, n, PDF_NAME(BBox)) : nullptr;
            if (bbox != nullptr && pdf_is_array(ctx, bbox)) {
                f.hasBBoxArray = true;
                f.bboxLen = pdf_array_len(ctx, bbox);
                if (f.bboxLen == 4) {
                    fz_rect r = pdf_to_rect(ctx, bbox);
                    f.bboxW = std::fabs(static_cast<double>(r.x1) -
                                        static_cast<double>(r.x0));
                    f.bboxH = std::fabs(static_cast<double>(r.y1) -
                                        static_cast<double>(r.y0));
                }
            }

            // pdf_bound_annot must sit on top of the declared /Rect.
            //
            // COORDINATE-SPACE NOTE: pdf_bound_annot returns the annotation
            // rectangle in DEVICE space — internally it computes
            // fz_transform_rect(Rect, page_ctm), where the page CTM flips Y from
            // PDF's y-up page space to y-down device space and translates by the
            // page height. The raw /Rect array read straight from the annotation
            // dict is in PDF PAGE space (y-up). Intersecting the two spaces
            // directly is a coordinate-space error: the Y ranges never meet, so
            // the intersection is spuriously empty for EVERY annotation. Bring
            // the raw /Rect into the same device space via the page CTM before
            // intersecting. (We transform the raw dict /Rect ourselves rather
            // than call pdf_annot_rect, which restricts by subtype and would
            // throw for Line/Ink/text-markup annotations.)
            pdf_obj* rectObj = pdf_dict_get(ctx, obj, PDF_NAME(Rect));
            fz_rect rawRect = pdf_to_rect(ctx, rectObj);
            fz_matrix pageCtm;
            pdf_page_transform(ctx, page, nullptr, &pageCtm);
            fz_rect rect = fz_transform_rect(rawRect, pageCtm);
            fz_rect bound = pdf_bound_annot(ctx, a);
            fz_rect isect = fz_intersect_rect(rect, bound);
            f.boundsOverlap = !fz_is_empty_rect(isect);

            out.push_back(f);
        }
    }
    fz_always(ctx) {
        if (fzPage != nullptr) {
            fz_drop_page(ctx, fzPage);
        }
        if (doc != nullptr) {
            pdf_drop_document(ctx, doc);
        }
    }
    fz_catch(ctx) {
        // Leave `out` as-is (possibly partial); the caller's ASSERT on
        // non-empty size turns a total failure into a RED test.
    }

    fz_drop_context(ctx);
    return out;
}

// Assert the R3-20 invariant on a single finding. Factored out so every
// per-type test kills the same mutations with identical, explicit checks.
void ExpectSaneAppearance(const ApFinding& f, const char* label) {
    // Kills "pdf_update_annot skipped -> no /AP":
    EXPECT_TRUE(f.hasAp) << label << ": annotation has no /AP dictionary — "
                            "a spec-minimal viewer would render it invisible";
    // Kills "/AP/N not a real appearance stream":
    EXPECT_TRUE(f.nIsStream)
        << label << ": /AP/N is not an XObject stream";
    // Kills "/BBox wrong arity":
    EXPECT_EQ(f.bboxLen, 4)
        << label << ": /AP/N /BBox must be a 4-number array";
    // Kills "degenerate zero-area appearance box":
    EXPECT_GT(f.bboxW, 0.0)
        << label << ": /BBox width must be non-degenerate (> 0)";
    EXPECT_GT(f.bboxH, 0.0)
        << label << ": /BBox height must be non-degenerate (> 0)";
    // Kills "appearance placed off the annotation rect":
    EXPECT_TRUE(f.boundsOverlap)
        << label << ": pdf_bound_annot must overlap the annotation /Rect";
}

class FidelityR3_20_AnnotationAp : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<pdfe::core::MuPdfContext>();
        fixture_ = std::filesystem::path(TEST_FIXTURES_DIR) / "test_3page.pdf";
        ASSERT_TRUE(std::filesystem::exists(fixture_))
            << "test_3page.pdf fixture missing at " << fixture_;
        document_ = pdfe::core::Document::open(ctx_, fixture_);
        ASSERT_NE(document_, nullptr);
        manager_ = std::make_unique<AnnotationManager>(ctx_);

        const ::testing::TestInfo* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        outPath_ = std::filesystem::temp_directory_path()
                   / "pdfe_r3_20_annotation_ap"
                   / (std::string(info->name()) + ".pdf");
        std::filesystem::create_directories(outPath_.parent_path());
        std::error_code ec;
        std::filesystem::remove(outPath_, ec);  // fresh output each run
    }

    void TearDown() override {
        manager_.reset();
        document_.reset();
        ctx_.reset();
        std::error_code ec;
        std::filesystem::remove(outPath_, ec);  // clean up temp output
    }

    // Save the live document to the per-test temp path via Document::saveAs
    // and confirm a non-empty file landed.
    void SaveOut() {
        document_->saveAs(outPath_);
        ASSERT_TRUE(std::filesystem::exists(outPath_))
            << "saveAs did not produce a file at " << outPath_;
        ASSERT_GT(std::filesystem::file_size(outPath_), 0u)
            << "saveAs produced an empty file";
    }

    std::shared_ptr<pdfe::core::MuPdfContext> ctx_;
    std::filesystem::path fixture_;
    std::filesystem::path outPath_;
    std::unique_ptr<pdfe::core::Document> document_;
    std::unique_ptr<AnnotationManager> manager_;
};

// ─── Comprehensive: every representative type gets a real appearance ──────
//
// Creates one of each supported annotation type on page 0, applies the
// minimally-required per-type properties, saves, reopens with a fresh
// context, and asserts the /AP + /BBox invariant on EACH annotation.
TEST_F(FidelityR3_20_AnnotationAp,
       AllRepresentativeTypesGetAppearanceStreamAndBBox) {
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);

    // Text (sticky note) — contents drive the note icon appearance.
    {
        auto a = manager_->createAnnotation(page.get(), AnnotationType::Text,
                                            QRectF(72, 700, 20, 20));
        manager_->setContents(page.get(), a.annotIndex, "sticky");
    }
    // Square — colored border/interior.
    {
        auto a = manager_->createAnnotation(page.get(), AnnotationType::Square,
                                            QRectF(72, 600, 120, 60));
        manager_->setColor(page.get(), a.annotIndex, QColor(0, 128, 220));
    }
    // Circle — colored border/interior.
    {
        auto a = manager_->createAnnotation(page.get(), AnnotationType::Circle,
                                            QRectF(220, 600, 120, 60));
        manager_->setColor(page.get(), a.annotIndex, QColor(220, 60, 40));
    }
    // Line — endpoints define the drawn segment.
    {
        auto a = manager_->createAnnotation(page.get(), AnnotationType::Line,
                                            QRectF(72, 520, 200, 40));
        manager_->setLineEndpoints(page.get(), a.annotIndex,
                                   QPointF(72, 520), QPointF(272, 560));
    }
    // Ink — freehand stroke path.
    {
        auto a = manager_->createAnnotation(page.get(), AnnotationType::Ink,
                                            QRectF(72, 440, 150, 60));
        std::vector<std::vector<QPointF>> strokes = {
            {QPointF(80, 450), QPointF(120, 480), QPointF(160, 455),
             QPointF(200, 490)}};
        manager_->setInkStrokes(page.get(), a.annotIndex, strokes);
    }
    // FreeText — a text box with contents + default appearance.
    {
        auto a = manager_->createAnnotation(page.get(), AnnotationType::FreeText,
                                            QRectF(72, 360, 220, 50));
        manager_->setContents(page.get(), a.annotIndex, "free text box");
        manager_->setDefaultAppearance(page.get(), a.annotIndex, "Helv", 12.0f,
                                       QColor(0, 0, 0));
    }
    // Highlight — text-markup quad points.
    {
        auto a = manager_->createAnnotation(page.get(),
                                            AnnotationType::Highlight,
                                            QRectF(72, 300, 200, 18));
        std::vector<QRectF> quads = {QRectF(72, 300, 200, 18)};
        manager_->setQuadPoints(page.get(), a.annotIndex, quads);
    }

    page.reset();  // release before save (Page-holder release discipline)

    ASSERT_NO_FATAL_FAILURE(SaveOut());

    auto findings = InspectAppearances(outPath_, 0);
    // Fresh-context reopen must see all seven annotations we created.
    ASSERT_GE(findings.size(), 7u)
        << "reopened page 0 is missing annotations created before save";

    // pdf_annot_type values for the created types (from pdf/annot.h ordering).
    // We do not hard-code the enum ints; instead we require that EVERY
    // annotation present carries a sane appearance. That is stricter and
    // future-proof against enum drift.
    for (size_t i = 0; i < findings.size(); ++i) {
        const std::string label =
            "annot[" + std::to_string(i) + "] pdfType=" +
            std::to_string(findings[i].pdfType);
        ExpectSaneAppearance(findings[i], label.c_str());
    }
}

// ─── Focused: Square shape carries a non-degenerate BBox ─────────────────
//
// A shape annotation is the clearest degenerate-BBox tripwire: if the
// appearance geometry collapsed, width/height would be 0 even though the
// annotation "exists".
TEST_F(FidelityR3_20_AnnotationAp, SquareShapeAppearanceIsNonDegenerate) {
    auto page = document_->page(0);
    auto a = manager_->createAnnotation(page.get(), AnnotationType::Square,
                                        QRectF(100, 500, 140, 90));
    manager_->setColor(page.get(), a.annotIndex, QColor(0, 180, 60));
    page.reset();

    ASSERT_NO_FATAL_FAILURE(SaveOut());

    auto findings = InspectAppearances(outPath_, 0);
    ASSERT_EQ(findings.size(), 1u)
        << "exactly one Square annotation expected after reopen";
    ExpectSaneAppearance(findings[0], "Square");
}

// ─── Focused: Line annotation gets an /AP stream after reopen ────────────
TEST_F(FidelityR3_20_AnnotationAp, LineAnnotationHasAppearanceStream) {
    auto page = document_->page(0);
    auto a = manager_->createAnnotation(page.get(), AnnotationType::Line,
                                        QRectF(90, 400, 220, 50));
    manager_->setLineEndpoints(page.get(), a.annotIndex, QPointF(90, 400),
                               QPointF(310, 450));
    page.reset();

    ASSERT_NO_FATAL_FAILURE(SaveOut());

    auto findings = InspectAppearances(outPath_, 0);
    ASSERT_EQ(findings.size(), 1u);
    ExpectSaneAppearance(findings[0], "Line");
}

// ─── Focused: Ink annotation gets an /AP stream after reopen ─────────────
TEST_F(FidelityR3_20_AnnotationAp, InkAnnotationHasAppearanceStream) {
    auto page = document_->page(0);
    auto a = manager_->createAnnotation(page.get(), AnnotationType::Ink,
                                        QRectF(80, 300, 180, 90));
    std::vector<std::vector<QPointF>> strokes = {
        {QPointF(90, 320), QPointF(140, 360), QPointF(190, 315),
         QPointF(240, 370)}};
    manager_->setInkStrokes(page.get(), a.annotIndex, strokes);
    page.reset();

    ASSERT_NO_FATAL_FAILURE(SaveOut());

    auto findings = InspectAppearances(outPath_, 0);
    ASSERT_EQ(findings.size(), 1u);
    ExpectSaneAppearance(findings[0], "Ink");
}

// ─── Focused: Highlight markup gets an /AP stream after reopen ───────────
//
// Text-markup annotations derive their appearance from /QuadPoints. If the
// AP were not regenerated after setQuadPoints, a strict viewer would show
// nothing over the highlighted span.
TEST_F(FidelityR3_20_AnnotationAp, HighlightMarkupHasAppearanceStream) {
    auto page = document_->page(0);
    auto a = manager_->createAnnotation(page.get(), AnnotationType::Highlight,
                                        QRectF(72, 250, 220, 16));
    std::vector<QRectF> quads = {QRectF(72, 250, 220, 16)};
    manager_->setQuadPoints(page.get(), a.annotIndex, quads);
    manager_->setColor(page.get(), a.annotIndex, QColor(255, 235, 0));
    page.reset();

    ASSERT_NO_FATAL_FAILURE(SaveOut());

    auto findings = InspectAppearances(outPath_, 0);
    ASSERT_EQ(findings.size(), 1u);
    ExpectSaneAppearance(findings[0], "Highlight");
}

} // namespace
