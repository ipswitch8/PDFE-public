#include "PdfContentWriter.h"
#include "TreTypes.h"
#include "LanczosResize.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/Document.h"
#include "core/ContentStreamPreamble.h"
#include "core/StextOptions.h"
#include "core/PageContentGuard.h"
#include "core/RetainedObjHolder.h"
#include "fonts/PdfFontEmbedder.h"
#include "image/ImagePlacement.h"
#include "image/ImagePlacementScanner.h"

#include <QBuffer>
#include <QDebug>
#include <QImage>
#include <QPainter>
#include <QTransform>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <vector>

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

namespace pdfe::tre {

// ---------------------------------------------------------------------------
// Draw-order image enumeration (fix-tre-writetarget; GitHub #443, #446)
//
// The write-target bug: resolving a page image back to a resource NAME by
// matching (Width,Height) is ambiguous whenever a resources dict holds several
// identical-dimension image XObjects (the e1139 file: 573 identical 1700x2200
// images, one drawn per page). The unambiguous signal is the content stream's
// own `/Name Do` operator plus the CTM that places it.
//
// The draws come from ImagePlacementScanner, the reader the image editor and
// ImageManager use (docs/image-placement-model.md), so the OCR write-back and
// the image tool agree on every placement: its identity (name + occurrence),
// its full matrix and its frame. The placement is reported in the RENDERED
// page frame (pdf_page_transform: box origin, /Rotate, Y flip), the frame of
// Page::render and of a selection rect. GitHub #443: this used to flip the
// user-space bound about the fz_bound_page height, which ignored the MediaBox
// origin and /Rotate.
// ---------------------------------------------------------------------------
namespace {

using pdfe::image::placement::Affine;

struct DrawnImage {
    std::string name;     // XObject name as written in the content that draws it
    QRectF fitzBbox;      // visible placement, rendered frame (top-left origin, Y-down)
    int w = 0;            // pixel width
    int h = 0;            // pixel height
    int occurrence = -1;  // n-th page-level Do of `name`; -1 inside a form
    bool inForm = false;  // drawn inside a Form XObject
    bool isJpeg = false;  // DCTDecode
    Affine pageCtm;       // unit square -> rendered frame
};

bool imageIsJpeg(fz_context* ctx, pdf_obj* image)
{
    pdf_obj* filter = pdf_dict_get(ctx, image, PDF_NAME(Filter));
    if (pdf_is_array(ctx, filter)) {
        const int n = pdf_array_len(ctx, filter);
        filter = n > 0 ? pdf_array_get(ctx, filter, n - 1) : nullptr;
    }
    return filter && pdf_name_eq(ctx, filter, PDF_NAME(DCTDecode));
}

// Enumerate drawn image XObjects in content-stream (draw) order, Form XObjects
// followed. `out` and `sites` MUST live in the caller's frame (not inside the
// MuPdfContext guarded region): a longjmp out of the scan would otherwise skip
// the std::string / std::vector destructors (GitHub #118 hazard class).
// Inline images are not reported: they carry no resource key to write into.
void enumerateDrawnImages(fz_context* ctx, pdf_page* pdfPage,
                          std::vector<pdfe::image::placement::DoSite>* sites,
                          std::vector<DrawnImage>* out)
{
    namespace pl = pdfe::image::placement;
    *sites = pl::scanPage(ctx, pdfPage, /*recurseForms=*/true);
    const Affine toRendered = pl::pageTransform(ctx, pdfPage);
    for (const auto& site : *sites) {
        if (!site.isImage || !site.xobject || site.name.empty()) continue;
        const Affine m = pl::concat(site.ctm, toRendered);
        const pl::UnitCrop& c = site.crop;
        const QPointF p0 = m.apply(c.u0, c.v0), p1 = m.apply(c.u1, c.v0);
        const QPointF p2 = m.apply(c.u1, c.v1), p3 = m.apply(c.u0, c.v1);
        const double x0 = std::min({p0.x(), p1.x(), p2.x(), p3.x()});
        const double x1 = std::max({p0.x(), p1.x(), p2.x(), p3.x()});
        const double y0 = std::min({p0.y(), p1.y(), p2.y(), p3.y()});
        const double y1 = std::max({p0.y(), p1.y(), p2.y(), p3.y()});
        out->emplace_back();
        DrawnImage& d = out->back();
        d.name = site.name;
        d.fitzBbox = QRectF(x0, y0, x1 - x0, y1 - y0);
        d.w = pdf_dict_get_int(ctx, site.xobject, PDF_NAME(Width));
        d.h = pdf_dict_get_int(ctx, site.xobject, PDF_NAME(Height));
        d.inForm = site.formDepth > 0;
        d.occurrence = d.inForm ? -1 : site.occurrence;
        d.isJpeg = imageIsJpeg(ctx, site.xobject);
        d.pageCtm = m;
    }
}

void fillInfo(const DrawnImage& d, PageImageInfo* info, bool withPlacement)
{
    info->xobjectName = d.name;
    info->width = d.w;
    info->height = d.h;
    info->isJpeg = d.isJpeg;
    info->occurrence = d.occurrence;
    info->inFormXObject = d.inForm;
    info->hasPageCtm = true;
    info->pageCtm = {d.pageCtm.a, d.pageCtm.b, d.pageCtm.c,
                     d.pageCtm.d, d.pageCtm.e, d.pageCtm.f};
    if (withPlacement) info->placementFitz = d.fitzBbox;
}

}  // namespace

// ---------------------------------------------------------------------------
// PIMPL Implementation
// ---------------------------------------------------------------------------

class PdfContentWriter::Impl {
public:
    pdfe::core::MuPdfContext* context_ = nullptr;
    pdfe::fonts::PdfFontEmbedder* fontEmbedder_ = nullptr;

