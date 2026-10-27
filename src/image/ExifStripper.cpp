#include "image/ExifStripper.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <QBuffer>
#include <QByteArray>

#include <algorithm>
#include <cstring>

namespace pdfe::image {

namespace {

/// Convert fz_pixmap to QImage based on number of components.
QImage pixmapToQImage(fz_pixmap* pix) {
    if (!pix) return QImage();

    int width = fz_pixmap_width(nullptr, pix);
    int height = fz_pixmap_height(nullptr, pix);
    int n = fz_pixmap_components(nullptr, pix);
    unsigned char* samples = fz_pixmap_samples(nullptr, pix);
    int stride = fz_pixmap_stride(nullptr, pix);

    QImage::Format format;
    if (n == 1) {
        format = QImage::Format_Grayscale8;
    } else if (n == 3) {
        format = QImage::Format_RGB888;
    } else if (n == 4) {
        format = QImage::Format_RGBA8888;
    } else {
        return QImage(); // Unsupported format
    }

    QImage image(width, height, format);
    if (image.isNull()) return QImage();

    for (int y = 0; y < height; ++y) {
        const unsigned char* src = samples + y * stride;
        unsigned char* dst = image.scanLine(y);
        std::memcpy(dst, src, width * n);
    }

    return image;
}

/// Convert QImage to fz_pixmap.
fz_pixmap* qImageToPixmap(fz_context* ctx, const QImage& image) {
    if (image.isNull()) return nullptr;

    int width = image.width();
    int height = image.height();

    QImage convertedImage;
    fz_colorspace* cs = nullptr;
    int hasAlpha = 0;
    int components = 0;

    switch (image.format()) {
        case QImage::Format_Grayscale8:
            convertedImage = image;
            cs = fz_device_gray(ctx);
            hasAlpha = 0;
            components = 1;
            break;
        case QImage::Format_RGB888:
            convertedImage = image;
            cs = fz_device_rgb(ctx);
            hasAlpha = 0;
            components = 3;
            break;
        case QImage::Format_RGBA8888:
            convertedImage = image;
            cs = fz_device_rgb(ctx);
            hasAlpha = 1;
            components = 4;
            break;
        default:
            // Convert to RGBA for other formats
            convertedImage = image.convertToFormat(QImage::Format_RGBA8888);
            cs = fz_device_rgb(ctx);
            hasAlpha = 1;
            components = 4;
            break;
    }

    fz_pixmap* pix = fz_new_pixmap(ctx, cs, width, height, nullptr, hasAlpha);
    if (!pix) return nullptr;

    unsigned char* samples = fz_pixmap_samples(ctx, pix);
    int stride = fz_pixmap_stride(ctx, pix);

    for (int y = 0; y < height; ++y) {
        const unsigned char* src = convertedImage.constScanLine(y);
        unsigned char* dst = samples + y * stride;
        std::memcpy(dst, src, width * components);
    }

    return pix;
}

} // anonymous namespace

class ExifStripper::Impl {
public:
    std::shared_ptr<pdfe::core::MuPdfContext> context;

    explicit Impl(std::shared_ptr<pdfe::core::MuPdfContext> ctx)
        : context(std::move(ctx)) {}

    pdf_page* getPdfPage(const pdfe::core::Page* page) const {
        fz_page* fzPage = page->fzPage();
        return pdf_page_from_fz_page(nullptr, fzPage);
    }

