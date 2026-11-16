// Repro harness for the "insert image into a very large PDF crashes" report.
//
// The user opens a ~266 MB engineering-spec PDF, inserts an image, and the app
// crashes immediately; the same action on a small PDF works. This file
// reproduces the full flow the UI drives (open -> insert -> re-enumerate ->
// render) in separate TEST_F cases so the faulting STEP is unambiguous.
//
// CI-SAFE: the large PDF is read from the PDFE_LARGE_PDF environment variable.
// The test GTEST_SKIPs if that is unset or the file is missing, so it never
// fails a clean CI machine. It is intended to be driven locally with:
//
//   PDFE_LARGE_PDF="<path-to-spec.pdf>" \
//       build/vs2022/bin/Debug/pdfe_tests.exe --gtest_filter='*InsertScaleRepro*'
//
// We model usage on tests/test_image_manager.cpp. We do NOT modify src/.

#include <gtest/gtest.h>
#include "image/ImageManager.h"
#include "image/ImageData.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"
#include "rendering/TileRenderer.h"
#include "rendering/TileId.h"
#include <mupdf/fitz.h>
#include <QImage>
#include <QRectF>
#include <QCoreApplication>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

using namespace pdfe::core;
using namespace pdfe::image;

namespace {

// Resolve the large-PDF path from the environment. Empty string => skip.
std::string largePdfPath() {
    const char* p = std::getenv("PDFE_LARGE_PDF");
    if (!p || !*p) return {};
    return std::string(p);
}

std::string fixturePng() {
    return std::string(TEST_FIXTURES_DIR) + "/test_image_rgb.png";
}

} // namespace

class InsertScaleReproTest : public ::testing::Test {
protected:
    void SetUp() override {
        pdfPath_ = largePdfPath();
        if (pdfPath_.empty()) {
            GTEST_SKIP() << "PDFE_LARGE_PDF not set; skipping large-PDF repro.";
        }
        if (!std::filesystem::exists(pdfPath_)) {
            GTEST_SKIP() << "PDFE_LARGE_PDF points to a missing file: " << pdfPath_;
        }
        context_ = std::make_shared<MuPdfContext>();
        manager_ = std::make_unique<ImageManager>(context_);
    }

    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<ImageManager> manager_;
    std::string pdfPath_;
};

// STEP 1 --------------------------------------------------------------------
// Open the large PDF and report page count + page(0) MediaBox dimensions.
TEST_F(InsertScaleReproTest, Step1_OpenAndReportGeometry) {
    std::fprintf(stderr, "[repro] STEP1: opening large PDF...\n");
    std::fflush(stderr);

    auto doc = Document::open(context_, pdfPath_);
    ASSERT_NE(doc, nullptr) << "Document::open returned null";

    int pages = doc->pageCount();
    std::fprintf(stderr, "[repro] STEP1: pageCount=%d\n", pages);
    std::fflush(stderr);
    ASSERT_GT(pages, 0);

    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    QRectF bb = page->boundingBox();
    std::fprintf(stderr,
        "[repro] STEP1: page(0) MediaBox = %.2f x %.2f pt (x0=%.2f y0=%.2f)\n",
        bb.width(), bb.height(), bb.left(), bb.top());
    std::fflush(stderr);

    // Report the pixmap size a 150 DPI render would imply for this page.
    const double scale = 150.0 / 72.0;
    const double pxW = bb.width() * scale;
    const double pxH = bb.height() * scale;
    const double bytes = pxW * pxH * 4.0;
    std::fprintf(stderr,
        "[repro] STEP1: @150DPI pixmap ~= %.0f x %.0f px, %.0f bytes (%.2f GB); "
        "INT_MAX=%d, overflow=%s\n",
        pxW, pxH, bytes, bytes / (1024.0 * 1024.0 * 1024.0),
        INT32_MAX, (bytes > 2147483647.0 ? "YES" : "no"));
    std::fflush(stderr);

    SUCCEED();
}