    Impl(pdfe::core::MuPdfContext* context, pdfe::fonts::PdfFontEmbedder* fontEmbedder)
        : context_(context), fontEmbedder_(fontEmbedder)
    {}
};

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

PdfContentWriter::PdfContentWriter(pdfe::core::MuPdfContext* context,
                                   pdfe::fonts::PdfFontEmbedder* fontEmbedder)
    : pImpl_(std::make_unique<Impl>(context, fontEmbedder))
{
}

PdfContentWriter::~PdfContentWriter() = default;

PdfContentWriter::PdfContentWriter(PdfContentWriter&&) noexcept = default;
PdfContentWriter& PdfContentWriter::operator=(PdfContentWriter&&) noexcept = default;

// ---------------------------------------------------------------------------
// findPageImageXObject
// ---------------------------------------------------------------------------

std::optional<PageImageInfo> PdfContentWriter::findPageImageXObject(
    pdfe::core::Page* page, pdfe::core::Document* document)
{
    if (!page || !document || !pImpl_->context_) {
        return std::nullopt;
    }

    // fix-tre-writetarget (Finding 2): the historical rule was "largest image
    // XObject by pixel area", with a strict `>` that kept the FIRST of any tie.
    // On a resources dict full of identical-dimension images (the e1139 file,
    // 573 identical 1700x2200 XObjects) every image ties on pixel area, so the
    // pick was an arbitrary dictionary-order one that need not be the image the
    // page actually draws. Resolve instead against the images the page DRAWS,
    // choosing the largest by DISPLAYED area; only one of the identical XObjects
    // is drawn per page, so the tie is gone. placementFitz is deliberately NOT
    // recorded here (callers read an empty one as "the page's main image"); the
    // identity (occurrence) and the full matrix (pageCtm) are, so the two-
    // argument replacePageImage() can tell whether the image really spans the
    // page (GitHub #447). Only the page's OWN draws count: a form's image is not
    // a resource of this page and is never written. If the page draws no named
    // image (e.g. images nested only inside a Form XObject), fall back to the
    // old resources-dict largest-pixel-area walk.
    std::vector<pdfe::image::placement::DoSite> sites;
    std::vector<DrawnImage> draws;
    PageImageInfo info;
    bool found = false;

    try {
        pImpl_->context_->execute([&](fz_context* ctx) {
            fz_page* fzPage = page->fzPage();
            fz_document* fzDoc = document->fzDocument();
            pdf_page* pdfPage = pdf_page_from_fz_page(ctx, fzPage);
            pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, fzDoc);
            if (!pdfPage || !pdfDoc) return;

            enumerateDrawnImages(ctx, pdfPage, &sites, &draws);

            double bestDisplayArea = -1.0;
            for (const auto& d : draws) {
                if (d.inForm) continue;
                const double area = d.fitzBbox.width() * d.fitzBbox.height();
                if (area > bestDisplayArea) {
                    bestDisplayArea = area;
                    fillInfo(d, &info, /*withPlacement=*/false);
                    found = true;
                }
            }
            if (found) return;

            // Fallback: no named image draw found. Old behaviour — largest image
            // XObject in the resources dict by pixel area, first on tie.
            pdf_obj* resources = pdf_page_resources(ctx, pdfPage);
            if (!resources) return;
            pdf_obj* xobjectDict = pdf_dict_get(ctx, resources, PDF_NAME(XObject));
            if (!xobjectDict) return;
            int bestArea = 0;
            const int len = pdf_dict_len(ctx, xobjectDict);
            for (int i = 0; i < len; ++i) {
                pdf_obj* key = pdf_dict_get_key(ctx, xobjectDict, i);
                pdf_obj* val =
                    pdf_resolve_indirect(ctx, pdf_dict_get_val(ctx, xobjectDict, i));
                if (!val) continue;
                pdf_obj* subtype = pdf_dict_get(ctx, val, PDF_NAME(Subtype));
                if (!pdf_name_eq(ctx, subtype, PDF_NAME(Image))) continue;
                const int w = pdf_dict_get_int(ctx, val, PDF_NAME(Width));
                const int h = pdf_dict_get_int(ctx, val, PDF_NAME(Height));
                const int area = w * h;
                if (area > bestArea) {
                    bestArea = area;
                    info.xobjectName = pdf_to_name(ctx, key);
                    info.width = w;
                    info.height = h;
                    pdf_obj* filter = pdf_dict_get(ctx, val, PDF_NAME(Filter));
                    info.isJpeg = filter && pdf_name_eq(ctx, filter, PDF_NAME(DCTDecode));
                    found = true;
                }
            }
        });
    } catch (const std::exception& e) {
        qWarning("PdfContentWriter::findPageImageXObject: %s", e.what());
        return std::nullopt;
    } catch (...) {
        qWarning("PdfContentWriter::findPageImageXObject: unknown exception");
        return std::nullopt;
    }

    if (!found) return std::nullopt;
    return info;
}

// ---------------------------------------------------------------------------
// findImageXObjectForRegion  (GitHub #262)
// ---------------------------------------------------------------------------

std::optional<PageImageInfo> PdfContentWriter::findImageXObjectForRegion(
    pdfe::core::Page* page, pdfe::core::Document* document,
    const QRectF& regionFitz)
{
    if (!page || !document || !pImpl_->context_) {
        return std::nullopt;
    }

    // The resolved target is hoisted to THIS frame (not the guarded region):
    // PageImageInfo owns a std::string, and a longjmp out of the MuPDF region
    // would skip its destructor (GitHub #118 hazard class). The draw list is
    // owned here too so the enumerated names unwind safely.
    //
    // fix-tre-writetarget: resolve the region to the XObject NAME the page
    // actually DRAWS, via the content stream's `/Name Do` operators and their
    // CTM placement (enumerateDrawnImages), NOT by matching pixel (Width,Height)
    // against the resources dict. The old dimension match was ambiguous whenever
    // two image resources shared a size and bailed to nullopt even when the page
    // drew them at different, unambiguous placements (the e1139 bug: 573
    // identical 1700x2200 XObjects, one drawn per page).
    std::vector<pdfe::image::placement::DoSite> sites;
    std::vector<DrawnImage> draws;
    PageImageInfo info;
    bool resolved = false;

    try {
        pImpl_->context_->execute([&](fz_context* ctx) {
            fz_page* fzPage = page->fzPage();
            fz_document* fzDoc = document->fzDocument();
            pdf_page* pdfPage = pdf_page_from_fz_page(ctx, fzPage);
            pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, fzDoc);
            if (!pdfPage || !pdfDoc) return;

            enumerateDrawnImages(ctx, pdfPage, &sites, &draws);
            if (draws.empty()) return;

            const double regionArea = std::max(0.0, regionFitz.width()) *
                                      std::max(0.0, regionFitz.height());
            const QPointF regionCenter = regionFitz.center();

            // Among the drawn images covering the region, choose the one covering
            // the largest FRACTION of it; ties go to the LATER draw, which is the
            // image on top (the same rule as Page::sourceImageDpiAt(), expressed
            // over draw order rather than bbox tightness).
            int bestIdx = -1;
            double bestFrac = 0.0;
            for (int i = 0; i < static_cast<int>(draws.size()); ++i) {
                const QRectF& bb = draws[i].fitzBbox;
                if (bb.width() <= 0.0 || bb.height() <= 0.0) continue;
                const QRectF isect = bb.intersected(regionFitz);
                const double overlap = (isect.isValid() && !isect.isEmpty())
                                           ? isect.width() * isect.height()
                                           : 0.0;
                const double frac =
                    regionArea > 0.0 ? overlap / regionArea
                                     : (bb.contains(regionCenter) ? 1.0 : 0.0);
                if (frac <= 0.0) continue;
                if (bestIdx < 0 || frac > bestFrac + 1e-9) {
                    bestFrac = frac;
                    bestIdx = i;              // strictly better coverage
                } else if (frac > bestFrac - 1e-9) {
                    bestIdx = i;              // tie -> prefer the later (topmost) draw
                }
            }
            if (bestIdx < 0) return;          // nothing covers the region

            // Ambiguity guard, preserved from the old Step 2 but narrowed to the
            // case draw order genuinely cannot resolve: another covering draw is
            // a DIFFERENT resource occupying the SAME placement (two images
            // stacked exactly). Then which XObject owns the visible content is
            // unknowable without rendering, so report nothing rather than guess
            // -- the caller falls back and the occlusion guard catches an
            // invisible write. Different placements are no longer ambiguous.
            const QRectF chosen = draws[bestIdx].fitzBbox;
            const double tol = 1.0;
            for (int i = 0; i < static_cast<int>(draws.size()); ++i) {
                if (i == bestIdx) continue;
                if (draws[i].name == draws[bestIdx].name &&
                    draws[i].inForm == draws[bestIdx].inForm) continue;
                const QRectF& bb = draws[i].fitzBbox;
                const QRectF isect = bb.intersected(regionFitz);
                const bool covers = (isect.isValid() && !isect.isEmpty()) ||
                                    (regionArea <= 0.0 && bb.contains(regionCenter));
                if (!covers) continue;
                if (std::abs(bb.x() - chosen.x()) <= tol &&
                    std::abs(bb.y() - chosen.y()) <= tol &&
                    std::abs(bb.width() - chosen.width()) <= tol &&
                    std::abs(bb.height() - chosen.height()) <= tol) {
                    return;                   // same-placement ambiguity -> nullopt
                }
            }

            fillInfo(draws[bestIdx], &info, /*withPlacement=*/true);
            resolved = true;
        });
    } catch (const std::exception& e) {
        qWarning("PdfContentWriter::findImageXObjectForRegion: %s", e.what());
        return std::nullopt;
    } catch (...) {
        qWarning("PdfContentWriter::findImageXObjectForRegion: unknown exception");
        return std::nullopt;
    }

    if (!resolved) return std::nullopt;
    return info;
}

