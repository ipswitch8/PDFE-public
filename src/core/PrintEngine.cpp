#include "PrintEngine.h"
#include "Document.h"
#include "Page.h"
#include "PageInfo.h"
#include "PrintDeviceSeam.h"

#include <QCoreApplication>
#include <QPrinter>
#include <QPainter>
#include <QImage>
#include <cmath>
#include <algorithm>

namespace pdfe::core {

class PrintEngine::Impl {
public:
    explicit Impl(Document* doc) : document_(doc) {}

    PrintError lastError() const { return lastError_; }
    QString lastErrorMessage() const { return lastErrorMessage_; }

    bool print(QPrinter* printer, const PrintOptions& options) {
        clearError();

        if (!printer || !document_) {
            return fail(PrintError::NoDocument,
                        QCoreApplication::translate(
                            "pdfe::core::PrintEngine",
                            "There is no open document to print. Open a PDF "
                            "and try again."));
        }

        // R2-09: enforce the live print permission at the rendering boundary.
        // This backstops the UI gate in MainWindow::onPrint so that ANY path
        // reaching the engine — including a Named /Print link action or a
        // programmatic/batch print — refuses a document whose owner denied
        // printing. Unencrypted documents allow printing (printAllowed()==true).
        if (!document_->printAllowed()) {
            return fail(PrintError::PrintNotPermitted,
                        QCoreApplication::translate(
                            "pdfe::core::PrintEngine",
                            "This document's security settings do not allow "
                            "printing. Ask the document's author for a version "
                            "with printing permitted."));
        }

        // ------------------------------------------------------------------
        // GitHub #78 / WIN-28 — the stopped-spooler / no-printers failure path.
        //
        // With the Windows Print Spooler service stopped, winspool's
        // EnumPrinters returns nothing, so QPrinterInfo::availablePrinters() is
        // EMPTY and the native back-end has no device to bind. Without this
        // guard the attempt fell through to painter.begin(), which fails deep
        // inside QWin32PrintEngine and produced a bare `false` — the UI then
        // said only "Failed to print the document.", which tells a user nothing
        // they can act on. Detect the condition where it is legible and say
        // what to do about it.
        //
        // Scoped to the NATIVE back-end on purpose: a PdfFormat QPrinter writes
        // a file through QPdfEngine and needs no spooler and no installed
        // device, so refusing it when the printer list is empty would break
        // Print-to-PDF export on exactly the machines this guard is for.
        if (printer->outputFormat() == QPrinter::NativeFormat
            && testseam::availablePrinterCount() == 0) {
            return fail(PrintError::NoPrinterAvailable,
                        QCoreApplication::translate(
                            "pdfe::core::PrintEngine",
                            "No printer is available, so the document cannot be "
                            "printed. Windows reported that no print devices are "
                            "installed. Check that a printer is installed and "
                            "that the Windows Print Spooler service is running, "
                            "then try again."));
        }

        // Determine page range to print
        int startPage = 0;
        int endPage = document_->pageCount() - 1;

        switch (options.range) {
            case PrintRange::AllPages:
                startPage = 0;
                endPage = document_->pageCount() - 1;
                break;

            case PrintRange::CurrentPage:
                startPage = options.fromPage - 1; // Convert to 0-based
                endPage = options.fromPage - 1;
                break;

            case PrintRange::PageRange:
                startPage = options.fromPage - 1; // Convert to 0-based
                endPage = options.toPage - 1;
                // Validate range
                startPage = std::max(0, std::min(startPage, document_->pageCount() - 1));
                endPage = std::max(0, std::min(endPage, document_->pageCount() - 1));
                if (startPage > endPage) {
                    return fail(PrintError::InvalidPageRange,
                                QCoreApplication::translate(
                                    "pdfe::core::PrintEngine",
                                    "The selected page range is empty. Choose a "
                                    "first page that is not after the last page, "
                                    "then try again."));
                }
                break;
        }

        QPainter painter;
        if (!painter.begin(printer)) {
            // The device was enumerable but would not open: the spooler stopped
            // between enumeration and StartDoc, the driver rejected the job, or
            // the output file could not be written.
            return fail(PrintError::DeviceOpenFailed,
                        QCoreApplication::translate(
                            "pdfe::core::PrintEngine",
                            "The print job could not be started because the "
                            "printer did not accept it. Check that the printer "
                            "is switched on and available and that the Windows "
                            "Print Spooler service is running, then try again."));
        }

        bool firstPage = true;

        // Print each copy
        for (int copy = 0; copy < options.copies; ++copy) {
            for (int pageIndex = startPage; pageIndex <= endPage; ++pageIndex) {
                // Start a new page for all pages except the first
                if (!firstPage) {
                    if (!printer->newPage()) {
                        painter.end();
                        return fail(PrintError::PageEmitFailed,
                                    QCoreApplication::translate(
                                        "pdfe::core::PrintEngine",
                                        "The printer stopped accepting pages "
                                        "part-way through the job. Check that "
                                        "the printer is still available and that "
                                        "the Windows Print Spooler service is "
                                        "running, then print again."));
                    }
                } else {
                    firstPage = false;
                }

                // Get printer's page rectangle
                QRectF printerPageRect = printer->pageRect(QPrinter::DevicePixel);

                // Print the page
                if (!printPageImpl(&painter, pageIndex, printerPageRect, options)) {
                    painter.end();
                    return fail(PrintError::PageEmitFailed,
                                QCoreApplication::translate(
                                    "pdfe::core::PrintEngine",
                                    "One of the pages could not be prepared for "
                                    "printing, so the job was stopped. The "
                                    "document may be damaged; try printing a "
                                    "smaller page range."));
                }
            }
        }

        painter.end();
        return true;
    }

