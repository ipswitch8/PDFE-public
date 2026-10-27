#include "image/ImageManager.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/PageContentGuard.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pdfe::image {

// -----------------------------------------------------------------------
// ImageFilterState — used by pdf_filter_page_contents image_filter callback
// -----------------------------------------------------------------------
struct ImageFilterState {
    std::string targetName;          // XObject name to remove (empty = enumerate only)
    bool removeTarget = false;       // If true, return NULL for targetName
    std::vector<PageImageInfo> found; // Images found during enumeration
    int pageIndex = -1;
};

static fz_image* imageFilterCallback(
    fz_context* ctx, void* opaque, fz_matrix ctm,
    const char* name, fz_image* image, fz_rect scissor)
{
    (void)scissor;
    auto* state = static_cast<ImageFilterState*>(opaque);

    if (state->removeTarget && name && state->targetName == name) {
        return nullptr; // Remove this image
    }

    // Record image info
    if (name && image) {
        PageImageInfo info;
        info.xobjectName = name;
        info.pageIndex = state->pageIndex;
        info.pixelWidth = image->w;
        info.pixelHeight = image->h;
        info.bitsPerComponent = image->bpc;
        info.components = image->n;
        info.isMask = image->imagemask != 0;

        // CTM gives us position and display size
        info.ctm = {ctm.a, ctm.b, ctm.c, ctm.d, ctm.e, ctm.f};
        info.displayWidth = std::sqrt(ctm.a * ctm.a + ctm.b * ctm.b);
        info.displayHeight = std::sqrt(ctm.c * ctm.c + ctm.d * ctm.d);

        // Bounding box from CTM
        float x0 = ctm.e;
        float y0 = ctm.f;
        float x1 = ctm.e + ctm.a;
        float y1 = ctm.f + ctm.d;
        if (x0 > x1) std::swap(x0, x1);
        if (y0 > y1) std::swap(y0, y1);
        info.boundingBox = QRectF(x0, y0, x1 - x0, y1 - y0);

        // DPI
        if (info.displayWidth > 0)
            info.xDpi = static_cast<int>(std::round(info.pixelWidth * 72.0f / info.displayWidth));
        if (info.displayHeight > 0)
            info.yDpi = static_cast<int>(std::round(info.pixelHeight * 72.0f / info.displayHeight));

        state->found.push_back(std::move(info));
    }

    return image; // Keep the image
}

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

    // Run image_filter to enumerate images on a page
    std::vector<PageImageInfo> enumerateViaFilter(
        fz_context* ctx, pdf_document* pdfDoc, pdf_page* pdfPage, int pageIndex)
    {
        ImageFilterState state;
        state.pageIndex = pageIndex;
        state.removeTarget = false;

        pdf_sanitize_filter_options sopts;
        std::memset(&sopts, 0, sizeof(sopts));
        sopts.opaque = &state;
        sopts.image_filter = imageFilterCallback;

        pdf_filter_factory factories[2];
        std::memset(factories, 0, sizeof(factories));
        factories[0].filter = pdf_new_sanitize_filter;
        factories[0].options = &sopts;
        factories[1].filter = nullptr;

        pdf_filter_options opts;
        std::memset(&opts, 0, sizeof(opts));
        opts.filters = factories;
        opts.newlines = 1;
        opts.no_update = 1; // Don't actually modify the page

        pdf_filter_page_contents(ctx, pdfDoc, pdfPage, &opts);

        // Assign indices and check JPEG via XObject dict
        auto resources = pdf_page_resources(ctx, pdfPage);
        auto xobjectDict = pdf_dict_get(ctx, resources, PDF_NAME(XObject));

        for (int i = 0; i < static_cast<int>(state.found.size()); ++i) {
            state.found[i].imageIndex = i;

            // Check for DCTDecode filter (JPEG)
            auto xobj = pdf_dict_gets(ctx, xobjectDict, state.found[i].xobjectName.c_str());
            if (xobj) {
                auto filter = pdf_dict_get(ctx, xobj, PDF_NAME(Filter));
                if (pdf_name_eq(ctx, filter, PDF_NAME(DCTDecode))) {
                    state.found[i].isJpeg = true;
                } else if (pdf_name_eq(ctx, filter, PDF_NAME(JPXDecode))) {
                    state.found[i].isJpx = true;  // #50: JPEG2000 (gated OFF by default)
                } else if (pdf_name_eq(ctx, filter, PDF_NAME(JBIG2Decode))) {
                    state.found[i].isJbig2 = true;  // SEC-56: JBIG2 (gated OFF by default)
                } else if (pdf_is_array(ctx, filter)) {
                    int flen = pdf_array_len(ctx, filter);
                    for (int fi = 0; fi < flen; ++fi) {
                        auto f = pdf_array_get(ctx, filter, fi);
                        if (pdf_name_eq(ctx, f, PDF_NAME(DCTDecode))) {
                            state.found[i].isJpeg = true;
                        } else if (pdf_name_eq(ctx, f, PDF_NAME(JPXDecode))) {
                            state.found[i].isJpx = true;
                        } else if (pdf_name_eq(ctx, f, PDF_NAME(JBIG2Decode))) {
                            state.found[i].isJbig2 = true;  // SEC-56
                        }
                    }
                }
            }
        }

        return state.found;
    }

    // Remove an image from the content stream by XObject name
    void removeViaFilter(
        fz_context* ctx, pdf_document* pdfDoc, pdf_page* pdfPage,
        const std::string& xobjectName)
    {
        ImageFilterState state;
        state.targetName = xobjectName;
        state.removeTarget = true;

        pdf_sanitize_filter_options sopts;
        std::memset(&sopts, 0, sizeof(sopts));
        sopts.opaque = &state;
        sopts.image_filter = imageFilterCallback;

        pdf_filter_factory factories[2];
        std::memset(factories, 0, sizeof(factories));
        factories[0].filter = pdf_new_sanitize_filter;
        factories[0].options = &sopts;
        factories[1].filter = nullptr;

        pdf_filter_options opts;
        std::memset(&opts, 0, sizeof(opts));
        opts.filters = factories;
        opts.newlines = 1;

        pdf_filter_page_contents(ctx, pdfDoc, pdfPage, &opts);
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
        result = impl_->enumerateViaFilter(ctx, pdfDoc, pdfPage, page->index());
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

    // Load image via QImage
    QImage qimg(QString::fromStdString(opts.filePath));
    if (qimg.isNull()) {
        result.errorMessage = "Failed to load image: " + opts.filePath;
        return result;
    }

    // Convert to RGB32 for consistent pixel format
    if (qimg.format() != QImage::Format_RGB32 && qimg.format() != QImage::Format_ARGB32) {
        qimg = qimg.convertToFormat(QImage::Format_RGB32);
    }

    impl_->context->execute([&](fz_context* ctx) {
        pdf_page* pdfPage = impl_->getPdfPage(page);
        pdf_document* pdfDoc = impl_->getPdfDoc(doc);

        int width = qimg.width();
        int height = qimg.height();
        bool hasAlpha = qimg.hasAlphaChannel();
        int n = hasAlpha ? 4 : 3;

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

                int idx = (y * width + x) * n;
                samples[idx] = r;
                samples[idx + 1] = g;
                samples[idx + 2] = b;
                if (hasAlpha) samples[idx + 3] = a;
            }
        }

        // Create fz_image and add to PDF
        fz_image* fzImage = fz_new_image_from_pixmap(ctx, pix, nullptr);
        pdf_obj* imageObj = pdf_add_image(ctx, pdfDoc, fzImage);
        fz_drop_image(ctx, fzImage);
        fz_drop_pixmap(ctx, pix);

        // Add to XObject dictionary
        std::string xobjName = impl_->generateXObjectName(ctx, pdfPage);
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

        // Append content stream: q [w 0 0 h x y] cm /ImN Do Q
        char ops[256];
        std::snprintf(ops, sizeof(ops),
            "q %.4f 0 0 %.4f %.4f %.4f cm /%s Do Q\n",
            static_cast<float>(rect.width()),
            static_cast<float>(rect.height()),
            static_cast<float>(rect.left()),
            static_cast<float>(rect.top()),
            xobjName.c_str());

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
        result.imageInfo.boundingBox = rect;
    });

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
    // Removing an image orphans its (potentially sensitive) XObject stream;
    // force a full-rewrite save so it is scrubbed (pipeline b2e47d91, Tier 1).
    doc->markRequiresFullRewrite();

    auto images = enumerateImages(page, doc);
    if (imageIndex < 0 || imageIndex >= static_cast<int>(images.size())) {
        result.errorMessage = "Invalid image index";
        return result;
    }

    result.imageInfo = images[imageIndex];
    std::string xobjName = images[imageIndex].xobjectName;

    impl_->context->execute([&](fz_context* ctx) {
        pdf_page* pdfPage = impl_->getPdfPage(page);
        pdf_document* pdfDoc = impl_->getPdfDoc(doc);

        // Remove from content stream via filter
        impl_->removeViaFilter(ctx, pdfDoc, pdfPage, xobjName);

        // Remove from XObject dictionary
        pdf_obj* resources = pdf_page_resources(ctx, pdfPage);
        pdf_obj* xobjectDict = pdf_dict_get(ctx, resources, PDF_NAME(XObject));
        if (xobjectDict) {
            pdf_dict_dels(ctx, xobjectDict, xobjName.c_str());
        }

        result.success = true;
    });

    return result;
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
                pdf_obj* live = pdf_graft_object(ctx, livePdfDoc, h->obj);
                pdf_dict_put(ctx, pdfPage->obj, PDF_NAME(Contents), live);
                pdf_drop_obj(ctx, live);
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
        fz_image* fzImage = pdf_load_image(ctx, pdfDoc, xobj);
        if (!fzImage) return;

        fz_pixmap* pix = fz_get_pixmap_from_image(ctx, fzImage, nullptr, nullptr, nullptr, nullptr);
        fz_drop_image(ctx, fzImage);
        if (!pix) return;

        int w = pix->w;
        int h = pix->h;
        int n = pix->n;
        unsigned char* samples = pix->samples;

        QImage::Format fmt;
        if (n == 1) fmt = QImage::Format_Grayscale8;
        else if (n == 3) fmt = QImage::Format_RGB888;
        else if (n == 4) fmt = QImage::Format_RGBA8888;
        else {
            fz_drop_pixmap(ctx, pix);
            return;
        }

        // Copy pixel data to QImage
        result = QImage(w, h, fmt);
        for (int y = 0; y < h; ++y) {
            std::memcpy(result.scanLine(y), samples + y * pix->stride, w * n);
        }

        fz_drop_pixmap(ctx, pix);
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
        pdf_obj* contents = pdf_dict_get(ctx, pdfPage->obj, PDF_NAME(Contents));
        if (contents) {
            ref = makeRetainedObjHolder(ctx, contents);
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