// ---------------------------------------------------------------------------
// topmostImageOverRegion  (GitHub #262)
// ---------------------------------------------------------------------------

std::optional<PageImageInfo> PdfContentWriter::topmostImageOverRegion(
    pdfe::core::Page* page, pdfe::core::Document* document,
    const QRectF& regionFitz)
{
    if (!page || !document || !pImpl_->context_) {
        return std::nullopt;
    }

    // Hoisted to THIS frame for std::string safety under a MuPDF longjmp (as in
    // findImageXObjectForRegion). topmostImageOverRegion now ALSO records the
    // drawn XObject NAME (via enumerateDrawnImages), so the occlusion check can
    // compare identity by name rather than by dimensions — which are ambiguous
    // on a page carrying several same-sized images.
    std::vector<pdfe::image::placement::DoSite> sites;
    std::vector<DrawnImage> draws;
    PageImageInfo top;
    bool hit = false;

    try {
        pImpl_->context_->execute([&](fz_context* ctx) {
            fz_page* fzPage = page->fzPage();
            fz_document* fzDoc = document->fzDocument();
            pdf_page* pdfPage = pdf_page_from_fz_page(ctx, fzPage);
            pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, fzDoc);
            if (!pdfPage || !pdfDoc) return;

            enumerateDrawnImages(ctx, pdfPage, &sites, &draws);

            // Draws are in content-stream order, so the LAST image covering the
            // region centre is the one drawn on top of it.
            const QPointF centre = regionFitz.center();
            for (const auto& d : draws) {
                if (!d.fitzBbox.contains(centre)) continue;
                hit = true;
                fillInfo(d, &top, /*withPlacement=*/true);
            }
        });
        if (!hit) {
            return std::nullopt;
        }
        return top;
    } catch (const std::exception& e) {
        qWarning("PdfContentWriter::topmostImageOverRegion: %s", e.what());
        return std::nullopt;
    } catch (...) {
        qWarning("PdfContentWriter::topmostImageOverRegion: unknown exception");
        return std::nullopt;
    }
}

// ---------------------------------------------------------------------------
// replacePageImage
// ---------------------------------------------------------------------------

namespace {

// The placement matrix as a QTransform (both row-vector: x' = a x + c y + e).
QTransform toQTransform(const std::array<double, 6>& m)
{
    return QTransform(m[0], m[1], m[2], m[3], m[4], m[5]);
}

// Upright and unflipped in the rendered frame: the image's own x axis runs
// right and its rows run down the page (unit v = 1 is the image's top row).
bool isUprightUnflipped(const std::array<double, 6>& m)
{
    const double scale = std::max({std::abs(m[0]), std::abs(m[3]), 1e-9});
    return m[0] > 0.0 && m[3] < 0.0 &&
           std::abs(m[1]) <= 1e-6 * scale && std::abs(m[2]) <= 1e-6 * scale;
}

// Whether the main image IS the page: upright, unflipped, and covering the
// rendered page to within a point (or 0.5%) on every edge. Only then is a
// whole-page picture the image's own pixels (GitHub #447).
bool coversThePageUpright(const PageImageInfo& info, const QRectF& pageRect)
{
    if (!info.hasPageCtm || !isUprightUnflipped(info.pageCtm) || pageRect.isEmpty())
        return false;
    const QRectF placed = toQTransform(info.pageCtm).mapRect(QRectF(0, 0, 1, 1));
    const double tolX = std::max(1.0, pageRect.width() * 0.005);
    const double tolY = std::max(1.0, pageRect.height() * 0.005);
    return std::abs(placed.left() - pageRect.left()) <= tolX &&
           std::abs(placed.right() - pageRect.right()) <= tolX &&
           std::abs(placed.top() - pageRect.top()) <= tolY &&
           std::abs(placed.bottom() - pageRect.bottom()) <= tolY;
}

// GitHub #445: carry the image dictionary's non-pixel state over to the new
// image. The new samples are DECODED colours (what MuPDF draws), so /Decode is
// not copied. Everything copied here depends only on the colours, not on how
// the old samples were stored, except the colour-key /Mask array, which names
// sample values: it is kept only for 8-bit, 3-component samples without
// /Decode, which the new image's samples equal.
void copyImageState(fz_context* ctx, pdf_obj* oldImage, pdf_obj* newImage)
{
    if (!oldImage || !newImage) return;
    for (pdf_obj* key : {PDF_NAME(SMask), PDF_NAME(OC), PDF_NAME(Intent),
                         PDF_NAME(Interpolate)}) {
        if (pdf_obj* v = pdf_dict_get(ctx, oldImage, key)) {
            pdf_dict_put(ctx, newImage, key, v);
        }
    }

    // An ICCBased colour space with three components describes RGB samples:
    // the new ones are those same numbers, so the profile still applies.
    pdf_obj* cs = pdf_dict_get(ctx, oldImage, PDF_NAME(ColorSpace));
    int components = 0;
    if (pdf_name_eq(ctx, cs, PDF_NAME(DeviceRGB))) {
        components = 3;
    } else if (pdf_is_array(ctx, cs) &&
               pdf_name_eq(ctx, pdf_array_get(ctx, cs, 0), PDF_NAME(ICCBased))) {
        components = pdf_dict_get_int(ctx, pdf_array_get(ctx, cs, 1), PDF_NAME(N));
        if (components == 3) {
            pdf_dict_put(ctx, newImage, PDF_NAME(ColorSpace), cs);
        }
    }

    pdf_obj* mask = pdf_dict_get(ctx, oldImage, PDF_NAME(Mask));
    if (pdf_is_stream(ctx, mask)) {
        pdf_dict_put(ctx, newImage, PDF_NAME(Mask), mask);
    } else if (pdf_is_array(ctx, mask) && components == 3 &&
               pdf_dict_get_int(ctx, oldImage, PDF_NAME(BitsPerComponent)) == 8 &&
               !pdf_dict_get(ctx, oldImage, PDF_NAME(Decode))) {
        pdf_dict_put(ctx, newImage, PDF_NAME(Mask), mask);
    }
}

// Whether the page's /XObject dictionary is also another page's: then an entry
// changed in it changes that page too.
bool xobjectDictSharedWithAnotherPage(fz_context* ctx, pdf_document* doc,
                                      pdf_obj* pageObj, pdf_obj* xobjectDict)
{
    // O(pages): one resources lookup per other page, stopping at the first
    // page that shares the dictionary. The caller asks only when nothing else
    // on this page uses the entry (otherwise the answer cannot change the
    // outcome), and this page's own object is resolved once, not per page.
    pdf_obj* mine = pdf_resolve_indirect(ctx, xobjectDict);
    pdf_obj* self = pdf_resolve_indirect(ctx, pageObj);
    const int n = pdf_count_pages(ctx, doc);
    for (int i = 0; i < n; ++i) {
        pdf_obj* other = pdf_lookup_page_obj(ctx, doc, i);
        if (!other || pdf_resolve_indirect(ctx, other) == self) continue;
        pdf_obj* res = pdf_dict_get_inheritable(ctx, other, PDF_NAME(Resources));
        pdf_obj* xo = pdf_dict_get(ctx, res, PDF_NAME(XObject));
        if (xo && pdf_resolve_indirect(ctx, xo) == mine) return true;  // first hit
    }
    return false;
}

}  // namespace

