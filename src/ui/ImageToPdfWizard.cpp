// -*- coding: utf-8 -*-
#include "ImageToPdfWizard.h"

#include <QFileDialog>
#include <QImage>
#include <QImageReader>
#include <QMessageBox>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QWidget>

namespace pdfe::ui {

QStringList ImageToPdfWizard::supportedImageExtensions()
{
    // Keep this curated rather than querying QImageReader at runtime
    // — the dialog filter shouldn't surprise users with obscure
    // formats (XPM, WebP, XBM) that QImageReader supports but users
    // rarely intend to PDF-wrap.
    return QStringList{
        "png", "jpg", "jpeg", "bmp", "gif", "tif", "tiff",
    };
}

QString ImageToPdfWizard::imageFilterString()
{
    QStringList globs;
    for (const QString& ext : supportedImageExtensions()) {
        globs << ("*." + ext);
    }
    return QObject::tr("Image Files (%1);;All Files (*)").arg(globs.join(' '));
}

namespace {
// Forward declaration: the shared body is defined below, next to the page
// sizing it owns.
bool renderImagesIntoImpl(const QStringList& images, QPdfWriter& writer);
}  // namespace

bool ImageToPdfWizard::writePdf(const QStringList& images,
                                 const std::filesystem::path& outPath)
{
    if (images.isEmpty()) return false;
    QPdfWriter writer(QString::fromStdString(outPath.string()));
    return renderImagesIntoImpl(images, writer);
}

bool ImageToPdfWizard::writePdf(const QStringList& images, QIODevice* device)
{
    if (images.isEmpty() || device == nullptr) return false;
    QPdfWriter writer(device);
    return renderImagesIntoImpl(images, writer);
}

namespace {

/// The body both overloads share. Split out rather than duplicated: the page
/// sizing below carries an ISO 32000-1 Annex C.2 clamp (meta-audit F17, gap
/// A1-31) that a second copy would eventually drift away from.
bool renderImagesIntoImpl(const QStringList& images, QPdfWriter& writer)
{
    writer.setTitle(QObject::tr("Images"));
    writer.setResolution(72);  // 1 pt = 1 px in our mapping

    QPainter painter;
    bool any = false;
    for (int i = 0; i < images.size(); ++i) {
        QImage img(images[i]);
        if (img.isNull()) continue;

        // Size the page to the image's pixel dimensions at 72 DPI.
        // QPageSize::Point means 1 unit = 1 point. Clamp to a
        // minimum to avoid zero-size pages from pathological inputs.
        // DEFECT FIX (meta-audit follow-up F17, gap A1-31): only the LOWER
        // bound was clamped, so a 20000 px wide image produced a 20000 pt
        // page. ISO 32000-1 Annex C.2 caps a page at 14400 units, and
        // conforming readers (Acrobat among them) refuse anything larger --
        // PDFE was silently emitting documents it could not itself round-trip
        // through a conforming reader. Scale the page down isotropically so
        // the image keeps its aspect ratio; the raster is unchanged, only the
        // page's user-space size is bounded.
        constexpr double kMaxPageUnits = 14400.0;
        double wPtD = std::max(1, img.width());
        double hPtD = std::max(1, img.height());
        const double over = std::max(wPtD, hPtD) / kMaxPageUnits;
        if (over > 1.0) {
            wPtD = std::max(1.0, wPtD / over);
            hPtD = std::max(1.0, hPtD / over);
        }
        const int wPt = static_cast<int>(wPtD);
        const int hPt = static_cast<int>(hPtD);
        QPageSize pageSize(QSizeF(wPt, hPt), QPageSize::Point);
        QPageLayout layout(pageSize, QPageLayout::Portrait, QMarginsF());
        layout.setMode(QPageLayout::FullPageMode);
        writer.setPageLayout(layout);

        if (!any) {
            // First page: begin the painter now that the layout is set.
            if (!painter.begin(&writer)) return false;
        } else {
            writer.newPage();
        }
        // drawImage with a target rect of the full page — Qt scales
        // the image to fit.
        painter.drawImage(QRectF(0, 0, wPt, hPt), img);
        any = true;
    }
    if (any) painter.end();
    return any;
}

}  // namespace

std::filesystem::path ImageToPdfWizard::runInteractive(QWidget* parent)
{
    QStringList images = QFileDialog::getOpenFileNames(
        parent, QObject::tr("Choose Images"), QString(), imageFilterString());
    if (images.isEmpty()) return {};

    QString outPath = QFileDialog::getSaveFileName(
        parent, QObject::tr("Save As PDF"), QString(),
        QObject::tr("PDF Files (*.pdf)"));
    if (outPath.isEmpty()) return {};
    if (!outPath.endsWith(".pdf", Qt::CaseInsensitive)) {
        outPath += ".pdf";
    }

    std::filesystem::path out(outPath.toStdString());
    if (!writePdf(images, out)) {
        QMessageBox::warning(parent,
            QObject::tr("Create PDF from Images"),
            QObject::tr("No images could be loaded; nothing written."));
        return {};
    }
    return out;
}

} // namespace pdfe::ui