// STEP 2 --------------------------------------------------------------------
// Model the MainWindow::onInsertImage flow: insertImage then re-enumerate
// (ImageTool::refreshImages runs enumerateImages again after the insert).
TEST_F(InsertScaleReproTest, Step2_InsertThenReEnumerate) {
    const std::string png = fixturePng();
    if (!std::filesystem::exists(png)) {
        GTEST_SKIP() << "Fixture PNG missing: " << png;
    }

    std::fprintf(stderr, "[repro] STEP2: opening...\n");
    std::fflush(stderr);
    auto doc = Document::open(context_, pdfPath_);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    InsertImageOptions opts;
    opts.filePath = png;
    opts.targetRect = QRectF(100, 100, 200, 160);

    std::fprintf(stderr, "[repro] STEP2: calling insertImage...\n");
    std::fflush(stderr);
    auto result = manager_->insertImage(page.get(), doc.get(), opts);
    std::fprintf(stderr, "[repro] STEP2: insertImage returned success=%d msg='%s'\n",
        result.success ? 1 : 0, result.errorMessage.c_str());
    std::fflush(stderr);
    EXPECT_TRUE(result.success) << result.errorMessage;

    std::fprintf(stderr, "[repro] STEP2: re-enumerating images (refreshImages)...\n");
    std::fflush(stderr);
    auto images = manager_->enumerateImages(page.get(), doc.get());
    std::fprintf(stderr, "[repro] STEP2: re-enumerate found %zu images\n", images.size());
    std::fflush(stderr);

    SUCCEED();
}

// STEP 3 --------------------------------------------------------------------
// The canvas renders the page to a pixmap after the insert. For a large-format
// MediaBox this allocates a huge pixmap -- a strong scale-dependent crash
// candidate. Use the real app render API (Page::render).
TEST_F(InsertScaleReproTest, Step3_RenderPageToPixmap) {
    std::fprintf(stderr, "[repro] STEP3: opening...\n");
    std::fflush(stderr);
    auto doc = Document::open(context_, pdfPath_);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    QRectF bb = page->boundingBox();
    std::fprintf(stderr, "[repro] STEP3: MediaBox %.1f x %.1f pt\n",
        bb.width(), bb.height());
    std::fflush(stderr);

    // Typical on-screen render: start conservative (zoom ~1.0 == 72 DPI, the
    // render() default scale of 1.0) then push toward a normal screen DPI.
    for (float scale : {1.0f, 1.3333f /*96dpi*/, 2.0f /*~150dpi*/}) {
        const double pxW = bb.width() * static_cast<double>(scale);
        const double pxH = bb.height() * static_cast<double>(scale);
        std::fprintf(stderr,
            "[repro] STEP3: Page::render(scale=%.4f) -> ~%.0f x %.0f px (%.2f GB)...\n",
            scale, pxW, pxH, pxW * pxH * 4.0 / (1024.0 * 1024.0 * 1024.0));
        std::fflush(stderr);

        QImage img = page->render(bb, scale);

        std::fprintf(stderr, "[repro] STEP3: render(scale=%.4f) returned %dx%d null=%d\n",
            scale, img.width(), img.height(), img.isNull() ? 1 : 0);
        std::fflush(stderr);
    }

    SUCCEED();
}