bool PdfContentWriter::renderedFrameToImagePixels(const PageImageInfo& target,
                                                  QTransform* out)
{
    if (!out || !target.hasPageCtm || target.width <= 0 || target.height <= 0) return false;
    bool invertible = false;
    const QTransform toUnit = toQTransform(target.pageCtm).inverted(&invertible);
    if (!invertible) return false;
    // Unit square -> image pixels: u -> u * W; v = 1 is the image's top row.
    const QTransform unitToPixels(target.width, 0.0, 0.0, -target.height, 0.0, target.height);
    *out = toUnit * unitToPixels;
    return true;
}

bool PdfContentWriter::replacePageImage(
    pdfe::core::Page* page, pdfe::core::Document* document,
    const QImage& newImage)
{
    if (!page || !document || !pImpl_->context_ || newImage.isNull()) {
        return false;
    }

    // Find existing image
    auto imageInfo = findPageImageXObject(page, document);
    if (!imageInfo.has_value()) {
        return false;
    }

    // GitHub #447. `newImage` is a picture of the whole page. Only when the main
    // image is the page (upright, covering it) are the two the same pixels;
    // the historical path below then stays exactly as it was. Otherwise the
    // part of the picture under the image is taken through the full placement
    // matrix into the image's own pixel grid, on top of its current pixels.
    const QRectF pageRect = page->boundingBox();
    if (!imageInfo->hasPageCtm || coversThePageUpright(*imageInfo, pageRect)) {
        return replacePageImage(page, document, newImage, *imageInfo);
    }

    QTransform renderedToPixels;
    if (pageRect.isEmpty() || !renderedFrameToImagePixels(*imageInfo, &renderedToPixels)) {
        return false;
    }
    const QImage current = getOriginalImage(page, document, *imageInfo);
    if (current.isNull()) {
        return false;
    }
    QImage canvas = current.convertToFormat(QImage::Format_RGB32);
    const QTransform pictureToRendered =
        QTransform::fromScale(pageRect.width() / newImage.width(),
                              pageRect.height() / newImage.height()) *
        QTransform::fromTranslate(pageRect.x(), pageRect.y());
    {
        QPainter painter(&canvas);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.setTransform(pictureToRendered * renderedToPixels);
        painter.drawImage(QPointF(0.0, 0.0), newImage);
    }
    return replacePageImage(page, document, canvas, *imageInfo);
}

bool PdfContentWriter::replacePageImage(
    pdfe::core::Page* page, pdfe::core::Document* document,
    const QImage& newImage, const PageImageInfo& target)
{
    if (!page || !document || !pImpl_->context_ || newImage.isNull()) {
        return false;
    }
    if (target.xobjectName.empty()) {
        return false;
    }
    // The phase-2 rule of the placement model: an image inside a Form XObject is
    // shown and selectable, never written (the form may be drawn elsewhere).
    if (target.inFormXObject) {
        qWarning("PdfContentWriter::replacePageImage: '%s' is drawn inside a Form "
                 "XObject, which may be shared; it is not written",
                 target.xobjectName.c_str());
        return false;
    }
    const PageImageInfo* imageInfo = &target;

    // Convert to RGB if needed
    QImage rgbImage = newImage;
    if (rgbImage.format() != QImage::Format_RGB888) {
        rgbImage = rgbImage.convertToFormat(QImage::Format_RGB888);
    }

    // Scale to match original XObject dimensions to preserve resolution.
    // Only scale when the input is at a reasonable resolution (>=50% of original
    // in both dimensions). This ensures the ImageReplacer pipeline (which renders
    // at 300 DPI) gets scaled up to match a 600 DPI original, but low-res
    // captures (72 DPI page renders used for undo/redo) are NOT upscaled to
    // avoid blurry artifacts.
    if (imageInfo->width > 0 && imageInfo->height > 0 &&
        rgbImage.width() >= imageInfo->width / 2 &&
        rgbImage.height() >= imageInfo->height / 2 &&
        (rgbImage.width() != imageInfo->width || rgbImage.height() != imageInfo->height)) {
        // C1: Lanczos-3 for >=1.5x rescales (noticeably sharper than
        // bilinear for text); bilinear for small rescales.
        rgbImage = LanczosResize::resizeBest(
            rgbImage, QSize(imageInfo->width, imageInfo->height));
    }

    // Owned by THIS frame, not the guarded region: DoSite holds a std::string
    // and the vector owns them (GitHub #118 hazard class).
    std::vector<pdfe::image::placement::DoSite> sites;
    pdfe::image::placement::DoSite repointed;
    const std::string& name = imageInfo->xobjectName;
    const QRectF wantedBox = imageInfo->placementFitz;
    const int wantedOccurrence = imageInfo->occurrence;
    const int rowBytes = rgbImage.width() * 3;
    const int imageW = rgbImage.width();
    const int imageH = rgbImage.height();

    try {
        pImpl_->context_->execute([&](fz_context* ctx) {
            namespace pl = pdfe::image::placement;
            fz_page* fzPage = page->fzPage();
            fz_document* fzDoc = document->fzDocument();
            pdf_page* pdfPage = pdf_page_from_fz_page(ctx, fzPage);
            pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, fzDoc);

            if (!pdfPage || !pdfDoc) {
                fz_throw(ctx, FZ_ERROR_GENERIC, "Invalid page or document");
            }

            pdf_obj* resources = pdf_page_resources(ctx, pdfPage);
            if (!resources) {
                fz_throw(ctx, FZ_ERROR_GENERIC, "Failed to get page resources");
            }
            pdf_obj* xobjectDict = pdf_dict_get(ctx, resources, PDF_NAME(XObject));
            if (!xobjectDict) {
                fz_throw(ctx, FZ_ERROR_GENERIC, "No XObject dictionary found");
            }
            pdf_obj* oldImage = pdf_dict_gets(ctx, xobjectDict, name.c_str());

            // GitHub #446: which Do is the target, and does anything else use
            // the entry it draws? Every other Do that looks the name up in the
            // page's own resource dict (a page-level Do, or one inside a form
            // that inherits the page's resources) would change with it, and so
            // would every page sharing the /XObject dict.
            sites = pl::scanPage(ctx, pdfPage, /*recurseForms=*/true);
            const pl::Affine toRendered = pl::pageTransform(ctx, pdfPage);
            const pl::DoSite* site = nullptr;
            int uses = 0;
            int pageLevel = 0;
            const pl::DoSite* onlyPageLevel = nullptr;
            for (const auto& s : sites) {
                if (s.name != name) continue;
                if (s.formDepth == 0 || s.resources == resources) ++uses;
                if (s.formDepth != 0 || !s.isImage) continue;
                ++pageLevel;
                onlyPageLevel = &s;
                if (wantedOccurrence >= 0) {
                    if (s.occurrence == wantedOccurrence) site = &s;
                } else if (!wantedBox.isEmpty() && !site) {
                    const pl::Affine m = pl::concat(s.ctm, toRendered);
                    const QPointF p0 = m.apply(s.crop.u0, s.crop.v0);
                    const QPointF p2 = m.apply(s.crop.u1, s.crop.v1);
                    const QPointF p1 = m.apply(s.crop.u1, s.crop.v0);
                    const QPointF p3 = m.apply(s.crop.u0, s.crop.v1);
                    const double x0 = std::min({p0.x(), p1.x(), p2.x(), p3.x()});
                    const double y0 = std::min({p0.y(), p1.y(), p2.y(), p3.y()});
                    const double x1 = std::max({p0.x(), p1.x(), p2.x(), p3.x()});
                    const double y1 = std::max({p0.y(), p1.y(), p2.y(), p3.y()});
                    if (std::abs(x0 - wantedBox.left()) <= 1.0 &&
                        std::abs(y0 - wantedBox.top()) <= 1.0 &&
                        std::abs(x1 - wantedBox.right()) <= 1.0 &&
                        std::abs(y1 - wantedBox.bottom()) <= 1.0)
                        site = &s;
                }
            }
            if (!site && wantedOccurrence < 0 && wantedBox.isEmpty() && pageLevel == 1) {
                site = onlyPageLevel;
            }
            // The O(pages) sharing check runs only when nothing else on this
            // page uses the entry; otherwise it cannot change the outcome.
            const bool inPlace =
                uses <= 1 &&
                !xobjectDictSharedWithAnotherPage(ctx, pdfDoc, pdfPage->obj, xobjectDict);
            if (!inPlace && !site) {
                fz_throw(ctx, FZ_ERROR_GENERIC,
                         "the targeted placement is not in the page content and the "
                         "image is used elsewhere");
            }

            // The new pixels, as a fresh image object.
            fz_pixmap* pix = nullptr;
            fz_image* fzImage = nullptr;
            pdf_obj* newImageObj = nullptr;
            fz_var(pix);
            fz_var(fzImage);
            fz_var(newImageObj);
            fz_try(ctx) {
                pix = fz_new_pixmap(ctx, fz_device_rgb(ctx), imageW, imageH, nullptr, 0);
                unsigned char* samples = fz_pixmap_samples(ctx, pix);
                const int stride = fz_pixmap_stride(ctx, pix);
                for (int y = 0; y < imageH; ++y) {
                    std::memcpy(samples + static_cast<std::ptrdiff_t>(y) * stride,
                                rgbImage.constScanLine(y), static_cast<size_t>(rowBytes));
                }
                fzImage = fz_new_image_from_pixmap(ctx, pix, nullptr);
                newImageObj = pdf_add_image(ctx, pdfDoc, fzImage);
                copyImageState(ctx, oldImage, newImageObj);

                if (inPlace) {
                    // The sole user of the entry: replace it under its own name
                    // (what undo snapshots and the page's main-image lookup use).
                    pdf_dict_puts(ctx, xobjectDict, name.c_str(), newImageObj);
                } else {
                    // Another placement uses the entry: add the new image under a
                    // fresh name and repoint only the targeted Do, in place: only
                    // its name operand is replaced, so its wrapper, every other
                    // byte of the page content and the drawing order stay.
                    char fresh[32];
                    for (int i = 1;; ++i) {
                        std::snprintf(fresh, sizeof(fresh), "PdfeTreIm%d", i);
                        if (!pdf_dict_gets(ctx, xobjectDict, fresh)) break;
                    }
                    pdf_dict_puts(ctx, xobjectDict, fresh, newImageObj);
                    repointed = *site;
                    repointed.name = fresh;
                    if (!pl::renameSite(ctx, pdfDoc, pdfPage, *site, repointed.name)) {
                        pdf_dict_dels(ctx, xobjectDict, fresh);
                        fz_throw(ctx, FZ_ERROR_GENERIC, "could not repoint the placement");
                    }
                }
            }
            fz_always(ctx) {
                pdf_drop_obj(ctx, newImageObj);
                fz_drop_image(ctx, fzImage);
                fz_drop_pixmap(ctx, pix);
            }
            fz_catch(ctx) {
                fz_rethrow(ctx);
            }
        });

        return true;
    } catch (const std::exception& e) {
        qWarning("PdfContentWriter::replacePageImage: %s", e.what());
        return false;
    } catch (...) {
        qWarning("PdfContentWriter::replacePageImage: unknown exception");
        return false;
    }
}

