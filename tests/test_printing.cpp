#include <gtest/gtest.h>
#include "core/Document.h"
#include "core/Page.h"
#include "core/PrintEngine.h"
#include "core/MuPdfContext.h"
#include "core/Error.h"

#include <QImage>
#include <QPrinter>
#include <QPainter>
#include <QRectF>
#include <QTemporaryFile>
#include <filesystem>
#include <memory>
#include <system_error>

using namespace pdfe::core;

class PrintTest : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<MuPdfContext>();

        std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
        testPdf = fixturesDir / "test_3page.pdf";

        if (std::filesystem::exists(testPdf)) {
            document = Document::open(context, testPdf);
        }
    }

    std::shared_ptr<MuPdfContext> context;
    std::filesystem::path testPdf;
    std::unique_ptr<Document> document;
};

TEST_F(PrintTest, PrintOptionsDefaults) {
    PrintOptions options;

    EXPECT_EQ(options.range, PrintRange::AllPages);
    EXPECT_EQ(options.fromPage, 1);
    EXPECT_EQ(options.toPage, 1);
    EXPECT_EQ(options.copies, 1);
    EXPECT_TRUE(options.fitToPage);
    EXPECT_TRUE(options.centerOnPage);
    EXPECT_TRUE(options.autoRotate);
}

// WIN-29 (F12): this test used to end in SUCCEED() with the comment "Just
// verify construction doesn't crash" — it constrained nothing and inflated the
// print-suite count. It now asserts that a constructed engine is actually BOUND
// to the document it was given: every in-range page renders through it, and the
// two out-of-range boundaries are refused.
//
// RED / mutation: drop the `pageIndex < 0 || pageIndex >= pageCount()` guard in
// PrintEngine::Impl::printPageImpl and the boundary expectations fail; break the
// document binding and the in-range renders fail.
TEST_F(PrintTest, PrintEngineConstruction) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    ASSERT_GT(document->pageCount(), 0);

    PrintEngine engine(document.get());

    QImage canvas(400, 500, QImage::Format_RGBA8888);
    canvas.fill(Qt::white);
    QPainter painter(&canvas);
    PrintOptions options;
    const QRectF target(0, 0, 400, 500);

    for (int i = 0; i < document->pageCount(); ++i) {
        EXPECT_TRUE(engine.printPage(&painter, i, target, options))
            << "a constructed engine must render every in-range page of the "
               "document it was bound to; page " << i << " failed";
    }
    EXPECT_FALSE(engine.printPage(&painter, -1, target, options))
        << "a negative page index must be refused, not rendered";
    EXPECT_FALSE(engine.printPage(&painter, document->pageCount(), target, options))
        << "one past the last page must be refused, not rendered";
    painter.end();

    // Something was actually drawn — a silently no-op engine would leave the
    // canvas untouched and still return true.
    bool anyNonWhite = false;
    for (int y = 0; y < canvas.height() && !anyNonWhite; ++y) {
        for (int x = 0; x < canvas.width(); ++x) {
            const QRgb px = canvas.pixel(x, y);
            if (qRed(px) < 250 || qGreen(px) < 250 || qBlue(px) < 250) {
                anyNonWhite = true;
                break;
            }
        }
    }
    EXPECT_TRUE(anyNonWhite)
        << "printPage returned true but painted nothing — the pre-WIN-29 oracle "
           "could not tell that apart from a real render";
}

