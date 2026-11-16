// Repro + diagnosis harness for GitHub #392 (tiny TRANSPARENT signature PNG).
//
// The user's crash came from inserting a small RGBA (transparent) signature
// PNG, NOT a large image. This file drives ImageManager::insertImage with the
// real image and, in a second test, replicates the MuPDF path inline with
// fprintf checkpoints so the faulting call (if any) is unambiguous.
//
// SECURITY: the real signature image is SENSITIVE. Its path is read from the
// PDFE_SIG_PNG environment variable; nothing is committed with a user path.
// Tests GTEST_SKIP when it is unset / missing.
//
//   PDFE_SIG_PNG="C:/path/sig.png" \
//     build/vs2022/bin/Debug/pdfe_tests.exe --gtest_filter='*RgbaInsertRepro*'
//
// Optional large PDF via PDFE_LARGE_PDF (same as the scale repro).
// We do NOT modify src/.

#include <gtest/gtest.h>
#include "image/ImageManager.h"
#include "image/ImageData.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <QImage>
#include <QRectF>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

using namespace pdfe::core;
using namespace pdfe::image;

namespace {

std::string sigPngPath() {
    const char* p = std::getenv("PDFE_SIG_PNG");
    if (!p || !*p) return {};
    return std::string(p);
}

std::string largePdfPath() {
    const char* p = std::getenv("PDFE_LARGE_PDF");
    if (!p || !*p) return {};
    return std::string(p);
}

std::string smallPdf() {
    return std::string(TEST_FIXTURES_DIR) + "/test_single.pdf";
}

} // namespace

class RgbaInsertReproTest : public ::testing::Test {
protected:
    void SetUp() override {
        sigPath_ = sigPngPath();
        if (sigPath_.empty())
            GTEST_SKIP() << "PDFE_SIG_PNG not set; skipping RGBA signature repro.";
        if (!std::filesystem::exists(sigPath_))
            GTEST_SKIP() << "PDFE_SIG_PNG missing: " << sigPath_;
        context_ = std::make_shared<MuPdfContext>();
        manager_ = std::make_unique<ImageManager>(context_);
    }

    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<ImageManager> manager_;
    std::string sigPath_;
};

// (1) How QImage loads the signature PNG.
TEST_F(RgbaInsertReproTest, ReportQImageFormat) {
    QImage q(QString::fromStdString(sigPath_));
    std::fprintf(stderr,
        "[rgba] QImage: null=%d  w=%d  h=%d  format=%d  hasAlpha=%d  depth=%d\n",
        q.isNull() ? 1 : 0, q.width(), q.height(),
        static_cast<int>(q.format()), q.hasAlphaChannel() ? 1 : 0, q.depth());
    std::fflush(stderr);
    ASSERT_FALSE(q.isNull());
    SUCCEED();
}

// (2) Real insertImage on the SMALL PDF. Report success / graceful-fail / (a
// crash would be a nonzero process exit before this returns).
TEST_F(RgbaInsertReproTest, InsertRealSignatureIntoSmallPdf) {
    const std::string pdf = smallPdf();
    ASSERT_TRUE(std::filesystem::exists(pdf)) << pdf;

    std::fprintf(stderr, "[rgba] SMALL: opening %s\n", pdf.c_str());
    std::fflush(stderr);
    auto doc = Document::open(context_, pdf);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    InsertImageOptions opts;
    opts.filePath = sigPath_;
    opts.targetRect = QRectF(72, 72, 172, 42);

    std::fprintf(stderr, "[rgba] SMALL: calling insertImage (real path)...\n");
    std::fflush(stderr);
    ImageOperationResult result;
    EXPECT_NO_THROW({ result = manager_->insertImage(page.get(), doc.get(), opts); });
    std::fprintf(stderr, "[rgba] SMALL: insertImage returned success=%d msg='%s'\n",
        result.success ? 1 : 0, result.errorMessage.c_str());
    std::fflush(stderr);
    // Report-only: we want to SEE the outcome, not force a verdict.
    SUCCEED();
}

// (2b) Real insertImage on the LARGE PDF, if provided.
TEST_F(RgbaInsertReproTest, InsertRealSignatureIntoLargePdf) {
    const std::string pdf = largePdfPath();
    if (pdf.empty() || !std::filesystem::exists(pdf))
        GTEST_SKIP() << "PDFE_LARGE_PDF not set/missing.";

    std::fprintf(stderr, "[rgba] LARGE: opening %s\n", pdf.c_str());
    std::fflush(stderr);
    auto doc = Document::open(context_, pdf);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    InsertImageOptions opts;
    opts.filePath = sigPath_;
    opts.targetRect = QRectF(72, 72, 172, 42);

    std::fprintf(stderr, "[rgba] LARGE: calling insertImage (real path)...\n");
    std::fflush(stderr);
    ImageOperationResult result;
    EXPECT_NO_THROW({ result = manager_->insertImage(page.get(), doc.get(), opts); });
    std::fprintf(stderr, "[rgba] LARGE: insertImage returned success=%d msg='%s'\n",
        result.success ? 1 : 0, result.errorMessage.c_str());
    std::fflush(stderr);
    SUCCEED();
}