// utf8ToCodepoints() is now in TreTypes.h/cpp

// ---------------------------------------------------------------------------
// encodeGlyphIndicesHex
// ---------------------------------------------------------------------------
// Resolve every codepoint to a glyph index via the font cmap and emit it as a
// 4-hex-digit CID (the embedded font is Type0/Identity-H, so show-string bytes
// are glyph indices). This is the single correct encoder for the reconstruction
// font; both the normal path and the error fallback route through it so neither
// can ever emit raw codepoints as glyph indices (out-of-class finding #3).
std::string PdfContentWriter::encodeGlyphIndicesHex(
    fz_context* ctx, void* fzFont, const std::vector<uint32_t>& codepoints)
{
    fz_font* font = static_cast<fz_font*>(fzFont);
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (uint32_t cp : codepoints) {
        int gid = 0;
        // Guard each lookup: a codepoint the font cannot encode degrades to
        // GID 0 (.notdef) -- never to the raw codepoint value.
        fz_try(ctx) {
            gid = font ? fz_encode_character(ctx, font, static_cast<int>(cp)) : 0;
        }
        fz_catch(ctx) {
            gid = 0;
        }
        oss << std::setw(4) << (gid & 0xFFFF);
    }
    return oss.str();
}

// ---------------------------------------------------------------------------
// writeTextBlock
// ---------------------------------------------------------------------------

std::string PdfContentWriter::writeTextBlock(
    pdfe::core::Page* page, pdfe::core::Document* document,
    const std::string& utf8Text, const FontMatch& fontMatch,
    const PdfRect& pdfRect, const std::vector<HocrLine>& lines)
{
    if (!page || !document || !pImpl_->context_ || !pImpl_->fontEmbedder_) {
        return "";
    }

    if (utf8Text.empty() || fontMatch.fontFilePath.empty()) {
        return "";
    }

    // Embed font
    auto embedResult = pImpl_->fontEmbedder_->embedFont(page, document, fontMatch.fontFilePath);
    if (!embedResult.has_value()) {
        return "";
    }

    std::string resourceName = embedResult->resourceName;

    // pdfRect is in fitz-space (Y-down). Tm operators use standard PDF space
    // (Y-up), so we convert: standardY = pageHeight - fitzY.
    QRectF pageBBox = page->boundingBox();
    double pageH = pageBBox.height();

    // Build PDF operators string
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);

    // Save graphics state
    oss << "q\n";
    // Reset ambient text state to a known basis so an ambient -100 Tz / Tc /
    // Tw / Ts on the page can't mirror or distort the reconstructed text. The
    // black rg here is immediately overridden by this block's own colour below.
    oss << pdfe::core::knownTextStateBasis();
    oss << "BT\n";

    // Set font and size
    oss << "/" << resourceName << " " << fontMatch.sizePt << " Tf\n";

    // Set text color
    double r = fontMatch.color.r / 255.0;
    double g = fontMatch.color.g / 255.0;
    double b = fontMatch.color.b / 255.0;
    oss << r << " " << g << " " << b << " rg\n";

    // For multi-line text with HOCR line data
    if (!lines.empty()) {
        // Use HOCR line positioning
        for (const auto& line : lines) {
            // Convert fitz-space Y to standard PDF Y for Tm operator.
            // line.bbox.y1 (fitz) is the bottom of this line in Y-down space.
            // We position baseline at: pageH - fitz_y, offset from rect top.
            double fitzLineY = pdfRect.y0 + (line.bbox.y1 - pdfRect.y0);
            double baselineY = pageH - fitzLineY;
            oss << "1 0 0 1 " << pdfRect.x0 << " " << baselineY << " Tm\n";

            // Encode text for this line
            std::string lineText;
            for (const auto& word : line.words) {
                lineText += word.text + " ";
            }
            if (!lineText.empty()) {
                lineText.pop_back(); // Remove trailing space
            }

            // Map codepoints -> glyph indices via the font cmap. The embedded
            // font is Type0/Identity-H, so the show-string bytes are glyph
            // indices; emitting raw codepoints would render the wrong glyphs
            // (out-of-class finding #3). encodeGlyphIndicesHex() degrades an
            // unresolvable glyph to .notdef, never to a raw codepoint.
            auto codepoints = utf8ToCodepoints(lineText);
            std::string glyphHex;
            try {
                pImpl_->context_->execute([&](fz_context* ctx) {
                    glyphHex = encodeGlyphIndicesHex(
                        ctx, embedResult->fzFont, codepoints);
                });
            } catch (const std::exception& e) {
                qWarning("PdfContentWriter::writeTextBlock: glyph encoding failed for line: %s", e.what());
                // All-.notdef rather than raw codepoints on total failure.
                glyphHex.assign(codepoints.size() * 4, '0');
            } catch (...) {
                qWarning("PdfContentWriter::writeTextBlock: unknown glyph encoding failure for line");
                glyphHex.assign(codepoints.size() * 4, '0');
            }
            oss << "<" << glyphHex << "> Tj\n";
        }
    } else {
        // Single-line text, position near top of bounding box (fitz y0 = top)
        double yOffset = fontMatch.sizePt * 0.8; // Approximate descent
        double tmY = pageH - pdfRect.y0 - yOffset;
        oss << "1 0 0 1 " << pdfRect.x0 << " " << tmY << " Tm\n";

        // Map codepoints -> glyph indices via the font cmap (Type0/Identity-H
        // CID font). Previously the exception fallback re-emitted the raw
        // Unicode codepoints as hex glyph indices (<XXXX>), rendering the wrong
        // glyphs because glyph index != codepoint for virtually every font
        // (out-of-class finding #3). Routing through encodeGlyphIndicesHex()
        // guarantees glyph indices, degrading only to .notdef on failure.
        auto codepoints = utf8ToCodepoints(utf8Text);
        std::string glyphHex;
        try {
            pImpl_->context_->execute([&](fz_context* ctx) {
                glyphHex = encodeGlyphIndicesHex(
                    ctx, embedResult->fzFont, codepoints);
            });
        } catch (const std::exception& e) {
            qWarning("PdfContentWriter::writeTextBlock: glyph encoding failed: %s", e.what());
            glyphHex.assign(codepoints.size() * 4, '0');
        } catch (...) {
            qWarning("PdfContentWriter::writeTextBlock: unknown glyph encoding failure");
            glyphHex.assign(codepoints.size() * 4, '0');
        }
        oss << "<" << glyphHex << "> Tj\n";
    }

    oss << "ET\n";
    oss << "Q\n";

    std::string operators = oss.str();

    // Append to content stream
    appendContentStream(page, document, operators);

    return operators;
}