TEST_F(PrintTest, PrintToFile) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    // Create a temporary file for output
    QTemporaryFile tempFile;
    tempFile.setAutoRemove(true);
    ASSERT_TRUE(tempFile.open());
    QString outputPath = tempFile.fileName();
    tempFile.close();

    // Setup printer to print to PDF file
    QPrinter printer;
    printer.setResolution(300);
    printer.setOutputFormat(QPrinter::PdfFormat);
    printer.setOutputFileName(outputPath);

    // Setup print options
    PrintOptions options;
    options.range = PrintRange::AllPages;
    options.copies = 1;
    options.fitToPage = true;
    options.centerOnPage = true;

    // Print
    PrintEngine engine(document.get());
    bool result = engine.print(&printer, options);

    EXPECT_TRUE(result);

    // Verify file was created and is non-empty
    std::filesystem::path outputFile(outputPath.toStdString());
    EXPECT_TRUE(std::filesystem::exists(outputFile));

    if (std::filesystem::exists(outputFile)) {
        auto fileSize = std::filesystem::file_size(outputFile);
        EXPECT_GT(fileSize, 0);
    }

    // ── WIN-29 (F12) ────────────────────────────────────────────────────
    // `file_size > 0` passes on a blank page and on a 1-page output when 3
    // were requested. Re-open the printed artifact and constrain what is
    // actually in it: the right number of pages, and ink on the first one.
    //
    // RED / mutation: make PrintEngine::print stop after the first page (skip
    // the newPage() loop) → the page-count assertion fires. Make printPageImpl
    // return true without drawImage → the non-blank assertion fires. Neither
    // was detectable before.
    ASSERT_TRUE(std::filesystem::exists(outputFile));
    const std::filesystem::path reopenPath =
        std::filesystem::temp_directory_path() / "pdfe_win29_printed_output.pdf";
    std::error_code copyEc;
    std::filesystem::copy_file(outputFile, reopenPath,
                               std::filesystem::copy_options::overwrite_existing,
                               copyEc);
    ASSERT_FALSE(static_cast<bool>(copyEc))
        << "could not stage the printed artifact for re-open: " << copyEc.message();

    {
        auto verifyCtx = std::make_shared<MuPdfContext>();
        auto printed = Document::open(verifyCtx, reopenPath);
        ASSERT_NE(printed, nullptr)
            << "the printed output must itself be a readable PDF";
        EXPECT_EQ(printed->pageCount(), document->pageCount())
            << "an AllPages print must emit exactly as many pages as the source "
               "document (" << document->pageCount() << ")";

        ASSERT_GT(printed->pageCount(), 0);
        auto p0 = printed->page(0);
        ASSERT_NE(p0, nullptr);
        QImage raster = p0->render(p0->boundingBox(), 1.0f);
        ASSERT_FALSE(raster.isNull()) << "printed page 0 must rasterize";

        long long nonWhite = 0;
        for (int y = 0; y < raster.height(); ++y) {
            for (int x = 0; x < raster.width(); ++x) {
                const QRgb px = raster.pixel(x, y);
                if (qRed(px) < 250 || qGreen(px) < 250 || qBlue(px) < 250)
                    ++nonWhite;
            }
        }
        const long long total =
            static_cast<long long>(raster.width()) * raster.height();
        ASSERT_GT(total, 0);
        EXPECT_GT(nonWhite, 0)
            << "the printed page must not be blank — " << total
            << " pixels rasterized, none of them ink. `file_size > 0` (the "
               "pre-WIN-29 oracle) passes on exactly this failure.";
    }
    std::error_code rmEc;
    std::filesystem::remove(reopenPath, rmEc);
}

TEST_F(PrintTest, PrintAllPages) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    QTemporaryFile tempFile;
    tempFile.setAutoRemove(true);
    ASSERT_TRUE(tempFile.open());
    QString outputPath = tempFile.fileName();
    tempFile.close();

    QPrinter printer;
    printer.setResolution(300);
    printer.setOutputFormat(QPrinter::PdfFormat);
    printer.setOutputFileName(outputPath);

    PrintOptions options;
    options.range = PrintRange::AllPages;
    options.fromPage = 1;
    options.toPage = document->pageCount();

    PrintEngine engine(document.get());
    bool result = engine.print(&printer, options);

    EXPECT_TRUE(result);
}