// SCAN ----------------------------------------------------------------------
// The spec PDF has many pages; page 0 is standard Letter. Engineering specs
// typically have large-format sheets (E-size etc.) deeper in. Find the biggest
// page, then run insert + render on IT -- that is the scale-dependent crash
// candidate the user actually hits.
TEST_F(InsertScaleReproTest, Scan_FindLargestPageThenInsertAndRender) {
    const std::string png = fixturePng();

    std::fprintf(stderr, "[repro] SCAN: opening...\n");
    std::fflush(stderr);
    auto doc = Document::open(context_, pdfPath_);
    ASSERT_NE(doc, nullptr);
    int pages = doc->pageCount();
    std::fprintf(stderr, "[repro] SCAN: %d pages, measuring MediaBoxes...\n", pages);
    std::fflush(stderr);

    int biggest = 0;
    double biggestArea = 0.0;
    double biggestW = 0.0, biggestH = 0.0;
    for (int i = 0; i < pages; ++i) {
        auto pg = doc->page(i);
        if (!pg) continue;
        QRectF bb = pg->boundingBox();
        double area = bb.width() * bb.height();
        if (area > biggestArea) {
            biggestArea = area;
            biggest = i;
            biggestW = bb.width();
            biggestH = bb.height();
        }
    }
    std::fprintf(stderr,
        "[repro] SCAN: largest page index=%d, %.1f x %.1f pt (%.2f x %.2f in)\n",
        biggest, biggestW, biggestH, biggestW / 72.0, biggestH / 72.0);
    std::fflush(stderr);

    auto page = doc->page(biggest);
    ASSERT_NE(page, nullptr);

    // Insert into the large page.
    if (std::filesystem::exists(png)) {
        InsertImageOptions opts;
        opts.filePath = png;
        opts.targetRect = QRectF(100, 100, 200, 160);
        std::fprintf(stderr, "[repro] SCAN: inserting into page %d...\n", biggest);
        std::fflush(stderr);
        auto result = manager_->insertImage(page.get(), doc.get(), opts);
        std::fprintf(stderr, "[repro] SCAN: insert success=%d msg='%s'\n",
            result.success ? 1 : 0, result.errorMessage.c_str());
        std::fflush(stderr);

        std::fprintf(stderr, "[repro] SCAN: re-enumerate on page %d...\n", biggest);
        std::fflush(stderr);
        auto imgs = manager_->enumerateImages(page.get(), doc.get());
        std::fprintf(stderr, "[repro] SCAN: re-enumerate found %zu\n", imgs.size());
        std::fflush(stderr);
    }

    // Render the large page at increasing scale.
    for (float scale : {1.0f, 2.0f}) {
        const double pxW = biggestW * static_cast<double>(scale);
        const double pxH = biggestH * static_cast<double>(scale);
        std::fprintf(stderr,
            "[repro] SCAN: render page %d scale=%.2f -> ~%.0f x %.0f px (%.2f GB)...\n",
            biggest, scale, pxW, pxH, pxW * pxH * 4.0 / (1024.0*1024.0*1024.0));
        std::fflush(stderr);
        QImage img = page->render(page->boundingBox(), scale);
        std::fprintf(stderr, "[repro] SCAN: render returned %dx%d null=%d\n",
            img.width(), img.height(), img.isNull() ? 1 : 0);
        std::fflush(stderr);
    }

    SUCCEED();
}