// ---------------------------------------------------------------------------
// appendContentStream
// ---------------------------------------------------------------------------

bool PdfContentWriter::appendContentStream(
    pdfe::core::Page* page, pdfe::core::Document* document,
    const std::string& operators)
{
    if (!page || !document || !pImpl_->context_ || operators.empty()) {
        return false;
    }

    try {
        pImpl_->context_->execute([&](fz_context* ctx) {
            fz_page* fzPage = page->fzPage();
            fz_document* fzDoc = document->fzDocument();
            pdf_page* pdfPage = pdf_page_from_fz_page(ctx, fzPage);
            pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, fzDoc);

            if (!pdfPage || !pdfDoc) {
                fz_throw(ctx, FZ_ERROR_GENERIC, "Invalid page or document");
            }

            // Append so the reconstructed text runs at IDENTITY CTM even on
            // N-up / imposed pages: appendContentIsolatingAmbientCtm brackets
            // the existing page content in `q ... Q`, so its outer `Q` restores
            // the page-initial identity CTM before this fragment runs. A bare
            // append would inherit an ambient `cm` (e.g. "0.5 0 0 0.5 0 0 cm")
            // and render the text half-size. The fragment is itself balanced
            // `q ... Q`, so the guard is a no-op for the OUTBOUND direction.
            // The helper creates its own fragment stream from the operator
            // bytes. (Register H1 / content-stream-state-continuity audit,
            // pipeline content-stream-continuity phase-2.)
            pdfe::core::appendContentIsolatingAmbientCtm(
                ctx, pdfDoc, pdfPage->obj, operators.data(), operators.size());
        });

        return true;
    } catch (const std::exception& e) {
        qWarning("PdfContentWriter::appendContentStream: %s", e.what());
        return false;
    } catch (...) {
        qWarning("PdfContentWriter::appendContentStream: unknown exception");
        return false;
    }
}

// ---------------------------------------------------------------------------
// getImageXObjectData
// ---------------------------------------------------------------------------

std::vector<uint8_t> PdfContentWriter::getImageXObjectData(
    pdfe::core::Page* page, pdfe::core::Document* document)
{
    if (!page || !document || !pImpl_->context_) {
        return {};
    }

    auto imageInfo = findPageImageXObject(page, document);
    if (!imageInfo.has_value()) {
        return {};
    }

    // `result` lives in THIS frame, not the guarded region: a longjmp out of
    // MuPDF would skip its destructor (GitHub #118 hazard class).
    std::vector<uint8_t> result;
    try {
        pImpl_->context_->execute([&](fz_context* ctx) {
                fz_page* fzPage = page->fzPage();
                fz_document* fzDoc = document->fzDocument();
                pdf_page* pdfPage = pdf_page_from_fz_page(ctx, fzPage);
                const pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, fzDoc);

                if (!pdfPage || !pdfDoc) {
                    return;
                }

                pdf_obj* resources = pdf_page_resources(ctx, pdfPage);
                if (!resources) {
                    return;
                }

                pdf_obj* xobjectDict = pdf_dict_get(ctx, resources, PDF_NAME(XObject));
                if (!xobjectDict) {
                    return;
                }

                pdf_obj* imageObj = pdf_dict_gets(ctx, xobjectDict, imageInfo->xobjectName.c_str());
                if (!imageObj) {
                    return;
                }

                // pdf_load_stream needs the indirect reference (not resolved)
                // to locate the stream data by object number
                fz_buffer* buf = pdf_load_stream(ctx, imageObj);
                if (!buf) {
                    return;
                }

                unsigned char* data = nullptr;
                const size_t bufSize = fz_buffer_storage(ctx, buf, &data);
                if (data) {
                    result.assign(data, data + bufSize);
                }

                fz_drop_buffer(ctx, buf);
            });
        return result;
    } catch (const std::exception& e) {
        qWarning("PdfContentWriter::getImageXObjectData: %s", e.what());
        return {};
    } catch (...) {
        qWarning("PdfContentWriter::getImageXObjectData: unknown exception");
        return {};
    }
}

// ---------------------------------------------------------------------------
// getOriginalImage
// ---------------------------------------------------------------------------

QImage PdfContentWriter::getOriginalImage(
    pdfe::core::Page* page, pdfe::core::Document* document)
{
    if (!page || !document || !pImpl_->context_) {
        return {};
    }

    auto resolved = findPageImageXObject(page, document);
    if (!resolved.has_value() || resolved->width <= 0 || resolved->height <= 0) {
        return {};
    }

    return getOriginalImage(page, document, *resolved);
}

