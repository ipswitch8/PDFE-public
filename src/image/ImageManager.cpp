#include "image/ImageManager.h"
#include "image/ImageSampleIndex.h"
#include "image/ImagePlacementScanner.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/PageContentGuard.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <QBuffer>
#include <QImageReader>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>  // std::exception (GitHub #392 insert exception boundary)
#include <new>        // std::bad_alloc

namespace pdfe::image {

namespace {
// Standalone holder for a retained content-stream / image-XObject object.
// Deep-copies the object into its OWN pdf_document so it survives a full-rewrite
// save (drop+reopen) or saveAs (GC-renumber) of the live document. Mirrors
// Document::RetainedPageHolder (R2-01). On restore the object is grafted back
// into whatever document is live at undo time; releaseRef drops the holder.
struct RetainedImageObjHolder {
    pdf_document* doc = nullptr;   // standalone holder document
    pdf_obj*      obj = nullptr;   // deep copy, lives in `doc`
};
// Deep-copy `src` (a live-doc obj) into a fresh standalone holder document.
void* makeRetainedObjHolder(fz_context* ctx, pdf_obj* src) {
    if (!src) return nullptr;
    pdf_document* holder = pdf_create_document(ctx);
    if (!holder) fz_throw(ctx, FZ_ERROR_GENERIC, "pdf_create_document failed");
    pdf_obj* copy = nullptr;
    fz_try(ctx) {
        copy = pdf_graft_object(ctx, holder, src);
    } fz_catch(ctx) {
        pdf_drop_document(ctx, holder);
        fz_rethrow(ctx);
    }
    return new RetainedImageObjHolder{holder, copy};
}
} // namespace

// -----------------------------------------------------------------------
// Impl
// -----------------------------------------------------------------------
class ImageManager::Impl {
public:
    std::shared_ptr<pdfe::core::MuPdfContext> context;

    explicit Impl(std::shared_ptr<pdfe::core::MuPdfContext> ctx) : context(std::move(ctx)) {}

    pdf_page* getPdfPage(const pdfe::core::Page* page) const {
        fz_page* fzPage = page->fzPage();
        return pdf_page_from_fz_page(nullptr, fzPage);
    }

    pdf_document* getPdfDoc(const pdfe::core::Document* doc) const {
        fz_document* fzDoc = doc->fzDocument();
        return pdf_document_from_fz_document(nullptr, fzDoc);
    }

    // Enumerate every image placement on a page (docs/image-placement-model.md).
    //
    // One entry per `Do` of an Image XObject, in content order, including the
    // Do's inside Form XObjects (GitHub #427; reported with inFormXObject and
    // not editable). Each entry carries:
    //   ctm       - effective PDF user-space CTM (Y-up), unchanged meaning;
    //   pageCtm   - the same placement in the RENDERED fitz frame: user space
    //               mapped through pdf_page_transform, so page /Rotate and the
    //               box origin are applied (GitHub #426) and Y points down
    //               (GitHub #373);
    //   crop      - the unit-square clip of a PDFE wrapper (GitHub #420);
    //   rotation / flips / skew from placement::decompose (GitHub #424);
    //   boundingBox  - the VISIBLE region, upright, centred on the visible
    //                  centre; placementBox - the whole placement.
    std::vector<PageImageInfo> enumerateViaScanner(
        fz_context* ctx, pdf_document* pdfDoc, pdf_page* pdfPage, int pageIndex)
    {
        using namespace pdfe::image::placement;
        std::vector<PageImageInfo> found;
        const std::vector<DoSite> sites = scanPage(ctx, pdfPage, /*recurseForms=*/true);
        const Affine pageT = pageTransform(ctx, pdfPage);

        for (const DoSite& site : sites) {
            if (!site.isImage || !site.xobject) continue;
            PageImageInfo info;
            info.xobjectName = site.name;
            info.pageIndex = pageIndex;
            info.occurrence = site.occurrence;
            info.inFormXObject = site.formDepth > 0;

            // Pixel facts: the decoded image when it loads, else its dictionary.
            fz_image* image = nullptr;
            fz_var(image);
            fz_try(ctx) {
                image = pdf_load_image(ctx, pdfDoc, site.xobject);
            }
            fz_catch(ctx) {
                fz_rethrow_if(ctx, FZ_ERROR_SYSTEM);
                fz_report_error(ctx);
                image = nullptr;
            }
            if (image) {
                info.pixelWidth = image->w;
                info.pixelHeight = image->h;
                info.bitsPerComponent = image->bpc;
                info.components = image->n;
                info.isMask = image->imagemask != 0;
                fz_drop_image(ctx, image);
            } else {
                info.pixelWidth = pdf_dict_get_int(ctx, site.xobject, PDF_NAME(Width));
                info.pixelHeight = pdf_dict_get_int(ctx, site.xobject, PDF_NAME(Height));
                info.bitsPerComponent = pdf_dict_get_int(ctx, site.xobject, PDF_NAME(BitsPerComponent));
                info.isMask = pdf_dict_get_bool(ctx, site.xobject, PDF_NAME(ImageMask));
            }

            const Affine& m = site.ctm;
            info.ctm = m.toArray();

            const Affine P = concat(m, pageT);  // unit square -> rendered fitz frame
            info.pageCtm = P.toArray();
            const Decomposition dec = decompose(P);
            info.rotationRadians = static_cast<float>(dec.rotation);
            info.flippedX = dec.flipX;
            info.flippedY = dec.flipY;
            info.skewRadians = static_cast<float>(dec.skew);
            info.displayWidth = static_cast<float>(dec.width);
            info.displayHeight = static_cast<float>(dec.height);

            const UnitCrop& c = site.crop;
            info.cropped = site.cropped;
            info.crop = {float(c.u0), float(c.v0), float(c.u1), float(c.v1)};

            // GitHub #342/#373/#420: an upright rect centred on the image's
            // centre in the Y-down rendered frame; the tool rotates it by
            // -rotationRadians about its centre. boundingBox is the VISIBLE
            // part (the crop mapped through the placement), placementBox the
            // whole image.
            const QPointF full = P.apply(0.5, 0.5);
            info.placementBox = QRectF(full.x() - dec.width / 2.0, full.y() - dec.height / 2.0,
                                       dec.width, dec.height);
            const QPointF vis = visibleCentre(P, c);
            const double vw = (c.u1 - c.u0) * dec.width;
            const double vh = (c.v1 - c.v0) * dec.height;
            info.boundingBox = QRectF(vis.x() - vw / 2.0, vis.y() - vh / 2.0, vw, vh);

            // DPI of the full placement
            if (info.displayWidth > 0)
                info.xDpi = static_cast<int>(std::round(info.pixelWidth * 72.0f / info.displayWidth));
            if (info.displayHeight > 0)
                info.yDpi = static_cast<int>(std::round(info.pixelHeight * 72.0f / info.displayHeight));

            info.imageIndex = static_cast<int>(found.size());
            classifyFilter(ctx, site.xobject, info);
            found.push_back(std::move(info));
        }
        return found;
    }

