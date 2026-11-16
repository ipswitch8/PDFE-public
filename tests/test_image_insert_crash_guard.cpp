// PHASE1_HARDENING: skip=0 expect_ge=0 no_throw=1  (2026-11-14)
//
// Regression guard for GitHub #392 (image-insert crash).
//
// Root cause: ImageManager::insertImage ran the MuPDF work inside
// MuPdfContext::execute() with NO C++ exception boundary. Any recoverable
// MuPDF fault (OOM/limit on a large document, or a NULL pdf_document/pdf_page
// for a non-PDF document) was converted by execute() into a thrown
// MuPdfException that unwound uncaught through the Qt slot -> std::terminate ->
// immediate process death.
//
// The fix adds (1) a try/catch boundary in insertImage that turns any
// MuPdfException / std::bad_alloc / std::exception into a graceful
// result.success == false with a user-facing errorMessage, and (2) a
// deterministic guard inside the execute() lambda: when the target is not a
// PDF (pdf_document_from_fz_document / pdf_page_from_fz_page return NULL) it
// fz_throws FZ_ERROR_ARGUMENT, which the boundary turns into that graceful
// failure.
//
// This test drives the non-PDF path, which is the cheap, deterministic trigger
// for the boundary. It must EXPECT_NO_THROW and EXPECT_FALSE(result.success).
//
// Deterministic non-PDF fixture: an SVG file. MuPDF's fz_open_document selects
// its document handler by the file extension (".svg" -> the SVG handler, which
// is compiled in: FZ_ENABLE_SVG). pdf_specifics() on that fz_document is NULL,
// so Document::isPdf() is false and getPdfDoc()/getPdfPage() return NULL inside
// insertImage. The file also carries a "%PDF-" marker inside an XML comment so
// it satisfies Document::open()'s _looks_like_pdf() magic-byte gate (that gate
// scans the first 1024 bytes); the marker is inert to the SVG parser and does
// not change handler selection (which is by extension).

#include <gtest/gtest.h>
#include "image/ImageManager.h"
#include "image/ImageData.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"
#include <QImage>
#include <QRectF>
#include <filesystem>
#include <fstream>
#include <memory>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

using namespace pdfe::core;
using namespace pdfe::image;

class ImageInsertCrashGuardTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<MuPdfContext>();
        manager_ = std::make_unique<ImageManager>(context_);

        // A real, loadable raster to insert (QImage must decode it).
        insertPngPath_ = std::string(TEST_FIXTURES_DIR) + "/test_image_rgb.png";

        // Write the deterministic non-PDF (SVG) fixture to a temp file.
        namespace fs = std::filesystem;
        svgPath_ = (fs::temp_directory_path() /
                    "pdfe_392_nonpdf_fixture.svg").string();
        std::ofstream out(svgPath_, std::ios::binary | std::ios::trunc);
        // The "%PDF-" marker lives in an XML comment to pass the open() gate;
        // the body is a valid 1-page SVG the SVG handler opens as a non-PDF doc.
        out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
               "<!-- %PDF-1.7 marker: satisfies PDFE open() magic-byte gate; "
               "this file is really an SVG, not a PDF -->\n"
               "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"120\" "
               "height=\"90\" viewBox=\"0 0 120 90\">\n"
               "  <rect x=\"0\" y=\"0\" width=\"120\" height=\"90\" "
               "fill=\"#cccccc\"/>\n"
               "  <rect x=\"10\" y=\"10\" width=\"40\" height=\"30\" "
               "fill=\"#336699\"/>\n"
               "</svg>\n";
        out.close();
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove(svgPath_, ec);
    }

    // Open the SVG fixture as a non-PDF Document. Returns nullptr and records a
    // skip reason if the environment could not produce the non-PDF condition.
    std::unique_ptr<Document> openNonPdfDocument() {
        if (!std::filesystem::exists(svgPath_)) return nullptr;
        std::unique_ptr<Document> doc;
        try {
            doc = Document::open(context_, svgPath_);
        } catch (...) {
            return nullptr;
        }
        return doc;
    }

    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<ImageManager> manager_;
    std::string insertPngPath_;
    std::string svgPath_;
};

// Confirms the fixture really is a non-PDF document that MuPDF opens with a
// valid first page. This is the precondition the two boundary tests rely on;
// if it does not hold the trigger would be vacuous, so assert it explicitly.
TEST_F(ImageInsertCrashGuardTest, Fixture_IsNonPdfDocumentWithAPage) {
    auto doc = openNonPdfDocument();
    ASSERT_NE(doc, nullptr) << "SVG non-PDF fixture failed to open: " << svgPath_;
    EXPECT_FALSE(doc->isPdf())
        << "Fixture opened as a PDF; the non-PDF insert trigger would be vacuous";
    ASSERT_GE(doc->pageCount(), 1);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr) << "Non-PDF document produced no page(0)";
}

// (a) Inserting an image into a NON-PDF document must fail gracefully, never
// crash or throw. Before the fix this hit a NULL pdf_document inside the
// execute() lambda (pdf_add_image on a NULL doc) and the resulting MuPDF fault
// escaped as an uncaught MuPdfException (or an outright AV), terminating the
// process. After the fix the boundary catches it: success == false with a
// non-empty, path-free message.
TEST_F(ImageInsertCrashGuardTest, NonPdfDocument_InsertFailsGracefully_DoesNotCrash) {
    auto doc = openNonPdfDocument();
    ASSERT_NE(doc, nullptr) << "SVG non-PDF fixture failed to open: " << svgPath_;
    ASSERT_FALSE(doc->isPdf());
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    InsertImageOptions opts;
    opts.filePath = insertPngPath_;         // a real, decodable image
    opts.targetRect = QRectF(10, 10, 50, 50);

    ImageOperationResult result;
    EXPECT_NO_THROW({
        result = manager_->insertImage(page.get(), doc.get(), opts);
    });
    EXPECT_FALSE(result.success)
        << "insertImage on a non-PDF document must report failure, not success";
    EXPECT_FALSE(result.errorMessage.empty())
        << "a graceful failure must carry a user-facing errorMessage";
}

// (b) The same exception-boundary contract stated directly: insertImage into a
// faulting (non-PDF) target must NOT propagate any exception out of the call,
// and must leave a failure result. Re-running the insert must stay just as
// safe (the boundary is not a one-shot).
TEST_F(ImageInsertCrashGuardTest, InsertImage_WhenMuPdfFaults_ReturnsGracefulFailure) {
    auto doc = openNonPdfDocument();
    ASSERT_NE(doc, nullptr) << "SVG non-PDF fixture failed to open: " << svgPath_;
    ASSERT_FALSE(doc->isPdf());
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    InsertImageOptions opts;
    opts.filePath = insertPngPath_;
    opts.targetRect = QRectF(0, 0, 25, 25);

    ImageOperationResult first;
    ImageOperationResult second;
    EXPECT_NO_THROW({
        first = manager_->insertImage(page.get(), doc.get(), opts);
        second = manager_->insertImage(page.get(), doc.get(), opts);
    });
    EXPECT_FALSE(first.success);
    EXPECT_FALSE(second.success);
}