// STEP 4 --------------------------------------------------------------------
// Replicate the REAL canvas pixel path the UI drives after insert:
// Document::createDisplayList(page) -> raster each 512x512 tile covering the
// page via fz_run_display_list into a tile pixmap. This is the exact code in
// src/rendering/TileRenderer.cpp (renderTileToPixmap), run single-threaded and
// deterministically here. Page::render (step 3) uses fz_new_pixmap_from_page
// instead, so this exercises the display-list path specifically.
TEST_F(InsertScaleReproTest, Step4_DisplayListTileRasterAfterInsert) {
    const std::string png = fixturePng();

    std::fprintf(stderr, "[repro] STEP4: opening...\n");
    std::fflush(stderr);
    auto doc = Document::open(context_, pdfPath_);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    if (std::filesystem::exists(png)) {
        InsertImageOptions opts;
        opts.filePath = png;
        opts.targetRect = QRectF(100, 100, 200, 160);
        std::fprintf(stderr, "[repro] STEP4: inserting...\n");
        std::fflush(stderr);
        auto result = manager_->insertImage(page.get(), doc.get(), opts);
        std::fprintf(stderr, "[repro] STEP4: insert success=%d\n", result.success ? 1 : 0);
        std::fflush(stderr);
    }

    QRectF bb = page->boundingBox();

    // Mirror canvas effectiveRenderZoom: zoomLevel_ * devicePixelRatio_.
    // Try 100% @ dpr 1, then a 200% zoom, then a HiDPI 200% @ dpr 2 (=4.0).
    for (float zoom : {1.0f, 2.0f, 4.0f}) {
        std::fprintf(stderr, "[repro] STEP4: createDisplayList(page 0) zoom=%.1f...\n", zoom);
        std::fflush(stderr);

        void* dlist = nullptr;
        try {
            dlist = doc->createDisplayList(0);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[repro] STEP4: createDisplayList threw: %s\n", e.what());
            std::fflush(stderr);
            continue;
        }
        ASSERT_NE(dlist, nullptr);

        const int tilesX = static_cast<int>((bb.width()  * zoom) / 512) + 1;
        const int tilesY = static_cast<int>((bb.height() * zoom) / 512) + 1;
        std::fprintf(stderr, "[repro] STEP4: rastering %dx%d tiles @zoom %.1f...\n",
            tilesX, tilesY, zoom);
        std::fflush(stderr);

        int rendered = 0;
        for (int row = 0; row < tilesY; ++row) {
            for (int col = 0; col < tilesX; ++col) {
                const float tileX = static_cast<float>(col * 512) / zoom;
                const float tileY = static_cast<float>(row * 512) / zoom;
                context_->execute([&](fz_context* ctx) {
                    fz_matrix ctm = fz_pre_translate(fz_scale(zoom, zoom), -tileX, -tileY);
                    fz_display_list* list = static_cast<fz_display_list*>(dlist);
                    fz_pixmap* pix = nullptr;
                    fz_device* dev = nullptr;
                    fz_var(pix);
                    fz_var(dev);
                    fz_try(ctx) {
                        fz_colorspace* cs = fz_device_rgb(ctx);
                        pix = fz_new_pixmap(ctx, cs, 512, 512, nullptr, 1);
                        fz_clear_pixmap_with_value(ctx, pix, 255);
                        dev = fz_new_draw_device(ctx, fz_identity, pix);
                        fz_run_display_list(ctx, list, dev, ctm, fz_infinite_rect, nullptr);
                        fz_close_device(ctx, dev);
                    }
                    fz_always(ctx) {
                        fz_drop_device(ctx, dev);
                        if (pix) fz_drop_pixmap(ctx, pix);
                    }
                    fz_catch(ctx) {
                        fz_rethrow(ctx);
                    }
                });
                ++rendered;
            }
        }
        std::fprintf(stderr, "[repro] STEP4: rendered %d tiles @zoom %.1f OK\n", rendered, zoom);
        std::fflush(stderr);

        doc->releaseDisplayList(dlist);
    }

    SUCCEED();
}