    // DCT / JPX / JBIG2 from the image's /Filter (a name or an array).
    static void classifyFilter(fz_context* ctx, pdf_obj* xobj, PageImageInfo& info)
    {
        auto mark = [&](pdf_obj* f) {
            if (pdf_name_eq(ctx, f, PDF_NAME(DCTDecode))) {
                info.isJpeg = true;
            } else if (pdf_name_eq(ctx, f, PDF_NAME(JPXDecode))) {
                info.isJpx = true;    // #50: JPEG2000 (gated OFF by default)
            } else if (pdf_name_eq(ctx, f, PDF_NAME(JBIG2Decode))) {
                info.isJbig2 = true;  // SEC-56: JBIG2 (gated OFF by default)
            }
        };
        pdf_obj* filter = pdf_dict_get(ctx, xobj, PDF_NAME(Filter));
        if (pdf_is_array(ctx, filter)) {
            for (int fi = 0, flen = pdf_array_len(ctx, filter); fi < flen; ++fi)
                mark(pdf_array_get(ctx, filter, fi));
        } else {
            mark(filter);
        }
    }

    // Generate a unique XObject name for the page
    std::string generateXObjectName(fz_context* ctx, pdf_page* pdfPage) {
        auto resources = pdf_page_resources(ctx, pdfPage);
        auto xobjectDict = pdf_dict_get(ctx, resources, PDF_NAME(XObject));
        int count = xobjectDict ? pdf_dict_len(ctx, xobjectDict) : 0;

        for (int attempt = count; attempt < count + 100; ++attempt) {
            std::string name = "Im" + std::to_string(attempt);
            if (!xobjectDict || !pdf_dict_gets(ctx, xobjectDict, name.c_str())) {
                return name;
            }
        }
        return "Im999"; // Fallback
    }
};

// -----------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------

ImageManager::ImageManager(std::shared_ptr<pdfe::core::MuPdfContext> context)
    : impl_(std::make_unique<Impl>(std::move(context)))
{
}

ImageManager::~ImageManager() = default;

std::vector<PageImageInfo> ImageManager::enumerateImages(
    const pdfe::core::Page* page, const pdfe::core::Document* doc)
{
    if (!page || !doc) return {};

    std::vector<PageImageInfo> result;
    impl_->context->execute([&](fz_context* ctx) {
        pdf_page* pdfPage = impl_->getPdfPage(page);
        pdf_document* pdfDoc = impl_->getPdfDoc(doc);
        result = impl_->enumerateViaScanner(ctx, pdfDoc, pdfPage, page->index());
    });
    return result;
}

PageImageInfo ImageManager::getImageInfo(
    const pdfe::core::Page* page, const pdfe::core::Document* doc, int imageIndex)
{
    auto images = enumerateImages(page, doc);
    if (imageIndex < 0 || imageIndex >= static_cast<int>(images.size())) {
        return {};
    }
    return images[imageIndex];
}

int ImageManager::imageCount(const pdfe::core::Page* page, const pdfe::core::Document* doc)
{
    return static_cast<int>(enumerateImages(page, doc).size());
}

ImageOperationResult ImageManager::insertImage(
    const pdfe::core::Page* page, const pdfe::core::Document* doc,
    const InsertImageOptions& opts)
{
    ImageOperationResult result;
    if (!page || !doc) {
        result.errorMessage = "Null page or document";
        return result;
    }
    if (opts.filePath.empty()) {
        result.errorMessage = "No file path specified";
        return result;
    }
    if (opts.targetRect.isEmpty()) {
        result.errorMessage = "Target rectangle is empty";
        return result;
    }

    // Load the image. docs/image-placement-model.md, "Inserting an image":
    //  - EXIF orientation is applied (GitHub #436): the pixels are stored the
    //    way a viewer shows the photo, not the way the sensor wrote them.
    //    QImage(path) does not apply it; QImageReader with autoTransform does.
    //  - The source FORMAT decides the encoding (GitHub #435), read from the
    //    file's content, not its extension.
    QImageReader reader(QString::fromStdString(opts.filePath));
    reader.setAutoTransform(true);
    // The bytes decide, never the extension: a PNG named .jpg is a PNG.
    reader.setDecideFormatFromContent(true);
    const QByteArray sourceFormat = reader.format().toLower();
    QImage qimg = reader.read();
    if (qimg.isNull()) {
        result.errorMessage = "Failed to load image: " + opts.filePath;
        return result;
    }

    // Normalize pixel format. Transparent inputs (e.g. Format_RGBA8888,
    // Format_Indexed8 with a transparent palette entry) MUST keep their alpha,
    // so convert alpha-bearing images to Format_ARGB32_Premultiplied rather than
    // dropping to RGB32. Premultiplied is also the format MuPDF's pdf_add_image
    // expects: it un-premultiplies colour on save and splits alpha into an
    // /SMask (source/pdf/pdf-image.c `inva = a ? 255*256/a : 0`). Writing the
    // straight (non-premultiplied) samples blows out anti-aliased coloured
    // pixels; premultiplying here makes the BGRA copy loop below correct. The
    // byte order stays BGRA, so the existing b/g/r/a extraction is unchanged.
    if (qimg.hasAlphaChannel()) {
        if (qimg.format() != QImage::Format_ARGB32_Premultiplied) {
            qimg = qimg.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        }
    } else if (qimg.format() != QImage::Format_RGB32 &&
               qimg.format() != QImage::Format_ARGB32) {
        qimg = qimg.convertToFormat(QImage::Format_RGB32);
    }

    // GitHub #435 — the encoding rule (docs/image-placement-model.md):
    //  - a JPEG source (photographic, already lossy) that is opaque is
    //    re-encoded as DCT at opts.jpegQuality;
    //  - every other source (PNG, GIF, BMP, TIFF, ...) and anything with an
    //    alpha channel is stored losslessly (Flate, alpha as a soft mask), so
    //    line art and screenshots never pick up JPEG artefacts and
    //    transparency is kept. jpegQuality does not apply to them.
    // The pixels are always decoded and re-encoded from a FRESH image that
    // carries no text keys or colour-space tag, so no EXIF / APP1 (GPS,
    // camera, timestamps), comment or other source metadata reaches the PDF,
    // whatever stripExif says (GitHub #436; the dialog's checkbox says so).
    QByteArray dctBytes;
    if (sourceFormat == "jpeg" || sourceFormat == "jpg") {
        if (!qimg.hasAlphaChannel()) {
            QImage clean(qimg.width(), qimg.height(), QImage::Format_RGB32);
            if (clean.isNull()) {
                result.errorMessage = "Failed to insert image: out of memory";
                return result;
            }
            const size_t rowBytes = static_cast<size_t>(qimg.width()) * 4u;
            for (int y = 0; y < qimg.height(); ++y) {
                std::memcpy(clean.scanLine(y), qimg.constScanLine(y), rowBytes);
            }
            QBuffer out(&dctBytes);
            out.open(QIODevice::WriteOnly);
            const int quality = std::clamp(opts.jpegQuality, 0, 100);
            if (!clean.save(&out, "JPEG", quality)) {
                dctBytes.clear();  // fall back to the lossless path below
            }
        }
    }

    // GitHub #392 — exception boundary. MuPdfContext::execute() converts any
    // MuPDF fault inside the lambda (OOM/limit on a large document, or the
    // NULL pdf_document/pdf_page of a non-PDF target below) into a thrown
    // MuPdfException AT ITS OWN BOUNDARY, after the fz frame is popped. Before
    // this try/catch that exception unwound uncaught through the Qt slot ->
    // std::terminate -> immediate process death. Catch it here and report a
    // graceful failure instead; never let it escape. The C++ try/catch MUST
    // wrap the execute() CALL (outside the lambda) — a C++ throw unwinding
    // through fz_try/fz_catch's setjmp/longjmp would corrupt the error stack.
    //
    // The new XObject's name and the read-back enumeration live in THIS frame,
    // not the lambda's: the lambda body runs inside MuPdfContext::execute's
    // fz_try, and a longjmp skips the destructors of its locals
    // (tools/gate/fz_try_raii_scan.py).
    std::string xobjName;
    std::vector<PageImageInfo> placed;
    try {
        impl_->context->execute([&](fz_context* ctx) {
        pdf_page* pdfPage = impl_->getPdfPage(page);
        pdf_document* pdfDoc = impl_->getPdfDoc(doc);

        // GitHub #392 — a non-PDF document (e.g. an image/SVG opened as an
        // fz_document) has no pdf_document/pdf_page, so these are NULL and the
        // MuPDF calls below (pdf_add_image, pdf_page_resources, ...) would
        // dereference NULL / fault. fz_throw is correct INSIDE the lambda
        // (setjmp/longjmp, no C++ RAII on the stack); execute() converts it to
        // a MuPdfException that the try/catch around this call turns into a
        // graceful success=false.
        if (!pdfPage || !pdfDoc) {
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "target is not a PDF document");
        }

        int width = qimg.width();
        int height = qimg.height();
        bool hasAlpha = qimg.hasAlphaChannel();
        int n = hasAlpha ? 4 : 3;

        pdf_obj* imageObj = nullptr;
        if (!dctBytes.isEmpty()) {
            // GitHub #435: the DCT stream encoded above at opts.jpegQuality.
            // pdf_add_image writes a JPEG-compressed fz_image as a DCT stream
            // as is (no re-encode).
            fz_buffer* jbuf = fz_new_buffer_from_copied_data(
                ctx, reinterpret_cast<const unsigned char*>(dctBytes.constData()),
                static_cast<size_t>(dctBytes.size()));
            fz_image* jimg = nullptr;
            fz_var(jimg);
            fz_try(ctx) {
                jimg = fz_new_image_from_buffer(ctx, jbuf);
                imageObj = pdf_add_image(ctx, pdfDoc, jimg);
            }
            fz_always(ctx) {
                fz_drop_image(ctx, jimg);
                fz_drop_buffer(ctx, jbuf);
            }
            fz_catch(ctx) {
                fz_rethrow(ctx);
            }
            result.imageInfo.isJpeg = true;
        } else {
        // Create fz_pixmap (MuPDF uses fz_device_rgb for both; alpha is a separate parameter)
        fz_colorspace* cs = fz_device_rgb(ctx);
        fz_pixmap* pix = fz_new_pixmap(ctx, cs, width, height, nullptr, hasAlpha ? 1 : 0);
        pix->xres = 72;
        pix->yres = 72;

        // Copy pixel data
        unsigned char* samples = pix->samples;
        for (int y = 0; y < height; ++y) {
            const uchar* scanline = qimg.constScanLine(y);
            for (int x = 0; x < width; ++x) {
                // QImage RGB32: BGRA byte order
                uint32_t pixel;
                std::memcpy(&pixel, scanline + x * 4, 4);
                uint8_t b = pixel & 0xFF;
                uint8_t g = (pixel >> 8) & 0xFF;
                uint8_t r = (pixel >> 16) & 0xFF;
                uint8_t a = (pixel >> 24) & 0xFF;

                // GitHub #392 — 64-bit indexing via the shared helper (single
                // source of truth with AnnotationManager::setStampImageAppearance).
                // `int` arithmetic overflowed for an inserted image above
                // ~2^31/n pixels (~46k^2 for n=3), making this a wild
                // out-of-bounds write (heap corruption/AV).
                const size_t idx = imageSampleIndex(y, width, x, n);
                samples[idx] = r;
                samples[idx + 1] = g;
                samples[idx + 2] = b;
                if (hasAlpha) samples[idx + 3] = a;
            }
        }

        // Create fz_image and add to PDF
        fz_image* fzImage = fz_new_image_from_pixmap(ctx, pix, nullptr);
        imageObj = pdf_add_image(ctx, pdfDoc, fzImage);
        fz_drop_image(ctx, fzImage);
        fz_drop_pixmap(ctx, pix);
        }

        // Add to XObject dictionary
        xobjName = impl_->generateXObjectName(ctx, pdfPage);
        pdf_obj* resources = pdf_page_resources(ctx, pdfPage);
        pdf_obj* xobjectDict = pdf_dict_get(ctx, resources, PDF_NAME(XObject));
        if (!xobjectDict) {
            xobjectDict = pdf_new_dict(ctx, pdfDoc, 4);
            pdf_dict_put(ctx, resources, PDF_NAME(XObject), xobjectDict);
        }
        pdf_dict_puts(ctx, xobjectDict, xobjName.c_str(), imageObj);

        // Calculate target rect
        QRectF rect = opts.targetRect;
        if (opts.maintainAspectRatio) {
            float imgAspect = static_cast<float>(width) / height;
            float rectAspect = static_cast<float>(rect.width()) / rect.height();
            if (imgAspect > rectAspect) {
                float newH = rect.width() / imgAspect;
                rect.setTop(rect.top() + (rect.height() - newH) / 2);
                rect.setHeight(newH);
            } else {
                float newW = rect.height() * imgAspect;
                rect.setLeft(rect.left() + (rect.width() - newW) / 2);
                rect.setWidth(newW);
            }
        }

        // GitHub #373/#426: opts.targetRect is fitz Y-down in the RENDERED
        // page frame (top-left origin, page /Rotate applied - the frame of the
        // enumerated bounding boxes and of mouse input). The image is placed
        // upright as the user sees it: in that frame the unit square maps to
        // the rect with v = 0 at its BOTTOM edge, P = [w 0 0 -h x y+h]. The
        // `cm` matrix is PDF USER space, so convert ONCE at this content-stream
        // boundary through the inverse page transform (the same conversion
        // ImageContentEditor uses). On an unrotated page this is exactly
        //   [w 0 0 h  mediaBoxX0 + x  mediaBoxY1 - y - h].
        using namespace pdfe::image::placement;
        const double rw = rect.width(), rh = rect.height();
        const Affine P{rw, 0.0, 0.0, -rh, rect.left(), rect.top() + rh};
        Affine pageInv;
        if (!invert(pageTransform(ctx, pdfPage), pageInv)) {
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "degenerate page transform");
        }
        const Affine M = concat(P, pageInv);

        // Append content stream: q [a b c d e f] cm /ImN Do Q
        char ops[384];
        if (std::abs(M.b) < 1e-9 && std::abs(M.c) < 1e-9) {
            std::snprintf(ops, sizeof(ops),
                "q %.4f 0 0 %.4f %.4f %.4f cm /%s Do Q\n",
                static_cast<float>(M.a), static_cast<float>(M.d),
                static_cast<float>(M.e), static_cast<float>(M.f),
                xobjName.c_str());
        } else {
            std::snprintf(ops, sizeof(ops),
                "q %.4f %.4f %.4f %.4f %.4f %.4f cm /%s Do Q\n",
                static_cast<float>(M.a), static_cast<float>(M.b),
                static_cast<float>(M.c), static_cast<float>(M.d),
                static_cast<float>(M.e), static_cast<float>(M.f),
                xobjName.c_str());
        }

        // Append so the absolute `cm` runs at identity CTM even if the page
        // leaves an ambient transform active (N-up/imposed pages).
        pdfe::core::appendContentIsolatingAmbientCtm(
            ctx, pdfDoc, pdfPage->obj, ops, std::strlen(ops));

        // Fill result
        result.success = true;
        result.imageInfo.xobjectName = xobjName;
        result.imageInfo.pageIndex = page->index();
        result.imageInfo.pixelWidth = width;
        result.imageInfo.pixelHeight = height;
        result.imageInfo.components = n;
        result.imageInfo.displayWidth = static_cast<float>(rect.width());
        result.imageInfo.displayHeight = static_cast<float>(rect.height());
        result.imageInfo.boundingBox = rect;  // fitz Y-down, same space a
                                              // re-enumeration reports (#373)

        // GitHub #434: report the IDENTITY of what was inserted - the name
        // and the occurrence of the new Do among the page's Do's of that
        // name, plus its enumeration index - read back from the page, so
        // InsertImageCommand and the UI address exactly this placement.
        result.imageInfo.occurrence = 0;
        result.imageInfo.imageIndex = -1;
        {
            placed = impl_->enumerateViaScanner(ctx, pdfDoc, pdfPage, page->index());
            for (const PageImageInfo& p : placed) {
                if (p.xobjectName == xobjName && !p.inFormXObject) {
                    result.imageInfo.occurrence = p.occurrence;
                    result.imageInfo.imageIndex = p.imageIndex;
                }
            }
        }
        });
    } catch (const pdfe::core::MuPdfException& e) {
        // Recoverable MuPDF fault (OOM/limit, non-PDF target, ...). Report it
        // as a graceful failure rather than letting it escape (GitHub #392).
        result.success = false;
        result.errorMessage =
            std::string("Failed to insert image: ") + e.what();
    } catch (const std::bad_alloc&) {
        result.success = false;
        result.errorMessage = "Failed to insert image: out of memory";
    } catch (const std::exception& e) {
        result.success = false;
        result.errorMessage =
            std::string("Failed to insert image: ") + e.what();
    }

    return result;
}