// (2c) Full UI flow on the LARGE PDF: find the largest page, insert the real
// RGBA signature, then RENDER the page (the post-insert canvas refresh). This is
// where a soft-mask / large-pixmap fault would surface if the insert itself did
// not crash.
TEST_F(RgbaInsertReproTest, InsertSigThenRenderLargestPage) {
    const std::string pdf = largePdfPath();
    if (pdf.empty() || !std::filesystem::exists(pdf))
        GTEST_SKIP() << "PDFE_LARGE_PDF not set/missing.";

    auto doc = Document::open(context_, pdf);
    ASSERT_NE(doc, nullptr);
    int pages = doc->pageCount();
    int biggest = 0; double area = 0, bw = 0, bh = 0;
    for (int i = 0; i < pages; ++i) {
        auto pg = doc->page(i);
        if (!pg) continue;
        QRectF bb = pg->boundingBox();
        if (bb.width() * bb.height() > area) {
            area = bb.width() * bb.height(); biggest = i;
            bw = bb.width(); bh = bb.height();
        }
    }
    std::fprintf(stderr, "[rgba] FLOW: largest page=%d %.1fx%.1f pt\n", biggest, bw, bh);
    std::fflush(stderr);
    auto page = doc->page(biggest);
    ASSERT_NE(page, nullptr);

    InsertImageOptions opts;
    opts.filePath = sigPath_;
    opts.targetRect = QRectF(100, 100, 172, 42);
    std::fprintf(stderr, "[rgba] FLOW: insert sig into page %d...\n", biggest);
    std::fflush(stderr);
    auto result = manager_->insertImage(page.get(), doc.get(), opts);
    std::fprintf(stderr, "[rgba] FLOW: insert success=%d msg='%s'\n",
        result.success ? 1 : 0, result.errorMessage.c_str());
    std::fflush(stderr);

    for (float scale : {1.0f, 2.0f}) {
        std::fprintf(stderr, "[rgba] FLOW: render page %d scale=%.1f...\n", biggest, scale);
        std::fflush(stderr);
        QImage img = page->render(page->boundingBox(), scale);
        std::fprintf(stderr, "[rgba] FLOW: render -> %dx%d null=%d\n",
            img.width(), img.height(), img.isNull() ? 1 : 0);
        std::fflush(stderr);
    }

    // Save after insert (the real signature workflow writes the file out). This
    // is where the alpha image + its SMask get flate-encoded by MuPDF/QPDF.
    namespace fs = std::filesystem;
    std::string out = (fs::temp_directory_path() / "pdfe_392_sig_out.pdf").string();
    std::fprintf(stderr, "[rgba] FLOW: saveAs %s ...\n", out.c_str());
    std::fflush(stderr);
    bool threw = false;
    try {
        doc->saveAs(out);
    } catch (const std::exception& e) {
        threw = true;
        std::fprintf(stderr, "[rgba] FLOW: saveAs threw: %s\n", e.what());
        std::fflush(stderr);
    }
    std::error_code ec; fs::remove(out, ec);
    std::fprintf(stderr, "[rgba] FLOW: saveAs done (threw=%d)\n", threw ? 1 : 0);
    std::fflush(stderr);
    SUCCEED();
}