QImage PdfContentWriter::getOriginalImage(
    pdfe::core::Page* page, pdfe::core::Document* document,
    const PageImageInfo& target)
{
    if (!page || !document || !pImpl_->context_ || target.xobjectName.empty()) {
        return {};
    }
    const PageImageInfo* imageInfo = &target;

    // `result` lives in THIS frame, not the guarded region: a longjmp out of
    // MuPDF would skip its destructor (GitHub #118 hazard class).
    QImage result;
    try {
        pImpl_->context_->execute([&](fz_context* ctx) {
                fz_page* fzPage = page->fzPage();
                fz_document* fzDoc = document->fzDocument();
                pdf_page* pdfPage = pdf_page_from_fz_page(ctx, fzPage);
                pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, fzDoc);

                if (!pdfPage || !pdfDoc) {
                    return;
                }

                pdf_obj* resources = pdf_page_resources(ctx, pdfPage);
                if (!resources) return;

                pdf_obj* xobjectDict = pdf_dict_get(ctx, resources, PDF_NAME(XObject));
                if (!xobjectDict) return;

                pdf_obj* imageObj = pdf_dict_gets(ctx, xobjectDict,
                                                   imageInfo->xobjectName.c_str());
                if (!imageObj) return;

                // Load the image XObject (throws on failure, never null)
                fz_image* fzImage = pdf_load_image(ctx, pdfDoc, imageObj);

                // Decode to pixmap — use fz_always to ensure fz_drop_image
                fz_pixmap* pix = nullptr;
                fz_try(ctx) {
                    pix = fz_get_pixmap_from_image(
                        ctx, fzImage, nullptr, nullptr, nullptr, nullptr);
                } fz_always(ctx) {
                    fz_drop_image(ctx, fzImage);
                } fz_catch(ctx) {
                    fz_rethrow(ctx);
                }

                // Convert CMYK or other colorspaces to sRGB
                int n = fz_pixmap_components(ctx, pix);
                const fz_colorspace* cs = fz_pixmap_colorspace(ctx, pix);
                if (n == 4 && cs != fz_device_rgb(ctx)) {
                    fz_pixmap* rgbPix = fz_convert_pixmap(
                        ctx, pix, fz_device_rgb(ctx), nullptr,
                        nullptr, fz_default_color_params, 0);
                    fz_drop_pixmap(ctx, pix);
                    pix = rgbPix;
                    n = fz_pixmap_components(ctx, pix);
                }

                int w = fz_pixmap_width(ctx, pix);
                int h = fz_pixmap_height(ctx, pix);
                unsigned char* samples = fz_pixmap_samples(ctx, pix);
                int stride = fz_pixmap_stride(ctx, pix);

                // GitHub #445: a colour-key /Mask comes back as alpha, and MuPDF
                // zeroes the colour of every keyed-out pixel. Written back as
                // black, those pixels would no longer match the key and would
                // show. Give them the key's own colour instead (its lower
                // bound), so the key replacePageImage keeps still hides them.
                const bool alpha = fz_pixmap_alpha(ctx, pix) != 0;
                int key[3] = {-1, -1, -1};
                pdf_obj* colourKey = pdf_dict_get(ctx, imageObj, PDF_NAME(Mask));
                if (alpha && n == 4 && pdf_is_array(ctx, colourKey) &&
                    pdf_array_len(ctx, colourKey) == 6) {
                    for (int c = 0; c < 3; ++c)
                        key[c] = pdf_array_get_int(ctx, colourKey, 2 * c);
                }

                result = QImage(w, h, QImage::Format_RGB888);
                for (int y = 0; y < h; ++y) {
                    const unsigned char* srcLine = samples + y * stride;
                    unsigned char* dstLine = result.scanLine(y);
                    if (n >= 3) {
                        for (int x = 0; x < w; ++x) {
                            if (key[0] >= 0 && srcLine[x * n + 3] == 0) {
                                dstLine[x * 3 + 0] = static_cast<unsigned char>(key[0]);
                                dstLine[x * 3 + 1] = static_cast<unsigned char>(key[1]);
                                dstLine[x * 3 + 2] = static_cast<unsigned char>(key[2]);
                                continue;
                            }
                            dstLine[x * 3 + 0] = srcLine[x * n + 0];
                            dstLine[x * 3 + 1] = srcLine[x * n + 1];
                            dstLine[x * 3 + 2] = srcLine[x * n + 2];
                        }
                    } else {
                        // Grayscale (+ optional alpha) → replicate to RGB
                        for (int x = 0; x < w; ++x) {
                            dstLine[x * 3 + 0] = srcLine[x * n];
                            dstLine[x * 3 + 1] = srcLine[x * n];
                            dstLine[x * 3 + 2] = srcLine[x * n];
                        }
                    }
                }

                fz_drop_pixmap(ctx, pix);
            });
        return result;
    } catch (const std::exception& e) {
        qWarning("PdfContentWriter::getOriginalImage: %s", e.what());
        return {};
    } catch (...) {
        qWarning("PdfContentWriter::getOriginalImage: unknown exception");
        return {};
    }
}

// ---------------------------------------------------------------------------
// getContentStreamData
// ---------------------------------------------------------------------------

std::vector<uint8_t> PdfContentWriter::getContentStreamData(
    pdfe::core::Page* page, pdfe::core::Document* document)
{
    if (!page || !document || !pImpl_->context_) {
        return {};
    }

    // `result` lives in THIS frame, not the guarded region: a longjmp out of
    // MuPDF would skip its destructor (GitHub #118 hazard class).
    std::vector<uint8_t> result;
    try {
        pImpl_->context_->execute([&](fz_context* ctx) {
                fz_page* fzPage = page->fzPage();
                fz_document* fzDoc = document->fzDocument();
                pdf_page* pdfPage = pdf_page_from_fz_page(ctx, fzPage);
                const pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, fzDoc);

                if (!pdfPage || !pdfDoc) {
                    return;
                }

                pdf_obj* contents = pdf_dict_get(ctx, pdfPage->obj, PDF_NAME(Contents));
                if (!contents) {
                    return;
                }

                // DEFECT FIX (meta-audit follow-up F17, gap A1-51): both branches
                // used to call pdf_resolve_indirect() before pdf_load_stream().
                // pdf_load_stream() takes the indirect REFERENCE -- it needs the
                // object number to locate the stream body. Handing it the
                // resolved direct dict makes it fz_throw "object is not a
                // stream", so this function returned an EMPTY vector for every
                // ordinary page in every PDF, silently (the throw was swallowed
                // by the outer catch + qWarning). Nothing noticed because the
                // only oracles over it were `EXPECT_EQ(a.size(), b.size())`
                // guarded by `if (!a.empty())` -- 0 == 0, test green.
                // pdf_load_stream resolves internally, so pass the ref through.
                if (pdf_is_array(ctx, contents)) {
                    // Concatenate all streams in array
                    int len = pdf_array_len(ctx, contents);
                    for (int i = 0; i < len; ++i) {
                        pdf_obj* streamObj = pdf_array_get(ctx, contents, i);
                        if (!streamObj) continue;

                        fz_buffer* buf = pdf_load_stream(ctx, streamObj);
                        if (!buf) continue;

                        size_t bufSize = fz_buffer_storage(ctx, buf, nullptr);
                        unsigned char* data = nullptr;
                        fz_buffer_storage(ctx, buf, &data);
                        if (data) {
                            result.insert(result.end(), data, data + bufSize);
                        }

                        fz_drop_buffer(ctx, buf);
                    }
                } else {
                    // Single stream (see the fix note above: no resolve here).
                    if (contents) {
                        fz_buffer* buf = pdf_load_stream(ctx, contents);
                        if (buf) {
                            size_t bufSize = fz_buffer_storage(ctx, buf, nullptr);
                            result.resize(bufSize);

                            unsigned char* data = nullptr;
                            fz_buffer_storage(ctx, buf, &data);
                            if (data) {
                                std::memcpy(result.data(), data, bufSize);
                            }

                            fz_drop_buffer(ctx, buf);
                        }
                    }
                }
            });
        return result;
    } catch (const std::exception& e) {
        qWarning("PdfContentWriter::getContentStreamData: %s", e.what());
        return {};
    } catch (...) {
        qWarning("PdfContentWriter::getContentStreamData: unknown exception");
        return {};
    }
}