ImageOperationResult ImageManager::removeImage(
    const pdfe::core::Page* page, const pdfe::core::Document* doc, int imageIndex)
{
    ImageOperationResult result;
    if (!page || !doc) {
        result.errorMessage = "Null page or document";
        return result;
    }
    auto images = enumerateImages(page, doc);
    if (imageIndex < 0 || imageIndex >= static_cast<int>(images.size())) {
        result.errorMessage = "Invalid image index";
        return result;
    }

    result.imageInfo = images[imageIndex];
    if (images[imageIndex].inFormXObject) {
        // GitHub #427 / docs/image-placement-model.md: a placement inside a
        // Form XObject is enumerated, not editable or removable.
        result.errorMessage = "The image is drawn inside a Form XObject and cannot be removed here";
        return result;
    }
    const ImageOperationResult r = removePlacement(
        page, doc, images[imageIndex].xobjectName, images[imageIndex].occurrence);
    result.success = r.success;
    result.errorMessage = r.errorMessage;
    return result;
}

ImageOperationResult ImageManager::removePlacement(
    const pdfe::core::Page* page, const pdfe::core::Document* doc,
    const std::string& xobjectName, int occurrence)
{
    ImageOperationResult result;
    if (!page || !doc) {
        result.errorMessage = "Null page or document";
        return result;
    }
    if (xobjectName.empty() || occurrence < 0) {
        // occurrence -1 is a placement inside a Form XObject: not removable.
        result.errorMessage = "Not a removable page-level image placement";
        return result;
    }

    // Scanned in THIS frame, not the lambda's: the lambda runs inside
    // MuPdfContext::execute's fz_try, and a longjmp skips the destructors of
    // its locals (tools/gate/fz_try_raii_scan.py).
    std::vector<pdfe::image::placement::DoSite> sites;
    impl_->context->execute([&](fz_context* ctx) {
        using namespace pdfe::image::placement;
        pdf_page* pdfPage = impl_->getPdfPage(page);
        pdf_document* pdfDoc = impl_->getPdfDoc(doc);
        if (!pdfPage || !pdfDoc) {
            result.errorMessage = "Not a PDF page";
            return;
        }

        // GitHub #421: remove THIS placement only - its Do (or the PDFE
        // wrapper around it) is cut from the content in place; the other
        // placements and every other operator stay byte-for-byte.
        sites = scanPage(ctx, pdfPage, /*recurseForms=*/true);
        const DoSite* target = nullptr;
        for (const auto& site : sites) {
            if (site.formDepth == 0 && site.name == xobjectName &&
                site.occurrence == occurrence && site.isImage) {
                target = &site;
                break;
            }
        }
        // Every OTHER Do - page level, or inside a form that inherits the
        // page's resources - that still looks this name up in the page's own
        // resource dict keeps the XObject entry alive.
        int remaining = 0;
        if (target) {
            for (const auto& site : sites) {
                if (&site != target && site.name == xobjectName &&
                    site.resources == target->resources)
                    ++remaining;
            }
        }
        if (!target) {
            result.errorMessage = "Image placement not found in the page content "
                                  "(an image inside a Form XObject is not removable)";
            return;
        }
        if (!removeSite(ctx, pdfDoc, pdfPage, *target)) {
            result.errorMessage = "Could not rewrite the page content";
            return;
        }

        // The XObject resource goes with its last placement on the page.
        if (remaining == 0) {
            pdf_obj* resources = pdf_page_resources(ctx, pdfPage);
            pdf_obj* xobjectDict = pdf_dict_get(ctx, resources, PDF_NAME(XObject));
            if (xobjectDict) {
                pdf_dict_dels(ctx, xobjectDict, xobjectName.c_str());
            }
        }
        result.success = true;
    });

    // Removing an image orphans its (potentially sensitive) XObject stream;
    // force a full-rewrite save so it is scrubbed (pipeline b2e47d91, Tier 1).
    // Only after a real removal.
    if (result.success) doc->markRequiresFullRewrite();
    return result;
}

