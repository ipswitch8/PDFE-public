#define _USE_MATH_DEFINES
#include "image/ImageContentEditor.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/PageContentGuard.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <cmath>
#include <cstring>
#include <sstream>
#include <iomanip>

namespace pdfe::image {

// Filter state for removing image from content stream
struct RemoveFilterState {
    std::string targetName;
};

// Callback for pdf_filter_page_contents to remove an image
static fz_image* removeImageCallback(
    fz_context* ctx, void* opaque, fz_matrix ctm,
    const char* name, fz_image* image, fz_rect scissor)
{
    (void)ctx;
    (void)ctm;
    (void)scissor;

    const auto* state = static_cast<const RemoveFilterState*>(opaque);
    if (name && state->targetName == name) {
        // Return nullptr to remove this image from the content stream
        return nullptr;
    }
    // Keep all other images
    return image;
}

// Implementation class
class ImageContentEditor::Impl {
public:
    std::shared_ptr<pdfe::core::MuPdfContext> context;

    explicit Impl(std::shared_ptr<pdfe::core::MuPdfContext> ctx)
        : context(std::move(ctx))
    {
    }

    // Helper to get pdf_page from Page
    pdf_page* getPdfPage(pdfe::core::Page* page) const {
        if (!page) return nullptr;
        fz_page* fzPage = page->fzPage();
        if (!fzPage) return nullptr;
        return pdf_page_from_fz_page(context->get(), fzPage);
    }

    // Helper to get pdf_document from Document
    pdf_document* getPdfDoc(pdfe::core::Document* doc) const {
        if (!doc) return nullptr;
        fz_document* fzDoc = doc->fzDocument();
        if (!fzDoc) return nullptr;
        return pdf_document_from_fz_document(context->get(), fzDoc);
    }

    // Remove an image from the page's content stream
    void removeFromContentStream(pdf_document* pdfDoc, pdf_page* pdfPage,
                                  const std::string& xobjectName)
    {
        RemoveFilterState state;
        state.targetName = xobjectName;

        fz_context* ctx = context->get();

        // Save the XObject reference before filtering — pdf_filter_page_contents
        // may clean up unused resources, removing the XObject we still need.
        pdf_obj* resources = pdf_page_resources(ctx, pdfPage);
        pdf_obj* xobjects = resources ? pdf_dict_get(ctx, resources, PDF_NAME(XObject)) : nullptr;
        pdf_obj* savedXObj = nullptr;
        if (xobjects) {
            savedXObj = pdf_dict_gets(ctx, xobjects, xobjectName.c_str());
            if (savedXObj) {
                pdf_keep_obj(ctx, savedXObj);
            }
        }

        // Setup sanitize filter to remove the target image
        pdf_sanitize_filter_options sopts;
        std::memset(&sopts, 0, sizeof(sopts));
        sopts.opaque = &state;
        sopts.image_filter = removeImageCallback;

        // Setup filter factory
        pdf_filter_factory factories[2];
        std::memset(factories, 0, sizeof(factories));
        factories[0].filter = pdf_new_sanitize_filter;
        factories[0].options = &sopts;
        factories[1].filter = nullptr;

        // Setup filter options
        pdf_filter_options opts;
        std::memset(&opts, 0, sizeof(opts));
        opts.filters = factories;
        opts.newlines = 1;

        // Apply filter to remove the image
        pdf_filter_page_contents(ctx, pdfDoc, pdfPage, &opts);

        // Restore the XObject reference if it was removed during filtering
        if (savedXObj) {
            resources = pdf_page_resources(ctx, pdfPage);
            xobjects = resources ? pdf_dict_get(ctx, resources, PDF_NAME(XObject)) : nullptr;
            if (!xobjects) {
                xobjects = pdf_new_dict(ctx, pdfDoc, 4);
                pdf_dict_put(ctx, resources, PDF_NAME(XObject), xobjects);
            }
            pdf_dict_puts(ctx, xobjects, xobjectName.c_str(), savedXObj);
            pdf_drop_obj(ctx, savedXObj);
        }
    }