// ---------------------------------------------------------------------------
// saveImageXObjectRef
// ---------------------------------------------------------------------------

void* PdfContentWriter::saveImageXObjectRef(
    pdfe::core::Page* page, pdfe::core::Document* document)
{
    if (!page || !document || !pImpl_->context_) {
        return nullptr;
    }

    auto imageInfo = findPageImageXObject(page, document);
    if (!imageInfo.has_value()) {
        return nullptr;
    }

    try {
        return pImpl_->context_->executeReturn<void*>(
            [&](fz_context* ctx) -> void* {
                fz_page* fzPage = page->fzPage();
                pdf_page* pdfPage = pdf_page_from_fz_page(ctx, fzPage);
                if (!pdfPage) return nullptr;

                pdf_obj* resources = pdf_page_resources(ctx, pdfPage);
                if (!resources) return nullptr;

                pdf_obj* xobjectDict = pdf_dict_get(ctx, resources, PDF_NAME(XObject));
                if (!xobjectDict) return nullptr;

                pdf_obj* imageObj = pdf_dict_gets(ctx, xobjectDict, imageInfo->xobjectName.c_str());
                if (!imageObj) return nullptr;

                // R2-04/A4: deep-copy the image XObject into its OWN standalone
                // pdf_document (holder) so it survives a full-rewrite save
                // (Document::save drop+reopen / saveAs GC-renumber). A raw
                // pdf_keep_obj would dangle across such a save → post-save undo UAF.
                return pdfe::core::makeRetainedObjHolder(ctx, imageObj);
            });
    } catch (const std::exception& e) {
        qWarning("PdfContentWriter::saveImageXObjectRef: %s", e.what());
        return nullptr;
    } catch (...) {
        qWarning("PdfContentWriter::saveImageXObjectRef: unknown exception");
        return nullptr;
    }
}

// ---------------------------------------------------------------------------
// restoreImageXObjectRef
// ---------------------------------------------------------------------------

bool PdfContentWriter::restoreImageXObjectRef(
    pdfe::core::Page* page, const pdfe::core::Document* document,
    void* savedRef, const std::string& xobjectName)
{
    if (!page || !document || !pImpl_->context_ || !savedRef || xobjectName.empty()) {
        return false;
    }

    try {
        pImpl_->context_->execute([&](fz_context* ctx) {
            fz_page* fzPage = page->fzPage();
            pdf_page* pdfPage = pdf_page_from_fz_page(ctx, fzPage);
            if (!pdfPage) {
                fz_throw(ctx, FZ_ERROR_GENERIC, "Invalid page");
            }

            pdf_obj* resources = pdf_page_resources(ctx, pdfPage);
            if (!resources) {
                fz_throw(ctx, FZ_ERROR_GENERIC, "No page resources");
            }

            pdf_obj* xobjectDict = pdf_dict_get(ctx, resources, PDF_NAME(XObject));
            if (!xobjectDict) {
                fz_throw(ctx, FZ_ERROR_GENERIC, "No XObject dictionary");
            }

            // NON-consuming: graft a COPY of the held XObject into the LIVE doc
            // and put that; drop the local ref (pdf_dict_puts keeps its own).
            // The holder stays owned by the command (freed via releaseImageXObjectRef).
            pdf_obj* live = pdfe::core::graftRetainedInto(ctx, pdfPage->doc, savedRef);
            if (live) {
                pdf_dict_puts(ctx, xobjectDict, xobjectName.c_str(), live);
                pdf_drop_obj(ctx, live);
            }
        });
        return true;
    } catch (const std::exception& e) {
        qWarning("PdfContentWriter::restoreImageXObjectRef: %s", e.what());
        return false;
    } catch (...) {
        qWarning("PdfContentWriter::restoreImageXObjectRef: unknown exception");
        return false;
    }
}

// ---------------------------------------------------------------------------
// releaseImageXObjectRef
// ---------------------------------------------------------------------------

void PdfContentWriter::releaseImageXObjectRef(void* ref)
{
    if (!ref || !pImpl_->context_) {
        return;
    }

    try {
        pImpl_->context_->execute([&](fz_context* ctx) {
            pdfe::core::dropRetainedObjHolder(ctx, ref);
        });
    } catch (...) {
        // Silently ignore cleanup errors
    }
}

// ---------------------------------------------------------------------------
// saveContentsRef
// ---------------------------------------------------------------------------

void* PdfContentWriter::saveContentsRef(
    pdfe::core::Page* page, const pdfe::core::Document* document)
{
    if (!page || !document || !pImpl_->context_) {
        return nullptr;
    }

    try {
        return pImpl_->context_->executeReturn<void*>(
            [&](fz_context* ctx) -> void* {
                fz_page* fzPage = page->fzPage();
                pdf_page* pdfPage = pdf_page_from_fz_page(ctx, fzPage);
                if (!pdfPage) return nullptr;

                pdf_obj* contents = pdf_dict_get(ctx, pdfPage->obj, PDF_NAME(Contents));
                if (!contents) return nullptr;

                // R2-04/A4: deep-copy /Contents into its OWN standalone holder
                // document so it survives a full-rewrite save; a raw pdf_keep_obj
                // would dangle across Document::save() → post-save undo UAF.
                return pdfe::core::makeRetainedObjHolder(ctx, contents);
            });
    } catch (const std::exception& e) {
        qWarning("PdfContentWriter::saveContentsRef: %s", e.what());
        return nullptr;
    } catch (...) {
        qWarning("PdfContentWriter::saveContentsRef: unknown exception");
        return nullptr;
    }
}

// ---------------------------------------------------------------------------
// restoreContentsRef
// ---------------------------------------------------------------------------

bool PdfContentWriter::restoreContentsRef(
    pdfe::core::Page* page, const pdfe::core::Document* document,
    void* savedRef)
{
    if (!page || !document || !pImpl_->context_) {
        return false;
    }

    try {
        pImpl_->context_->execute([&](fz_context* ctx) {
            fz_page* fzPage = page->fzPage();
            pdf_page* pdfPage = pdf_page_from_fz_page(ctx, fzPage);
            if (!pdfPage) {
                fz_throw(ctx, FZ_ERROR_GENERIC, "Invalid page");
            }

            if (savedRef) {
                // NON-consuming: graft a COPY of the held /Contents into the
                // LIVE doc and put that; drop the local ref (pdf_dict_put keeps
                // its own). The holder stays owned by the command (freed via
                // releaseContentsRef).
                pdf_obj* live = pdfe::core::graftRetainedInto(ctx, pdfPage->doc, savedRef);
                if (live) {
                    pdf_dict_put(ctx, pdfPage->obj, PDF_NAME(Contents), live);
                    pdf_drop_obj(ctx, live);
                }
            } else {
                // No previous contents -- remove the key
                pdf_dict_del(ctx, pdfPage->obj, PDF_NAME(Contents));
            }
        });
        return true;
    } catch (const std::exception& e) {
        qWarning("PdfContentWriter::restoreContentsRef: %s", e.what());
        return false;
    } catch (...) {
        qWarning("PdfContentWriter::restoreContentsRef: unknown exception");
        return false;
    }
}

// ---------------------------------------------------------------------------
// releaseContentsRef
// ---------------------------------------------------------------------------

void PdfContentWriter::releaseContentsRef(void* ref)
{
    if (!ref || !pImpl_->context_) {
        return;
    }

    try {
        pImpl_->context_->execute([&](fz_context* ctx) {
            pdfe::core::dropRetainedObjHolder(ctx, ref);
        });
    } catch (...) {
        // Silently ignore cleanup errors
    }
}

} // namespace pdfe::tre