bool ImageManager::removeImageResourceIfUnused(
    const pdfe::core::Page* page, const pdfe::core::Document* doc,
    const std::string& xobjectName)
{
    if (!page || !doc || xobjectName.empty()) return false;
    bool removed = false;
    impl_->context->execute([&](fz_context* ctx) {
        using namespace pdfe::image::placement;
        pdf_page* pdfPage = impl_->getPdfPage(page);
        pdf_document* pdfDoc = impl_->getPdfDoc(doc);
        if (!pdfPage || !pdfDoc) return;
        pdf_obj* resources = pdf_page_resources(ctx, pdfPage);
        pdf_obj* xobjectDict = pdf_dict_get(ctx, resources, PDF_NAME(XObject));
        if (!xobjectDict || !pdf_dict_gets(ctx, xobjectDict, xobjectName.c_str())) return;
        // Anything that still looks this name up in the page's own resource
        // dict - a page Do, or a Do inside a form inheriting the page's
        // resources - keeps the entry (the rule removePlacement uses).
        for (const DoSite& site : scanPage(ctx, pdfPage, /*recurseForms=*/true)) {
            if (site.name == xobjectName &&
                (site.formDepth == 0 || site.resources == resources))
                return;
        }
        pdf_dict_dels(ctx, xobjectDict, xobjectName.c_str());
        removed = true;
    });
    // The orphaned stream must not survive an incremental save.
    if (removed) doc->markRequiresFullRewrite();
    return removed;
}