// STEP 5 --------------------------------------------------------------------
// Concurrency reproduction: the only "large crashes / small works" mechanism
// left after the deterministic paths proved clean. The canvas spawns worker
// threads (TileRenderer) that rasterize display lists referencing the shared
// 256MB MuPDF store; on a big scanned doc those renders are SLOW (decoding
// large embedded JPEGs), so workers are still in-flight when the user inserts.
// insertImage mutates the SAME shared store on the main thread with no lock
// shared with the workers -> data race. refreshPage() cancels but never joins.
// On a small doc tiles finish instantly, so no worker is ever in-flight: no race.
//
// This drives the REAL TileRenderer. Oracle: survive (no crash/AV). A crash
// here IS the reproduction.
TEST_F(InsertScaleReproTest, Step5_ConcurrentTileRenderDuringInsert) {
    const std::string png = fixturePng();
    if (!QCoreApplication::instance()) {
        GTEST_SKIP() << "No QCoreApplication (thread pool unavailable).";
    }

    std::fprintf(stderr, "[repro] STEP5: opening...\n");
    std::fflush(stderr);
    auto doc = Document::open(context_, pdfPath_);
    ASSERT_NE(doc, nullptr);
    const int pages = doc->pageCount();

    pdfe::rendering::TileRenderer renderer(doc.get());

    std::atomic<int> delivered{0};
    auto cb = [&delivered](pdfe::rendering::TileId, QImage) { ++delivered; };

    for (int iter = 0; iter < 8; ++iter) {
        std::fprintf(stderr, "[repro] STEP5: iter %d -- fan-out tile burst...\n", iter);
        std::fflush(stderr);

        // High fan-out across many pages at a high zoom to keep workers busy
        // decoding the big scanned images (mimics scrolling a big doc).
        const int burstPages = std::min(pages, 40);
        for (int p = 0; p < burstPages; ++p) {
            for (int col = 0; col < 3; ++col) {
                for (int row = 0; row < 4; ++row) {
                    pdfe::rendering::TileId id;
                    id.pageIndex = p;
                    id.col = col;
                    id.row = row;
                    id.zoom = 4.0f;
                    renderer.requestTile(id, cb);
                }
            }
        }

        // While those are in flight, mutate the document exactly as the UI does
        // on insert, then cancel-without-join and re-request (refreshPage).
        auto page = doc->page(0);
        if (page && std::filesystem::exists(png)) {
            InsertImageOptions opts;
            opts.filePath = png;
            opts.targetRect = QRectF(100, 100, 200, 160);
            std::fprintf(stderr, "[repro] STEP5: iter %d -- insertImage (workers live)...\n", iter);
            std::fflush(stderr);
            auto result = manager_->insertImage(page.get(), doc.get(), opts);
            (void)result;
        }

        // refreshPage() semantics: cancel this page's tiles (NO waitForDone),
        // then re-request -> createDisplayList(0) runs against the just-mutated
        // page while old workers may still touch the shared store.
        renderer.cancelPage(0);
        for (int col = 0; col < 3; ++col) {
            for (int row = 0; row < 4; ++row) {
                pdfe::rendering::TileId id;
                id.pageIndex = 0; id.col = col; id.row = row; id.zoom = 4.0f;
                renderer.requestTile(id, cb);
            }
        }

        // Pump the event loop so tileReady deliveries + any fault surface.
        for (int i = 0; i < 50; ++i) {
            QCoreApplication::processEvents();
        }
    }

    std::fprintf(stderr, "[repro] STEP5: draining workers...\n");
    std::fflush(stderr);
    renderer.waitForDone();
    for (int i = 0; i < 100; ++i) QCoreApplication::processEvents();

    std::fprintf(stderr, "[repro] STEP5: survived; tiles delivered=%d\n", delivered.load());
    std::fflush(stderr);
    SUCCEED();
}

// STEP 3b -------------------------------------------------------------------
// Same as step 3 but AFTER inserting the image, which is the exact post-insert
// canvas refresh the user triggers.
TEST_F(InsertScaleReproTest, Step3b_RenderAfterInsert) {
    const std::string png = fixturePng();
    if (!std::filesystem::exists(png)) {
        GTEST_SKIP() << "Fixture PNG missing: " << png;
    }

    std::fprintf(stderr, "[repro] STEP3b: opening...\n");
    std::fflush(stderr);
    auto doc = Document::open(context_, pdfPath_);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    InsertImageOptions opts;
    opts.filePath = png;
    opts.targetRect = QRectF(100, 100, 200, 160);

    std::fprintf(stderr, "[repro] STEP3b: inserting...\n");
    std::fflush(stderr);
    auto result = manager_->insertImage(page.get(), doc.get(), opts);
    std::fprintf(stderr, "[repro] STEP3b: insert success=%d\n", result.success ? 1 : 0);
    std::fflush(stderr);

    QRectF bb = page->boundingBox();
    std::fprintf(stderr, "[repro] STEP3b: Page::render(scale=2.0) after insert...\n");
    std::fflush(stderr);
    QImage img = page->render(bb, 2.0f);
    std::fprintf(stderr, "[repro] STEP3b: render returned %dx%d null=%d\n",
        img.width(), img.height(), img.isNull() ? 1 : 0);
    std::fflush(stderr);

    SUCCEED();
}