    pdf_document* getPdfDoc(const pdfe::core::Document* doc) const {
        fz_document* fzDoc = doc->fzDocument();
        return pdf_document_from_fz_document(nullptr, fzDoc);
    }
};

ExifStripper::ExifStripper(std::shared_ptr<pdfe::core::MuPdfContext> context)
    : impl_(std::make_unique<Impl>(std::move(context))) {
}

ExifStripper::~ExifStripper() = default;

QImage ExifStripper::stripExif(const QImage& image) {
    if (image.isNull()) return QImage();

    // NOTE: a PNG save->loadFromData round-trip does NOT strip metadata --
    // Qt writes QImage::text() entries as PNG tEXt/zTXt chunks and reads them
    // straight back, so embedded GPS/camera/Description tags survive. To
    // actually scrub privacy metadata we rebuild a fresh QImage from the raw
    // pixels only, which carries no text() metadata by construction.
    QImage clean(image.width(), image.height(), image.format());
    if (clean.isNull()) return image;  // unsupported/degenerate format: fail safe

    const int bytesPerLine = std::min(image.bytesPerLine(), clean.bytesPerLine());
    for (int y = 0; y < image.height(); ++y) {
        std::memcpy(clean.scanLine(y), image.constScanLine(y),
                    static_cast<size_t>(bytesPerLine));
    }
    // Preserve the palette for indexed formats (pixels index into it).
    if (!image.colorTable().isEmpty()) {
        clean.setColorTable(image.colorTable());
    }
    // clean has an empty text() map -> all embedded metadata is gone.
    return clean;
}

bool ExifStripper::stripImage(const pdfe::core::Page* page, const pdfe::core::Document* doc,
                               const std::string& xobjectName) {
    if (!page || !doc || xobjectName.empty()) return false;
    // Stripping EXIF removes metadata from the image stream; force a
    // full-rewrite save so the original (privacy-sensitive) bytes are
    // scrubbed rather than left orphaned (pipeline b2e47d91, Tier 1).
    doc->markRequiresFullRewrite();

    bool success = false;

    // Owned by THIS frame -- the CALLER's frame, not the lambda's. The lambda
    // body below is the body of MuPdfContext::execute's fz_try, which is
    // setjmp/longjmp: an error from any pdf_*/fz_* call abandons the lambda's
    // entire frame without running destructors, so a QImage declared anywhere
    // inside it leaks its shared, refcounted pixel buffer. Declaring them at
    // the top of the lambda is NOT enough -- that was the first attempt at
    // this fix, and tools/gate/fz_try_raii_scan.py still reported both.
    QImage qImage;
    QImage cleanQImage;

    impl_->context->execute([&](fz_context* ctx) {
        pdf_page* pdfPage = impl_->getPdfPage(page);
        pdf_document* pdfDoc = impl_->getPdfDoc(doc);

        if (!pdfPage || !pdfDoc) return;

        // Get page resources
        pdf_obj* resources = pdf_page_resources(ctx, pdfPage);
        if (!resources) return;

        // Get XObject dictionary
        pdf_obj* xobjectDict = pdf_dict_get(ctx, resources, PDF_NAME(XObject));
        if (!xobjectDict) return;

        // Get the specific image XObject by name
        pdf_obj* xobj = pdf_dict_gets(ctx, xobjectDict, xobjectName.c_str());
        if (!xobj) return;

        // Verify it's an image
        pdf_obj* subtype = pdf_dict_get(ctx, xobj, PDF_NAME(Subtype));
        if (!pdf_name_eq(ctx, subtype, PDF_NAME(Image))) return;

        fz_image* fzImage = nullptr;
        fz_pixmap* pix = nullptr;
        fz_pixmap* cleanPix = nullptr;
        fz_image* newImage = nullptr;
        pdf_obj* newObj = nullptr;

        fz_try(ctx) {
            // Load the image
            fzImage = pdf_load_image(ctx, pdfDoc, xobj);

            // Convert to pixmap
            pix = fz_get_pixmap_from_image(ctx, fzImage, nullptr, nullptr, nullptr, nullptr);

            // Convert to QImage
            qImage = pixmapToQImage(pix);
            if (qImage.isNull()) {
                fz_throw(ctx, FZ_ERROR_GENERIC, "Failed to convert pixmap to QImage");
            }

            // Strip EXIF metadata
            cleanQImage = stripExif(qImage);

            // Convert back to pixmap
            cleanPix = qImageToPixmap(ctx, cleanQImage);
            if (!cleanPix) {
                fz_throw(ctx, FZ_ERROR_GENERIC, "Failed to convert QImage to pixmap");
            }

            // Create new image from clean pixmap
            newImage = fz_new_image_from_pixmap(ctx, cleanPix, nullptr);

            // Add image to document and get the PDF object reference
            newObj = pdf_add_image(ctx, pdfDoc, newImage);

            // Replace in XObject dictionary
            pdf_dict_puts(ctx, xobjectDict, xobjectName.c_str(), newObj);

            success = true;
        }
        fz_always(ctx) {
            fz_drop_image(ctx, newImage);
            fz_drop_pixmap(ctx, cleanPix);
            fz_drop_pixmap(ctx, pix);
            fz_drop_image(ctx, fzImage);
        }
        fz_catch(ctx) {
            // Error already logged by MuPDF
            success = false;
        }
    });

    return success;
}

int ExifStripper::stripPageImages(const pdfe::core::Page* page, const pdfe::core::Document* doc) {
    if (!page || !doc) return 0;
    doc->markRequiresFullRewrite();  // EXIF strip orphans original image bytes → full rewrite

    int count = 0;

    // In the CALLER's frame for the same reason as stripImage() above: the
    // lambda's frame is abandoned wholesale by a longjmp. Reused across loop
    // iterations, so each pass assigns rather than declares.
    QImage qImage;
    QImage cleanQImage;

    impl_->context->execute([&](fz_context* ctx) {
        pdf_page* pdfPage = impl_->getPdfPage(page);
        pdf_document* pdfDoc = impl_->getPdfDoc(doc);

        if (!pdfPage || !pdfDoc) return;

        // Get page resources
        pdf_obj* resources = pdf_page_resources(ctx, pdfPage);
        if (!resources) return;

        // Get XObject dictionary
        pdf_obj* xobjectDict = pdf_dict_get(ctx, resources, PDF_NAME(XObject));
        if (!xobjectDict) return;

        // Iterate through all XObjects
        int numXObjects = pdf_dict_len(ctx, xobjectDict);
        for (int i = 0; i < numXObjects; ++i) {
            pdf_obj* key = pdf_dict_get_key(ctx, xobjectDict, i);
            pdf_obj* xobj = pdf_dict_get_val(ctx, xobjectDict, i);

            if (!key || !xobj) continue;

            // Check if it's an image
            pdf_obj* subtype = pdf_dict_get(ctx, xobj, PDF_NAME(Subtype));
            if (!pdf_name_eq(ctx, subtype, PDF_NAME(Image))) continue;

            // Get the name as string
            const char* name = pdf_to_name(ctx, key);
            if (!name) continue;

            // Strip this image (call the non-execute version directly)
            // We need to do the work inline to avoid nested execute() calls
            fz_image* fzImage = nullptr;
            fz_pixmap* pix = nullptr;
            fz_pixmap* cleanPix = nullptr;
            fz_image* newImage = nullptr;
            pdf_obj* newObj = nullptr;

            qImage = QImage();
            cleanQImage = QImage();

            fz_try(ctx) {
                // Load the image
                fzImage = pdf_load_image(ctx, pdfDoc, xobj);

                // Convert to pixmap
                pix = fz_get_pixmap_from_image(ctx, fzImage, nullptr, nullptr, nullptr, nullptr);

                // Convert to QImage
                qImage = pixmapToQImage(pix);
                if (qImage.isNull()) {
                    fz_throw(ctx, FZ_ERROR_GENERIC, "Failed to convert pixmap to QImage");
                }

                // Strip EXIF metadata
                cleanQImage = stripExif(qImage);

                // Convert back to pixmap
                cleanPix = qImageToPixmap(ctx, cleanQImage);
                if (!cleanPix) {
                    fz_throw(ctx, FZ_ERROR_GENERIC, "Failed to convert QImage to pixmap");
                }

                // Create new image from clean pixmap
                newImage = fz_new_image_from_pixmap(ctx, cleanPix, nullptr);

                // Add image to document and get the PDF object reference
                newObj = pdf_add_image(ctx, pdfDoc, newImage);

                // Replace in XObject dictionary
                pdf_dict_puts(ctx, xobjectDict, name, newObj);

                ++count;
            }
            fz_always(ctx) {
                fz_drop_image(ctx, newImage);
                fz_drop_pixmap(ctx, cleanPix);
                fz_drop_pixmap(ctx, pix);
                fz_drop_image(ctx, fzImage);
            }
            fz_catch(ctx) {
                // Continue to next image on error
            }
        }
    });

    return count;
}

int ExifStripper::stripAllImages(pdfe::core::Document* doc) {
    if (!doc) return 0;
    doc->markRequiresFullRewrite();  // EXIF strip orphans original image bytes → full rewrite

    int total = 0;
    int numPages = doc->pageCount();

    for (int i = 0; i < numPages; ++i) {
        auto page = doc->page(i);
        if (page) {
            total += stripPageImages(page.get(), doc);
        }
    }

    return total;
}

} // namespace pdfe::image