ImageOperationResult ImageManager::restoreImage(
    const pdfe::core::Page* page, const pdfe::core::Document* doc,
    const PageImageInfo& savedInfo,
    void* savedImageRef, void* savedContentsRef)
{
    ImageOperationResult result;
    if (!page || !doc) {
        result.errorMessage = "Null page or document";
        return result;
    }

    impl_->context->execute([&](fz_context* ctx) {
        pdf_page* pdfPage = impl_->getPdfPage(page);
        pdf_document* livePdfDoc = impl_->getPdfDoc(doc);

        // The saved refs are standalone holders (R2-01). Graft a COPY into the
        // LIVE document and put that copy into the dict, then drop the local ref
        // (the dict keeps its own reference). This is NON-CONSUMING: the holder
        // remains owned by the command and is freed later via releaseRef.
        if (savedContentsRef) {
            auto* h = static_cast<RetainedImageObjHolder*>(savedContentsRef);
            if (h->obj) {
                // The snapshot (saveContentsRef) is a dict holding the page's
                // /Contents and the resource categories an image's context
                // refers to: ExtGState (opacity / soft mask, GitHub #428) and
                // Properties (marked content / optional content, GitHub #430).
                // Undo puts all of them back, so an edit that touched a
                // resource can never leave it changed after undo (E11). The
                // XObject dict is restored by savedImageRef as before.
                pdf_obj* snapContents = pdf_dict_get(ctx, h->obj, PDF_NAME(Contents));
                if (snapContents) {
                    pdf_obj* live = pdf_graft_object(ctx, livePdfDoc, snapContents);
                    pdf_dict_put(ctx, pdfPage->obj, PDF_NAME(Contents), live);
                    pdf_drop_obj(ctx, live);
                } else {
                    pdf_dict_del(ctx, pdfPage->obj, PDF_NAME(Contents));
                }
                if (pdf_dict_get_bool(ctx, h->obj, PDF_NAME(Resources))) {
                    pdf_obj* resources = pdf_page_resources(ctx, pdfPage);
                    for (pdf_obj* key : {PDF_NAME(ExtGState), PDF_NAME(Properties)}) {
                        pdf_obj* snap = pdf_dict_get(ctx, h->obj, key);
                        pdf_obj* now = resources ? pdf_dict_get(ctx, resources, key) : nullptr;
                        if (snap) {
                            if (!resources) {
                                resources = pdf_dict_put_dict(ctx, pdfPage->obj, PDF_NAME(Resources), 4);
                            }
                            pdf_obj* live = pdf_graft_object(ctx, livePdfDoc, snap);
                            pdf_dict_put(ctx, resources, key, live);
                            pdf_drop_obj(ctx, live);
                        } else if (now) {
                            pdf_dict_del(ctx, resources, key);
                        }
                    }
                }
            }
        }

        // Restore image in XObject dictionary
        if (savedImageRef) {
            auto* h = static_cast<RetainedImageObjHolder*>(savedImageRef);
            if (h->obj) {
                pdf_obj* resources = pdf_page_resources(ctx, pdfPage);
                pdf_obj* xobjectDict = pdf_dict_get(ctx, resources, PDF_NAME(XObject));
                if (xobjectDict) {
                    pdf_obj* live = pdf_graft_object(ctx, livePdfDoc, h->obj);
                    pdf_dict_puts(ctx, xobjectDict, savedInfo.xobjectName.c_str(), live);
                    pdf_drop_obj(ctx, live);
                }
            }
        }

        result.success = true;
        result.imageInfo = savedInfo;
    });

    return result;
}

