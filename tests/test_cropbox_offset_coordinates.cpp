// -*- coding: utf-8 -*-
//
// Hostile class 9 (meta-audit A1-25): /CropBox != /MediaBox with a LARGE
// offset. Before this file the only CropBox coverage was
// tests/test_crop_tool.cpp, which round-trips getPageCropBox/setPageCropBox
// getters and setters — it never proves that a coordinate-dependent OPERATION
// lands in the right place on a cropped page.
//
// Fixture: tests/fixtures/cropbox_offset.pdf (tests/fixtures/gen_cropbox_offset.py)
//
//     MediaBox [0 0 1200 1600]      CropBox [400 500 1000 1100]
//
// MuPDF's "fitz space" — the space Page::boundingBox(), Page::extractText(),
// Page::render() and the annotation/redaction rect APIs all speak — puts the
// origin at the CROPBOX's top-left with Y running DOWN
// (pdf_page_obj_transform_box: "Always use CropBox to set origin to top left"
// then "Translate page origin of CropBox to 0,0"). So:
//
//     fitz_x = pdf_x - 400            pdf_x = fitz_x + 400
//     fitz_y = 1100 - pdf_y           pdf_y = 1100 - fitz_y
//
// Every oracle below is anchored to the literals authored in
// gen_cropbox_offset.py (and, for glyph advances, to the fixed standard-14
// Helvetica AFM widths) — never to what PDFE currently emits.
//
// The mutation this file exists to kill is "drop (or double) the CropBox-origin
// translation": with a 400/500 offset any such slip moves a rect by hundreds of
// points, off the visible page entirely.

#include <gtest/gtest.h>

#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationType.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/TextBlock.h"
#include "core/TextLine.h"
#include "redaction/RedactionData.h"
#include "redaction/RedactionManager.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <QColor>
#include <QImage>
#include <QRectF>

#include <array>
#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

// ── Authored ground truth (mirrors gen_cropbox_offset.py) ───────────────────
constexpr double kCropX0 = 400.0;   // /CropBox lower-left x, PDF user space
constexpr double kCropY1 = 1100.0;  // /CropBox upper-right y, PDF user space
constexpr double kCropW  = 600.0;
constexpr double kCropH  = 600.0;

constexpr double kMediaW = 1200.0;  // must NOT be what the page reports
constexpr double kMediaH = 1600.0;

constexpr double kFontSize = 24.0;

// Red block, PDF user space [450 1030 560 1070]  ->  fitz [50 30 160 70].
constexpr double kBlockFitzX0 = 50.0;
constexpr double kBlockFitzY0 = 30.0;
constexpr double kBlockFitzX1 = 160.0;
constexpr double kBlockFitzY1 = 70.0;

// Text baselines, PDF user space -> fitz space.
constexpr double kRedactmeFitzX = 450.0 - kCropX0;   //  50
constexpr double kRedactmeFitzY = kCropY1 - 1000.0;  // 100
constexpr double kKeepmeFitzX   = 450.0 - kCropX0;   //  50
constexpr double kKeepmeFitzY   = kCropY1 -  700.0;  // 400
constexpr double kOutsideFitzX  = 100.0 - kCropX0;   // -300  (outside the crop)
constexpr double kOutsideFitzY  = kCropY1 -  200.0;  // 900   (outside the crop)

// Standard-14 Helvetica advance widths (AFM, /1000 em) x 24 pt. These are
// fixed by the PDF specification's built-in font metrics, so they are authored
// ground truth exactly like the coordinates above.
constexpr double kRedactmeWidth = 5611.0 / 1000.0 * kFontSize;  // 134.664
constexpr double kKeepmeWidth   = 4168.0 / 1000.0 * kFontSize;  // 100.032
constexpr double kOutsideWidth  = 6557.0 / 1000.0 * kFontSize;  // 157.368

// The redaction rect, fitz space. Fully covers "REDACTME" (whose glyph box
// starts at x=50, baseline y=100) while staying clear of the red block
// (fitz y 30..70) and of "KEEPME" (baseline fitz y 400).
inline QRectF redactRectFitz() { return QRectF(45.0, 72.0, 155.0, 43.0); }