    bool printPage(QPainter* painter, int pageIndex, const QRectF& targetRect, const PrintOptions& options) {
        clearError();
        if (!painter || !document_) {
            return fail(PrintError::NoDocument,
                        QCoreApplication::translate(
                            "pdfe::core::PrintEngine",
                            "There is no open document to print. Open a PDF "
                            "and try again."));
        }

        if (!printPageImpl(painter, pageIndex, targetRect, options)) {
            return fail(PrintError::PageEmitFailed,
                        QCoreApplication::translate(
                            "pdfe::core::PrintEngine",
                            "That page could not be prepared for printing. The "
                            "document may be damaged; try printing a different "
                            "page."));
        }
        return true;
    }

private:
    void clearError()
    {
        lastError_ = PrintError::None;
        lastErrorMessage_.clear();
    }

    /// Record a failure and return false, so every failing branch is a
    /// one-liner that cannot forget to set the message.
    bool fail(PrintError code, const QString& message)
    {
        lastError_ = code;
        lastErrorMessage_ = message;
        return false;
    }

    bool printPageImpl(QPainter* painter, int pageIndex, const QRectF& targetRect, const PrintOptions& options) {
        if (pageIndex < 0 || pageIndex >= document_->pageCount()) {
            return false;
        }

        // Get page information
        PageInfo pageInfo = document_->pageInfo(pageIndex);
        auto page = document_->page(pageIndex);
        if (!page) {
            return false;
        }

        QRectF pageBBox = page->boundingBox();
        float pageWidth = pageBBox.width();
        float pageHeight = pageBBox.height();

        // Adjust for rotation
        bool isRotated = (pageInfo.rotation == 90 || pageInfo.rotation == 270);
        if (isRotated) {
            std::swap(pageWidth, pageHeight);
        }

        // Determine target rectangle dimensions
        float targetWidth = targetRect.width();
        float targetHeight = targetRect.height();

        // Auto-rotate: if page orientation doesn't match target orientation, rotate
        bool pageIsLandscape = pageWidth > pageHeight;
        bool targetIsLandscape = targetWidth > targetHeight;
        bool shouldRotate = options.autoRotate && (pageIsLandscape != targetIsLandscape);

        if (shouldRotate) {
            std::swap(pageWidth, pageHeight);
        }

        // Calculate scaling to fit page into target rectangle
        float scaleX = targetWidth / pageWidth;
        float scaleY = targetHeight / pageHeight;
        float scale = options.fitToPage ? std::min(scaleX, scaleY) : 1.0f;

        // Calculate render scale for good print quality (typically 150-300 DPI)
        // Assuming page dimensions are in points (72 DPI), calculate scale for higher DPI
        const float printDPI = 150.0f; // Moderate quality for testing
        const float pointDPI = 72.0f;
        float renderScale = (printDPI / pointDPI) * scale;

        // Render the page to an image at print resolution
        QImage pageImage = page->render(pageBBox, renderScale);
        if (pageImage.isNull()) {
            return false;
        }

        // Calculate the final scaled dimensions
        float scaledWidth = pageWidth * scale;
        float scaledHeight = pageHeight * scale;

        // Calculate position (centering if requested)
        float posX = targetRect.x();
        float posY = targetRect.y();

        if (options.centerOnPage) {
            posX += (targetWidth - scaledWidth) / 2.0f;
            posY += (targetHeight - scaledHeight) / 2.0f;
        }

        // Save painter state
        painter->save();

        // Apply rotation if needed
        if (shouldRotate) {
            // Rotate around the center of the target rectangle
            float centerX = targetRect.x() + targetWidth / 2.0f;
            float centerY = targetRect.y() + targetHeight / 2.0f;
            painter->translate(centerX, centerY);
            painter->rotate(90);
            painter->translate(-centerX, -centerY);

            // DEFECT FIX (meta-audit follow-up F17, gap A1-27): the drawn
            // rectangle's dimensions are swapped for the rotated frame, so the
            // centring arithmetic has to use the SWAPPED extents. It used
            // targetHeight against the pre-swap scaledHeight (and targetWidth
            // against scaledWidth), i.e. it paired each target extent with the
            // wrong content extent. On a landscape target the page was pushed
            // off the top of the sheet and the render came back CLIPPED --
            // measured: the auto-rotated page's ink box was 413x302 where the
            // rotated source ink is 412x393, with the box hard against y=0.
            // Rotation is about the target centre, which maps centre to
            // centre, so centring the swapped rect on the target is correct.
            std::swap(scaledWidth, scaledHeight);
            posX = targetRect.x() + (targetWidth - scaledWidth) / 2.0f;
            posY = targetRect.y() + (targetHeight - scaledHeight) / 2.0f;
        }

        // Draw the rendered page image
        QRectF drawRect(posX, posY, scaledWidth, scaledHeight);
        painter->drawImage(drawRect, pageImage);

        // Restore painter state
        painter->restore();

        return true;
    }

    Document* document_;
    PrintError lastError_ = PrintError::None;
    QString lastErrorMessage_;
};

PrintEngine::PrintEngine(Document* document)
    : pImpl(std::make_unique<Impl>(document))
{
}

PrintEngine::~PrintEngine() = default;

bool PrintEngine::print(QPrinter* printer, const PrintOptions& options) {
    return pImpl->print(printer, options);
}

bool PrintEngine::printPage(QPainter* painter, int pageIndex, const QRectF& targetRect, const PrintOptions& options) {
    return pImpl->printPage(painter, pageIndex, targetRect, options);
}

PrintError PrintEngine::lastError() const {
    return pImpl->lastError();
}

QString PrintEngine::lastErrorMessage() const {
    return pImpl->lastErrorMessage();
}

} // namespace pdfe::core