bool ImageManager::extractImageToFile(
    const pdfe::core::Page* page, const pdfe::core::Document* doc,
    int imageIndex, const std::string& outputPath)
{
    QImage img = extractImage(page, doc, imageIndex);
    if (img.isNull()) return false;
    return img.save(QString::fromStdString(outputPath));
}

QImage ImageManager::extractImage(
    const pdfe::core::Page* page, const pdfe::core::Document* doc, int imageIndex)
{
    if (!page || !doc) return {};

    auto images = enumerateImages(page, doc);
    if (imageIndex < 0 || imageIndex >= static_cast<int>(images.size())) {
        return {};
    }

    QImage result;
    std::string xobjName = images[imageIndex].xobjectName;

    impl_->context->execute([&](fz_context* ctx) {
        pdf_page* pdfPage = impl_->getPdfPage(page);

        // Load the image from XObject dictionary
        pdf_obj* resources = pdf_page_resources(ctx, pdfPage);
        pdf_obj* xobjectDict = pdf_dict_get(ctx, resources, PDF_NAME(XObject));
        if (!xobjectDict) return;

        pdf_obj* xobj = pdf_dict_gets(ctx, xobjectDict, xobjName.c_str());
        if (!xobj) return;

        pdf_document* pdfDoc = impl_->getPdfDoc(doc);
        fz_image* fzImage = nullptr;
        fz_pixmap* pix = nullptr;
        fz_pixmap* rgb = nullptr;
        fz_var(fzImage);
        fz_var(pix);
        fz_var(rgb);
        fz_try(ctx) {
            fzImage = pdf_load_image(ctx, pdfDoc, xobj);
            pix = fz_get_pixmap_from_image(ctx, fzImage, nullptr, nullptr, nullptr, nullptr);

            // The pixmap is in the image's own colour space. Only 8-bit gray
            // and RGB samples can be copied into a QImage as they are; any
            // other space (CMYK, Lab, ICC-based, DeviceN, ...) is converted to
            // RGB the way the page renders it. A 4-component pixmap is CMYK,
            // not RGBA: reading it as RGBA gave K as alpha (GitHub #439).
            fz_colorspace* cs = fz_pixmap_colorspace(ctx, pix);
            const bool gray = cs && fz_colorspace_is_gray(ctx, cs) && pix->n == 1;
            const bool rgbSpace = cs && fz_colorspace_is_rgb(ctx, cs);
            fz_pixmap* use = pix;
            if (!gray && !rgbSpace) {
                rgb = fz_convert_pixmap(ctx, pix, fz_device_rgb(ctx), nullptr, nullptr,
                                        fz_default_color_params, 1);
                use = rgb;
            }

            const int w = use->w;
            const int h = use->h;
            const int n = use->n;
            QImage::Format fmt = QImage::Format_Invalid;
            if (n == 1 && !use->alpha) fmt = QImage::Format_Grayscale8;
            else if (n == 3 && !use->alpha) fmt = QImage::Format_RGB888;
            // MuPDF keeps colour premultiplied by alpha in a pixmap with alpha.
            else if (n == 4 && use->alpha) fmt = QImage::Format_RGBA8888_Premultiplied;
            if (fmt != QImage::Format_Invalid) {
                result = QImage(w, h, fmt);
                // A QImage that could not be allocated is null and has no
                // scan lines: return it (null, "could not extract") rather
                // than copy into it.
                for (int y = 0; y < h && !result.isNull(); ++y) {
                    std::memcpy(result.scanLine(y), use->samples + y * use->stride,
                                static_cast<size_t>(w) * n);
                }
            }
        }
        fz_always(ctx) {
            fz_drop_pixmap(ctx, rgb);
            fz_drop_pixmap(ctx, pix);
            fz_drop_image(ctx, fzImage);
        }
        fz_catch(ctx) {
            fz_rethrow(ctx);
        }
    });

    return result;
}