// The authored annotation rect, fitz space — deliberately identical to the red
// block, so the expected stored /Rect is the block's own user-space extent.
inline QRectF annotRectFitz()
{
    return QRectF(kBlockFitzX0, kBlockFitzY0,
                  kBlockFitzX1 - kBlockFitzX0, kBlockFitzY1 - kBlockFitzY0);
}

std::filesystem::path fixturePath()
{
    return std::filesystem::path(TEST_FIXTURES_DIR) / "cropbox_offset.pdf";
}

inline long pdfeProcessId()
{
#ifdef _WIN32
    return static_cast<long>(_getpid());
#else
    return static_cast<long>(getpid());
#endif
}

// Unique per-call working directory: the fixture is COPIED in and every output
// path is distinct, so --gtest_repeat and concurrent processes never collide on
// a still-locked file (the lesson from pipeline_jpx_optin_49_50 / #49).
std::filesystem::path uniqueDir(const char* stem)
{
    static std::atomic<unsigned> counter{0};
    const unsigned n = counter.fetch_add(1, std::memory_order_relaxed);
    auto d = std::filesystem::temp_directory_path() / "pdfe_cropbox_offset"
             / (std::string(stem) + "_" + std::to_string(pdfeProcessId())
                + "_" + std::to_string(n));
    std::filesystem::create_directories(d);
    return d;
}

// Raw /Rect (PDF user space, as physically stored) of annotation 0 on page 0.
// Read with a private MuPDF context so it is independent of the product's own
// fitz-space accessors — the point of the test is that the STORED coordinates
// are absolute user space.
std::array<float, 4> firstAnnotStoredRect(const std::filesystem::path& file)
{
    std::array<float, 4> out{0.0f, 0.0f, 0.0f, 0.0f};
    // Every C++ object this frame needs is built BEFORE fz_try: the
    // setjmp/longjmp region must not construct or destroy any.
    const std::string pathStr = file.string();
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx) return out;
    fz_register_document_handlers(ctx);
    pdf_document* doc = nullptr;
    fz_var(doc);
    fz_var(out);
    fz_try(ctx) {
        doc = pdf_open_document(ctx, pathStr.c_str());
        pdf_obj* pageObj = pdf_lookup_page_obj(ctx, doc, 0);
        pdf_obj* annots = pdf_dict_get(ctx, pageObj, PDF_NAME(Annots));
        pdf_obj* annot = pdf_array_get(ctx, annots, 0);
        pdf_obj* rect = pdf_dict_get(ctx, annot, PDF_NAME(Rect));
        for (int i = 0; i < 4; ++i)
            out[static_cast<size_t>(i)] =
                pdf_to_real(ctx, pdf_array_get(ctx, rect, i));
    }
    fz_always(ctx) {
        pdf_drop_document(ctx, doc);
    }
    fz_catch(ctx) {
        // Leave the zeroed rect; the caller's assertions turn that into a
        // loud failure rather than a skip.
    }
    fz_drop_context(ctx);
    return out;
}

// Find the extracted block whose concatenated text contains `token`.
const pdfe::core::TextBlock* findBlock(
    const std::vector<pdfe::core::TextBlock>& blocks, const char* token)
{
    for (const auto& b : blocks) {
        if (b.text().find(token) != std::string::npos) return &b;
    }
    return nullptr;
}

class CropBoxOffsetTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(std::filesystem::exists(fixturePath()))
            << "cropbox_offset.pdf is a COMMITTED fixture — regenerate with "
               "python tests/fixtures/gen_cropbox_offset.py";
        context_ = std::make_shared<pdfe::core::MuPdfContext>();
        doc_ = pdfe::core::Document::open(context_, fixturePath());
        ASSERT_NE(doc_, nullptr);
        ASSERT_EQ(doc_->pageCount(), 1);
        page_ = doc_->page(0);
        ASSERT_NE(page_, nullptr);
    }

    void TearDown() override
    {
        page_.reset();
        doc_.reset();
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    std::unique_ptr<pdfe::core::Document> doc_;
    std::unique_ptr<pdfe::core::Page> page_;
};

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 1. Page geometry follows the CropBox, not the MediaBox.
// Kills: any mutation that reads /MediaBox where the visible box is required
// (e.g. pdf_page_transform_box's FZ_CROP_BOX -> FZ_MEDIA_BOX, or a PDFE helper
// falling back to the MediaBox when a CropBox IS present).
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(CropBoxOffsetTest, PageBoundsAreTheCropBoxNotTheMediaBox)
{
    const QRectF b = page_->boundingBox();

    EXPECT_NEAR(b.width(),  kCropW, 0.01)
        << "page width must be the CropBox width (600), not the MediaBox "
           "width (" << kMediaW << ")";
    EXPECT_NEAR(b.height(), kCropH, 0.01)
        << "page height must be the CropBox height (600), not the MediaBox "
           "height (" << kMediaH << ")";

    // Fitz space is rooted at the CropBox's top-left, so the reported origin is
    // (0,0) — NOT the CropBox's user-space corner (400,500) and not the
    // MediaBox corner either.
    EXPECT_NEAR(b.left(), 0.0, 0.01);
    EXPECT_NEAR(b.top(),  0.0, 0.01);
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. Rasterisation covers the CropBox and puts authored ink at authored pixels.
// Kills: a MediaBox-sized raster (1200x1600 instead of 600x600) and any
// mutation that renders without the -CropBox.x0 / -CropBox.y0 translation
// (which would put the red block outside the raster entirely).
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(CropBoxOffsetTest, RenderRasterisesTheCropBoxAtAuthoredPixelPositions)
{
    const QImage img = page_->render(QRectF(), 1.0f);
    ASSERT_FALSE(img.isNull()) << "render() returned a null image";

    EXPECT_EQ(img.width(),  static_cast<int>(kCropW));
    EXPECT_EQ(img.height(), static_cast<int>(kCropH));
    ASSERT_GE(img.width(),  static_cast<int>(kBlockFitzX1));
    ASSERT_GE(img.height(), static_cast<int>(kBlockFitzY1));

    // Well inside the authored red block (fitz x 50..160, y 30..70), so
    // anti-aliasing at the edges cannot reach the sample.
    const QColor inBlock = img.pixelColor(105, 50);
    EXPECT_GT(inBlock.red(),   200) << "authored red block missing at fitz (105,50)";
    EXPECT_LT(inBlock.green(),  60);
    EXPECT_LT(inBlock.blue(),   60);

    // Blank paper between the block and the "REDACTME" baseline region.
    const QColor blank = img.pixelColor(400, 250);
    EXPECT_GT(blank.red(),   200);
    EXPECT_GT(blank.green(), 200);
    EXPECT_GT(blank.blue(),  200);
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. Extracted text boxes are CropBox-relative.
// Kills: dropping the CropBox translation in the stext path — "REDACTME" would
// report left=450 (user space) or 50-400 offsets instead of the authored 50.
// The width oracle is the fixed standard-14 Helvetica advance sum.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(CropBoxOffsetTest, ExtractedTextBoxesAreCropBoxRelative)
{
    const std::vector<pdfe::core::TextBlock> blocks = page_->extractText();
    ASSERT_FALSE(blocks.empty()) << "fixture must extract native text";

    const pdfe::core::TextBlock* redactme = findBlock(blocks, "REDACTME");
    ASSERT_NE(redactme, nullptr) << "fixture token REDACTME not extracted";
    const QRectF r = redactme->boundingBox;

    EXPECT_NEAR(r.left(), kRedactmeFitzX, 0.5)
        << "REDACTME was authored at PDF user x=450 with CropBox.x0=400, so it "
           "must extract at fitz x=50; got " << r.left();
    EXPECT_NEAR(r.width(), kRedactmeWidth, 1.0)
        << "Helvetica-24 advance sum for \"REDACTME\" is 134.664 pt";
    // The authored baseline must lie strictly inside the reported glyph box;
    // ascent/descent are font-dependent, so only the baseline itself is pinned.
    EXPECT_LT(r.top(),    kRedactmeFitzY);
    EXPECT_GT(r.bottom(), kRedactmeFitzY);
    EXPECT_GT(r.top(),    kRedactmeFitzY - 2.0 * kFontSize);
    EXPECT_LT(r.bottom(), kRedactmeFitzY + 1.0 * kFontSize);

    const pdfe::core::TextBlock* keepme = findBlock(blocks, "KEEPME");
    ASSERT_NE(keepme, nullptr) << "fixture token KEEPME not extracted";
    const QRectF k = keepme->boundingBox;

    EXPECT_NEAR(k.left(), kKeepmeFitzX, 0.5);
    EXPECT_NEAR(k.width(), kKeepmeWidth, 1.0)
        << "Helvetica-24 advance sum for \"KEEPME\" is 100.032 pt";
    EXPECT_LT(k.top(),    kKeepmeFitzY);
    EXPECT_GT(k.bottom(), kKeepmeFitzY);
    EXPECT_GT(k.top(),    kKeepmeFitzY - 2.0 * kFontSize);
    EXPECT_LT(k.bottom(), kKeepmeFitzY + 1.0 * kFontSize);

    // The two tokens were authored 300 pt apart on the PDF's Y axis; that gap
    // must survive the transform (a sign flip would collapse or invert it).
    EXPECT_NEAR(k.top() - r.top(), 300.0, 1.0)
        << "REDACTME (user y=1000) and KEEPME (user y=700) are 300 pt apart";
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. Content OUTSIDE the CropBox but inside the MediaBox is still extracted,
// and is reported in the SAME CropBox-relative space — i.e. with NEGATIVE
// coordinates, not re-based onto the MediaBox.
//
// PDFE calls fz_new_stext_page_from_page with no clip rect
// (src/core/TextExtractor.cpp), and MuPDF's stext device does not clip to the
// page box, so out-of-crop text is part of the extracted (and therefore
// searchable / redactable) content. That is the correct editor contract — a
// CropBox is a view hint, the content is really there — but it MUST be reported
// in the crop-relative space or every downstream rect is wrong.
//
// Kills: re-basing extraction onto the MediaBox (OUTSIDEBOX would then report
// left=+100 instead of -300) and any clamp-to-page-box mutation (the token
// would vanish).
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(CropBoxOffsetTest, OutOfCropTextIsExtractedInCropBoxRelativeCoordinates)
{
    const std::vector<pdfe::core::TextBlock> blocks = page_->extractText();
    const pdfe::core::TextBlock* outside = findBlock(blocks, "OUTSIDEBOX");
    ASSERT_NE(outside, nullptr)
        << "OUTSIDEBOX was authored inside the MediaBox but outside the "
           "CropBox; PDFE extracts with no clip rect, so it must still be "
           "reported (content outside the crop is still in the file)";

    const QRectF o = outside->boundingBox;
    EXPECT_NEAR(o.left(), kOutsideFitzX, 0.5)
        << "authored at PDF user x=100 with CropBox.x0=400 => fitz x=-300; got "
        << o.left();
    EXPECT_LT(o.left(), 0.0)
        << "out-of-crop content must keep NEGATIVE crop-relative coordinates, "
           "not be re-based onto the MediaBox";
    EXPECT_NEAR(o.width(), kOutsideWidth, 1.0)
        << "Helvetica-24 advance sum for \"OUTSIDEBOX\" is 157.368 pt";
    EXPECT_LT(o.top(),    kOutsideFitzY);
    EXPECT_GT(o.bottom(), kOutsideFitzY);
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. Annotation placement: a rect handed to the product in fitz space must be
// STORED as the authored ABSOLUTE PDF user-space rect.
// Kills: dropping (or double-applying) the CropBox-origin term in the
// fitz->user inverse transform — /Rect would come back as [50 -30 160 -70]-ish
// or [850 1530 960 1570], both hundreds of points from the authored block.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(CropBoxOffsetTest, AnnotationRectRoundTripsToAuthoredUserSpaceRect)
{
    const auto dir = uniqueDir("annot_rect");
    const auto out = dir / "annot_rect_out.pdf";

    {
        pdfe::annotations::AnnotationManager mgr(context_);
        const auto data = mgr.createAnnotation(
            page_.get(), pdfe::annotations::AnnotationType::Square,
            annotRectFitz());
        ASSERT_NE(data.annotIndex, -1);
        page_.reset();
        doc_->saveAs(out);
        doc_.reset();
    }
    ASSERT_TRUE(std::filesystem::exists(out));

    const std::array<float, 4> rect = firstAnnotStoredRect(out);

    // Authored red-block extent, PDF user space: [450 1030 560 1070].
    // Tolerance 2 pt absorbs the annotation border half-width MuPDF may fold
    // into the stored rect; the mutation this kills displaces the rect by 400
    // or 500 pt, i.e. by 200x the tolerance.
    EXPECT_NEAR(rect[0], 450.0f,  2.0f) << "/Rect x0";
    EXPECT_NEAR(rect[1], 1030.0f, 2.0f) << "/Rect y0";
    EXPECT_NEAR(rect[2], 560.0f,  2.0f) << "/Rect x1";
    EXPECT_NEAR(rect[3], 1070.0f, 2.0f) << "/Rect y1";

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. THE PRIORITY CASE (audit note on A1-25): redaction on a large-offset
// CropBox page must destroy the content it was aimed at.
//
// Kills: adding or dropping the CropBox-origin subtraction anywhere on the
// mark -> annot -> pdf_redact_page path. With a 400/500 offset a mis-based rect
// lands on blank paper (or off the MediaBox entirely), so nothing is removed
// and REDACTME SURVIVES -> RED. The KEEPME / OUTSIDEBOX controls make the
// converse failure — "passed because redaction nuked the whole page" —
// impossible.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(CropBoxOffsetTest, RedactionOnCropBoxOffsetPageRemovesTheTargetedToken)
{
    page_.reset();
    doc_.reset();

    const auto dir = uniqueDir("redact");
    const auto src = dir / "cropbox_offset_src.pdf";
    const auto out = dir / "cropbox_offset_redacted.pdf";
    std::filesystem::copy_file(fixturePath(), src,
                               std::filesystem::copy_options::overwrite_existing);

    {
        auto doc = pdfe::core::Document::open(context_, src);
        ASSERT_NE(doc, nullptr);
        auto page = doc->page(0);
        ASSERT_NE(page, nullptr);

        // Pre-condition: the tokens are there before we redact, so an
        // "absent" assertion below cannot pass vacuously.
        const std::string before = page->extractPlainText();
        ASSERT_NE(before.find("REDACTME"), std::string::npos);
        ASSERT_NE(before.find("KEEPME"),   std::string::npos);

        pdfe::redaction::RedactionManager mgr(context_);
        const auto mark = mgr.createRedactionMark(page.get(), redactRectFitz());
        ASSERT_NE(mark.annotIndex, -1);

        ASSERT_TRUE(mgr.applyPageRedactions(page.get()))
            << "applyPageRedactions must report that it applied the mark";

        page.reset();
        doc->saveAs(out);
        doc.reset();
    }
    ASSERT_TRUE(std::filesystem::exists(out));

    auto reopened = pdfe::core::Document::open(context_, out);
    ASSERT_NE(reopened, nullptr);
    auto rpage = reopened->page(0);
    ASSERT_NE(rpage, nullptr);
    const std::string after = rpage->extractPlainText();

    EXPECT_EQ(after.find("REDACTME"), std::string::npos)
        << "REDACTME survived a redaction aimed at fitz rect (45,72,155,43) — "
           "the mark's rect was not converted through the CropBox origin "
           "(400,500), so it landed on blank paper. Extracted text was: "
        << after;
    EXPECT_NE(after.find("KEEPME"), std::string::npos)
        << "KEEPME sits 300 pt away and must SURVIVE — a redaction that "
           "removes it is over-broad, not correct";
    EXPECT_NE(after.find("OUTSIDEBOX"), std::string::npos)
        << "OUTSIDEBOX is outside both the redaction rect and the CropBox; it "
           "must survive too";

    rpage.reset();
    reopened.reset();

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