    // Append a new content stream to the page, isolating the fragment's
    // absolute `cm` from any ambient CTM the page left active (image
    // insert/resize/rotate/crop all place via absolute matrices).
    void appendContentStream(pdf_document* pdfDoc, pdf_page* pdfPage,
                            const char* ops, size_t len)
    {
        fz_context* ctx = context->get();
        pdfe::core::appendContentIsolatingAmbientCtm(
            ctx, pdfDoc, pdfPage->obj, ops, len);
    }
};

// Constructor
ImageContentEditor::ImageContentEditor(std::shared_ptr<pdfe::core::MuPdfContext> context)
    : impl_(std::make_unique<Impl>(std::move(context)))
{
}

// Destructor
ImageContentEditor::~ImageContentEditor() = default;

// Resize image
bool ImageContentEditor::resizeImage(
    pdfe::core::Page* page, pdfe::core::Document* doc,
    const std::string& xobjectName,
    float newWidth, float newHeight,
    float x, float y)
{
    if (!page || !doc || xobjectName.empty()) {
        return false;
    }
    // Editing an image rewrites the page content stream (removeFromContentStream
    // orphans the old placement); force a full-rewrite save (pipeline b2e47d91,
    // Tier 1).
    doc->markRequiresFullRewrite();

    bool success = false;

    impl_->context->execute([&](fz_context* ctx) {
        pdf_page* pdfPage = impl_->getPdfPage(page);
        pdf_document* pdfDoc = impl_->getPdfDoc(doc);

        if (!pdfPage || !pdfDoc) {
            return;
        }

        // Remove the old image placement
        impl_->removeFromContentStream(pdfDoc, pdfPage, xobjectName);

        // Build new content stream with resized CTM
        // Format: q [newW 0 0 newH x y] cm /ImN Do Q\n
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(4);
        oss << "q ";
        oss << newWidth << " 0 0 " << newHeight << " " << x << " " << y << " cm ";
        oss << "/" << xobjectName << " Do ";
        oss << "Q\n";

        std::string ops = oss.str();

        // Append the new content stream
        impl_->appendContentStream(pdfDoc, pdfPage, ops.c_str(), ops.length());

        success = true;
    });

    return success;
}

// Rotate image
bool ImageContentEditor::rotateImage(
    pdfe::core::Page* page, pdfe::core::Document* doc,
    const std::string& xobjectName,
    float angleDegrees,
    float centerX, float centerY,
    float displayWidth, float displayHeight)
{
    if (!page || !doc || xobjectName.empty()) {
        return false;
    }
    // Editing an image rewrites the page content stream (removeFromContentStream
    // orphans the old placement); force a full-rewrite save (pipeline b2e47d91,
    // Tier 1).
    doc->markRequiresFullRewrite();

    bool success = false;

    impl_->context->execute([&](fz_context* ctx) {
        pdf_page* pdfPage = impl_->getPdfPage(page);
        pdf_document* pdfDoc = impl_->getPdfDoc(doc);

        if (!pdfPage || !pdfDoc) {
            return;
        }

        // Remove the old image placement
        impl_->removeFromContentStream(pdfDoc, pdfPage, xobjectName);

        // Convert to radians (negative for clockwise rotation in PDF coordinates)
        float radians = -angleDegrees * static_cast<float>(M_PI) / 180.0f;
        float cosTheta = std::cos(radians);
        float sinTheta = std::sin(radians);

        // Build new content stream with rotation
        // Pattern: translate to center, rotate, translate back, apply size and position
        // q 1 0 0 1 cx cy cm [cos sin -sin cos 0 0] cm 1 0 0 1 -cx -cy cm [w 0 0 h x y] cm /ImN Do Q\n
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(4);
        oss << "q ";

        // Translate to center
        oss << "1 0 0 1 " << centerX << " " << centerY << " cm ";

        // Rotate
        oss << cosTheta << " " << sinTheta << " " << (-sinTheta) << " " << cosTheta << " 0 0 cm ";

        // Translate back
        oss << "1 0 0 1 " << (-centerX) << " " << (-centerY) << " cm ";

        // Apply size and position (position relative to rotation center)
        float offsetX = centerX - displayWidth / 2.0f;
        float offsetY = centerY - displayHeight / 2.0f;
        oss << displayWidth << " 0 0 " << displayHeight << " " << offsetX << " " << offsetY << " cm ";

        // Draw image
        oss << "/" << xobjectName << " Do ";
        oss << "Q\n";

        std::string ops = oss.str();

        // Append the new content stream
        impl_->appendContentStream(pdfDoc, pdfPage, ops.c_str(), ops.length());

        success = true;
    });

    return success;
}

// Crop image
bool ImageContentEditor::cropImage(
    pdfe::core::Page* page, pdfe::core::Document* doc,
    const std::string& xobjectName,
    QRectF cropRect,
    float displayWidth, float displayHeight,
    float x, float y)
{
    if (!page || !doc || xobjectName.empty()) {
        return false;
    }
    // Editing an image rewrites the page content stream (removeFromContentStream
    // orphans the old placement); force a full-rewrite save (pipeline b2e47d91,
    // Tier 1).
    doc->markRequiresFullRewrite();

    bool success = false;

    impl_->context->execute([&](fz_context* ctx) {
        pdf_page* pdfPage = impl_->getPdfPage(page);
        pdf_document* pdfDoc = impl_->getPdfDoc(doc);

        if (!pdfPage || !pdfDoc) {
            return;
        }

        // Remove the old image placement
        impl_->removeFromContentStream(pdfDoc, pdfPage, xobjectName);

        // Build new content stream with clipping path
        // Format: q [cropX cropY cropW cropH] re W n [w 0 0 h x y] cm /ImN Do Q\n
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(4);
        oss << "q ";

        // Define clipping rectangle
        oss << cropRect.x() << " " << cropRect.y() << " ";
        oss << cropRect.width() << " " << cropRect.height() << " re ";

        // Set clipping path (W = clip, n = end path without fill/stroke)
        oss << "W n ";

        // Apply image transformation
        oss << displayWidth << " 0 0 " << displayHeight << " " << x << " " << y << " cm ";

        // Draw image
        oss << "/" << xobjectName << " Do ";
        oss << "Q\n";

        std::string ops = oss.str();

        // Append the new content stream
        impl_->appendContentStream(pdfDoc, pdfPage, ops.c_str(), ops.length());

        success = true;
    });

    return success;
}

} // namespace pdfe::image