void* ImageManager::saveImageRef(
    const pdfe::core::Page* page, const pdfe::core::Document* doc,
    const std::string& xobjectName)
{
    void* ref = nullptr;
    if (!page || !doc) return ref;

    impl_->context->execute([&](fz_context* ctx) {
        pdf_page* pdfPage = impl_->getPdfPage(page);
        pdf_obj* resources = pdf_page_resources(ctx, pdfPage);
        pdf_obj* xobjectDict = pdf_dict_get(ctx, resources, PDF_NAME(XObject));
        if (xobjectDict) {
            pdf_obj* obj = pdf_dict_gets(ctx, xobjectDict, xobjectName.c_str());
            if (obj) {
                ref = makeRetainedObjHolder(ctx, obj);
            }
        }
    });

    return ref;
}

void* ImageManager::saveContentsRef(
    const pdfe::core::Page* page, const pdfe::core::Document* doc)
{
    void* ref = nullptr;
    if (!page || !doc) return ref;

    impl_->context->execute([&](fz_context* ctx) {
        pdf_page* pdfPage = impl_->getPdfPage(page);
        // GitHub #392 — a non-PDF document has no pdf_page; dereferencing
        // pdfPage->obj here was a NULL-deref AV on the insert/undo path. Leave
        // ref null (nothing to save) rather than crash.
        if (!pdfPage) return;
        // Snapshot dict: /Contents plus the ExtGState and Properties resource
        // dicts (restoreImage puts all three back; see there). /Resources true
        // marks that the resource entries were captured (absent = had none).
        pdf_document* pdfDoc = impl_->getPdfDoc(doc);
        pdf_obj* snap = pdf_new_dict(ctx, pdfDoc, 4);
        fz_try(ctx) {
            pdf_obj* contents = pdf_dict_get(ctx, pdfPage->obj, PDF_NAME(Contents));
            if (contents) pdf_dict_put(ctx, snap, PDF_NAME(Contents), contents);
            pdf_dict_put_bool(ctx, snap, PDF_NAME(Resources), 1);
            pdf_obj* resources = pdf_page_resources(ctx, pdfPage);
            for (pdf_obj* key : {PDF_NAME(ExtGState), PDF_NAME(Properties)}) {
                pdf_obj* v = resources ? pdf_dict_get(ctx, resources, key) : nullptr;
                if (v) pdf_dict_put(ctx, snap, key, v);
            }
            ref = makeRetainedObjHolder(ctx, snap);
        }
        fz_always(ctx) {
            pdf_drop_obj(ctx, snap);
        }
        fz_catch(ctx) {
            fz_rethrow(ctx);
        }
    });

    return ref;
}

void ImageManager::releaseRef(void* ref)
{
    if (!ref) return;
    impl_->context->execute([ref](fz_context* ctx) {
        auto* h = static_cast<RetainedImageObjHolder*>(ref);
        if (h->obj) pdf_drop_obj(ctx, h->obj);
        if (h->doc) pdf_drop_document(ctx, h->doc);
        delete h;
    });
}

} // namespace pdfe::image