// (3) Inline replica of the insertImage MuPDF path with a checkpoint before and
// after EVERY MuPDF call, so a hard crash pinpoints the faulting call; an
// fz_throw is caught and reported (proving it would be caught by the boundary).
TEST_F(RgbaInsertReproTest, BisectMuPdfPathWithCheckpoints) {
    const std::string pdf = smallPdf();
    ASSERT_TRUE(std::filesystem::exists(pdf)) << pdf;

    QImage qimg(QString::fromStdString(sigPath_));
    ASSERT_FALSE(qimg.isNull());
    std::fprintf(stderr, "[rgba] BISECT: loaded format=%d hasAlpha=%d %dx%d\n",
        static_cast<int>(qimg.format()), qimg.hasAlphaChannel() ? 1 : 0,
        qimg.width(), qimg.height());
    std::fflush(stderr);

    // Mirror insertImage's convert rule exactly.
    if (qimg.format() != QImage::Format_RGB32 &&
        qimg.format() != QImage::Format_ARGB32) {
        qimg = qimg.convertToFormat(QImage::Format_RGB32);
        std::fprintf(stderr, "[rgba] BISECT: converted to RGB32 (now format=%d)\n",
            static_cast<int>(qimg.format()));
    } else {
        std::fprintf(stderr, "[rgba] BISECT: NO convert (kept format=%d)\n",
            static_cast<int>(qimg.format()));
    }
    std::fflush(stderr);

    const std::string pdfPathCopy = pdf;
    context_->execute([&](fz_context* ctx) {
        // Open the PDF directly via MuPDF so we do not need Document's private
        // handle accessors. Same pdf_document / pdf_page the real path uses.
        ::pdf_document* pdfDoc = pdf_open_document(ctx, pdfPathCopy.c_str());
        ::pdf_page* pdfPage = pdf_load_page(ctx, pdfDoc, 0);
        std::fprintf(stderr, "[rgba] BISECT: pdfDoc=%p pdfPage=%p\n",
            (void*)pdfDoc, (void*)pdfPage);
        std::fflush(stderr);

        int width = qimg.width();
        int height = qimg.height();
        bool hasAlpha = qimg.hasAlphaChannel();
        int n = hasAlpha ? 4 : 3;
        std::fprintf(stderr, "[rgba] BISECT: n=%d hasAlpha=%d\n", n, hasAlpha);
        std::fflush(stderr);

        fz_colorspace* cs = fz_device_rgb(ctx);

        std::fprintf(stderr, "[rgba] BISECT: >> fz_new_pixmap(alpha=%d)\n", hasAlpha ? 1 : 0);
        std::fflush(stderr);
        fz_pixmap* pix = fz_new_pixmap(ctx, cs, width, height, nullptr, hasAlpha ? 1 : 0);
        std::fprintf(stderr, "[rgba] BISECT: << fz_new_pixmap pix=%p n=%d alpha=%d s=%d stride=%td\n",
            (void*)pix, pix->n, pix->alpha, pix->s, (ptrdiff_t)pix->stride);
        std::fflush(stderr);
        pix->xres = 72;
        pix->yres = 72;

        std::fprintf(stderr, "[rgba] BISECT: >> pixel copy loop\n");
        std::fflush(stderr);
        unsigned char* samples = pix->samples;
        for (int y = 0; y < height; ++y) {
            const uchar* scanline = qimg.constScanLine(y);
            for (int x = 0; x < width; ++x) {
                uint32_t pixel;
                std::memcpy(&pixel, scanline + x * 4, 4);
                uint8_t b = pixel & 0xFF;
                uint8_t g = (pixel >> 8) & 0xFF;
                uint8_t r = (pixel >> 16) & 0xFF;
                uint8_t a = (pixel >> 24) & 0xFF;
                const size_t idx =
                    (static_cast<size_t>(y) * static_cast<size_t>(width)
                     + static_cast<size_t>(x)) * static_cast<size_t>(n);
                samples[idx] = r;
                samples[idx + 1] = g;
                samples[idx + 2] = b;
                if (hasAlpha) samples[idx + 3] = a;
            }
        }
        std::fprintf(stderr, "[rgba] BISECT: << pixel copy done\n");
        std::fflush(stderr);

        std::fprintf(stderr, "[rgba] BISECT: >> fz_new_image_from_pixmap\n");
        std::fflush(stderr);
        fz_image* fzImage = fz_new_image_from_pixmap(ctx, pix, nullptr);
        std::fprintf(stderr, "[rgba] BISECT: << fz_new_image_from_pixmap img=%p\n", (void*)fzImage);
        std::fflush(stderr);

        std::fprintf(stderr, "[rgba] BISECT: >> pdf_add_image\n");
        std::fflush(stderr);
        pdf_obj* imageObj = pdf_add_image(ctx, pdfDoc, fzImage);
        std::fprintf(stderr, "[rgba] BISECT: << pdf_add_image obj=%p\n", (void*)imageObj);
        std::fflush(stderr);

        fz_drop_image(ctx, fzImage);
        fz_drop_pixmap(ctx, pix);
        fz_drop_page(ctx, (fz_page*)pdfPage);
        pdf_drop_document(ctx, pdfDoc);
        std::fprintf(stderr, "[rgba] BISECT: dropped image+pixmap; path OK\n");
        std::fflush(stderr);
    });

    std::fprintf(stderr, "[rgba] BISECT: completed without crash\n");
    std::fflush(stderr);
    SUCCEED();
}