TEST_F(PrintTest, PrintCurrentPage) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    QTemporaryFile tempFile;
    tempFile.setAutoRemove(true);
    ASSERT_TRUE(tempFile.open());
    QString outputPath = tempFile.fileName();
    tempFile.close();

    QPrinter printer;
    printer.setResolution(300);
    printer.setOutputFormat(QPrinter::PdfFormat);
    printer.setOutputFileName(outputPath);

    // Print only page 2 (1-based)
    PrintOptions options;
    options.range = PrintRange::CurrentPage;
    options.fromPage = 2;
    options.toPage = 2;

    PrintEngine engine(document.get());
    bool result = engine.print(&printer, options);

    EXPECT_TRUE(result);

    // Verify file exists
    std::filesystem::path outputFile(outputPath.toStdString());
    EXPECT_TRUE(std::filesystem::exists(outputFile));
}

TEST_F(PrintTest, PrintPageRange) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    if (document->pageCount() < 2) {
        GTEST_SKIP() << "Document has less than 2 pages";
    }

    QTemporaryFile tempFile;
    tempFile.setAutoRemove(true);
    ASSERT_TRUE(tempFile.open());
    QString outputPath = tempFile.fileName();
    tempFile.close();

    QPrinter printer;
    printer.setResolution(300);
    printer.setOutputFormat(QPrinter::PdfFormat);
    printer.setOutputFileName(outputPath);

    // Print pages 1-2
    PrintOptions options;
    options.range = PrintRange::PageRange;
    options.fromPage = 1;
    options.toPage = 2;

    PrintEngine engine(document.get());
    bool result = engine.print(&printer, options);

    EXPECT_TRUE(result);

    // Verify file exists
    std::filesystem::path outputFile(outputPath.toStdString());
    EXPECT_TRUE(std::filesystem::exists(outputFile));
}

TEST_F(PrintTest, FitToPage) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    QTemporaryFile tempFile;
    tempFile.setAutoRemove(true);
    ASSERT_TRUE(tempFile.open());
    QString outputPath = tempFile.fileName();
    tempFile.close();

    QPrinter printer;
    printer.setResolution(300);
    printer.setOutputFormat(QPrinter::PdfFormat);
    printer.setOutputFileName(outputPath);

    PrintOptions options;
    options.range = PrintRange::CurrentPage;
    options.fromPage = 1;
    options.toPage = 1;
    options.fitToPage = true;
    options.centerOnPage = false;
    options.autoRotate = false;

    PrintEngine engine(document.get());
    bool result = engine.print(&printer, options);

    EXPECT_TRUE(result);
}

TEST_F(PrintTest, MultipleCopies) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    QTemporaryFile tempFile;
    tempFile.setAutoRemove(true);
    ASSERT_TRUE(tempFile.open());
    QString outputPath = tempFile.fileName();
    tempFile.close();

    QPrinter printer;
    printer.setResolution(300);
    printer.setOutputFormat(QPrinter::PdfFormat);
    printer.setOutputFileName(outputPath);

    PrintOptions options;
    options.range = PrintRange::CurrentPage;
    options.fromPage = 1;
    options.toPage = 1;
    options.copies = 2;

    PrintEngine engine(document.get());
    bool result = engine.print(&printer, options);

    EXPECT_TRUE(result);
}

TEST_F(PrintTest, PrintPageDirectly) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    // Create a QImage to paint on
    QImage image(800, 600, QImage::Format_RGBA8888);
    image.fill(Qt::white);

    QPainter painter(&image);

    PrintOptions options;
    options.fitToPage = true;
    options.centerOnPage = true;

    QRectF targetRect(0, 0, 800, 600);

    PrintEngine engine(document.get());
    bool result = engine.printPage(&painter, 0, targetRect, options);

    painter.end();

    EXPECT_TRUE(result);
    EXPECT_FALSE(image.isNull());
}

TEST_F(PrintTest, InvalidPageIndex) {
    if (!document) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    QImage image(800, 600, QImage::Format_RGBA8888);
    QPainter painter(&image);

    PrintOptions options;
    QRectF targetRect(0, 0, 800, 600);

    PrintEngine engine(document.get());

    // Try to print a page that doesn't exist
    bool result = engine.printPage(&painter, 999, targetRect, options);

    painter.end();

    EXPECT_FALSE(result);
}
