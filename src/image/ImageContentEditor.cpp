// -*- coding: utf-8 -*-
#define _USE_MATH_DEFINES
#include "image/ImageContentEditor.h"
#include "image/ImagePlacement.h"
#include "image/ImagePlacementScanner.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <cmath>
#include <exception>
#include <functional>

namespace pdfe::image {

namespace pl = pdfe::image::placement;

namespace {

constexpr double kPi = 3.14159265358979323846;

// The visual-clockwise angle in degrees (toolbar / command / ImageTool
// convention) as a rotation in the decomposition's convention: CCW-positive
// in the Y-up rendered frame. A visually clockwise turn is a NEGATIVE math
// angle there; the Y-down -> Y-up conversion of positions does not change
// this, because the angle convention is defined visually (GitHub #376).
double rotationFromVisualClockwise(float angleDegrees) {
    return -static_cast<double>(angleDegrees) * kPi / 180.0;
}

// Place linear part `L` (rendered fitz frame) so that unit point (u, v)
// lands on `target`.
pl::Affine placeAt(pl::Affine L, double u, double v, QPointF target) {
    L.e = target.x() - (u * L.a + v * L.c);
    L.f = target.y() - (u * L.b + v * L.d);
    return L;
}

} // namespace

// An edit maps the placement's current (P, C) to a new (P, C). P is the
// placement in the RENDERED fitz frame (unit square -> Y-down page, page
// /Rotate applied); C the unit-square crop.
using EditFn = std::function<bool(const pl::Affine& P, const pl::UnitCrop& C,
                                  pl::Affine& newP, pl::UnitCrop& newC)>;

class ImageContentEditor::Impl {
public:
    std::shared_ptr<pdfe::core::MuPdfContext> context;

    explicit Impl(std::shared_ptr<pdfe::core::MuPdfContext> ctx)
        : context(std::move(ctx))
    {
    }

    pdf_page* getPdfPage(pdfe::core::Page* page) const {
        if (!page) return nullptr;
        fz_page* fzPage = page->fzPage();
        if (!fzPage) return nullptr;
        return pdf_page_from_fz_page(context->get(), fzPage);
    }

    pdf_document* getPdfDoc(pdfe::core::Document* doc) const {
        if (!doc) return nullptr;
        fz_document* fzDoc = doc->fzDocument();
        if (!fzDoc) return nullptr;
        return pdf_document_from_fz_document(context->get(), fzDoc);
    }

    // Find the targeted Do, hand its current geometry to `fn`, and splice the
    // new wrapper in place of the old Do (or of the PDFE wrapper around it).
    bool edit(pdfe::core::Page* page, pdfe::core::Document* doc,
              const PlacementId& id, const EditFn& fn)
    {
        if (!page || !doc || id.xobjectName.empty() || id.occurrence < 0) return false;
        bool success = false;
        // Scanned in THIS frame, not the lambda's: the lambda runs inside
        // MuPdfContext::execute's fz_try, and a longjmp skips the destructors
        // of its locals (tools/gate/fz_try_raii_scan.py).
        std::vector<pl::DoSite> sites;
        try {
            context->execute([&](fz_context* ctx) {
                pdf_page* pdfPage = getPdfPage(page);
                pdf_document* pdfDoc = getPdfDoc(doc);
                if (!pdfPage || !pdfDoc) return;

                sites = pl::scanPage(ctx, pdfPage, false);
                const pl::DoSite* target = nullptr;
                for (const auto& s : sites) {
                    if (s.formDepth == 0 && s.name == id.xobjectName &&
                        s.occurrence == id.occurrence) {
                        target = &s;
                        break;
                    }
                }
                // Only a page-level Do that resolves to an Image XObject is
                // editable: an unresolvable name, a Form, or a Do inside a Form
                // XObject (never matched above: formDepth != 0) is refused.
                if (!target || !target->isImage) return;

                // User space -> rendered fitz frame, and back (applied ONCE at
                // the content-stream boundary; GitHub #373/#426).
                const pl::Affine pageT = pl::pageTransform(ctx, pdfPage);
                pl::Affine pageInv;
                if (!pl::invert(pageT, pageInv)) return;

                const pl::Affine P = pl::concat(target->ctm, pageT);
                pl::Affine newP = P;
                pl::UnitCrop newC = target->crop;
                if (!fn(P, target->crop, newP, newC)) return;
                if (newC.isEmpty()) return;

                success = pl::rewriteSite(ctx, pdfDoc, pdfPage, *target,
                                          pl::concat(newP, pageInv), newC);
            });
        } catch (const std::exception&) {
            success = false;
        }
        // The old content stream is orphaned by a rewrite; force a
        // full-rewrite save (pipeline b2e47d91, Tier 1). Only after a real
        // change: a refused edit leaves the document's save routing alone.
        if (success) doc->markRequiresFullRewrite();
        return success;
    }
};

ImageContentEditor::ImageContentEditor(std::shared_ptr<pdfe::core::MuPdfContext> context)
    : impl_(std::make_unique<Impl>(std::move(context)))
{
}

ImageContentEditor::~ImageContentEditor() = default;

// ---------------------------------------------------------------------------
// Move / resize
// ---------------------------------------------------------------------------
bool ImageContentEditor::resizeImage(
    pdfe::core::Page* page, pdfe::core::Document* doc,
    const std::string& xobjectName,
    float newWidth, float newHeight,
    float x, float y,
    float angleDegrees)
{
    return resizeImage(page, doc, PlacementId{xobjectName, 0},
                       newWidth, newHeight, x, y, angleDegrees);
}

bool ImageContentEditor::resizeImage(
    pdfe::core::Page* page, pdfe::core::Document* doc,
    const PlacementId& placement,
    float newWidth, float newHeight,
    float x, float y,
    float angleDegrees)
{
    if (!(newWidth > 0.0f) || !(newHeight > 0.0f)) return false;
    // GitHub #416: (x, y, newWidth, newHeight) is the UPRIGHT full rect and
    // angleDegrees its orientation, so a move or resize keeps the rotation.
    // GitHub #424: the new matrix is the old one re-scaled in the image's own
    // axes and re-oriented, never rebuilt from (w, h, angle), so flips and
    // skew survive.
    return impl_->edit(page, doc, placement,
        [&](const pl::Affine& P, const pl::UnitCrop& C, pl::Affine& newP, pl::UnitCrop& newC) {
            pl::Affine L;
            if (!pl::reorient(P, rotationFromVisualClockwise(angleDegrees),
                              newWidth, newHeight, L))
                return false;
            newP = placeAt(L, 0.5, 0.5, QPointF(double(x) + newWidth / 2.0,
                                                double(y) + newHeight / 2.0));
            newC = C;  // GitHub #419: the crop moves and scales with the image
            return true;
        });
}

// ---------------------------------------------------------------------------
// Rotate
// ---------------------------------------------------------------------------
bool ImageContentEditor::rotateImage(
    pdfe::core::Page* page, pdfe::core::Document* doc,
    const std::string& xobjectName,
    float angleDegrees,
    float centerX, float centerY,
    float displayWidth, float displayHeight)
{
    return rotateImage(page, doc, PlacementId{xobjectName, 0}, angleDegrees,
                       centerX, centerY, displayWidth, displayHeight);
}

bool ImageContentEditor::rotateImage(
    pdfe::core::Page* page, pdfe::core::Document* doc,
    const PlacementId& placement,
    float angleDegrees,
    float centerX, float centerY,
    float displayWidth, float displayHeight)
{
    if (!(displayWidth > 0.0f) || !(displayHeight > 0.0f)) return false;
    // Rotation about the VISIBLE centre: the centre of the crop stays at
    // (centerX, centerY), the crop and flips/skew are kept (GitHub #419/#424).
    return impl_->edit(page, doc, placement,
        [&](const pl::Affine& P, const pl::UnitCrop& C, pl::Affine& newP, pl::UnitCrop& newC) {
            pl::Affine L;
            if (!pl::reorient(P, rotationFromVisualClockwise(angleDegrees),
                              displayWidth, displayHeight, L))
                return false;
            newP = placeAt(L, (C.u0 + C.u1) / 2.0, (C.v0 + C.v1) / 2.0,
                           QPointF(centerX, centerY));
            newC = C;
            return true;
        });
}

// ---------------------------------------------------------------------------
// Crop
// ---------------------------------------------------------------------------
bool ImageContentEditor::cropImage(
    pdfe::core::Page* page, pdfe::core::Document* doc,
    const std::string& xobjectName,
    QRectF cropRect,
    float displayWidth, float displayHeight,
    float x, float y)
{
    return cropImage(page, doc, PlacementId{xobjectName, 0}, cropRect,
                     displayWidth, displayHeight, x, y);
}

bool ImageContentEditor::cropImage(
    pdfe::core::Page* page, pdfe::core::Document* doc,
    const PlacementId& placement,
    QRectF cropRect,
    float /*displayWidth*/, float /*displayHeight*/,
    float /*x*/, float /*y*/)
{
    cropRect = cropRect.normalized();
    if (cropRect.isEmpty()) return false;
    // GitHub #417: the crop is taken in the image's own axes and stored in its
    // unit square, inside the cm, so the placement matrix - and with it the
    // rotation - is untouched.
    return impl_->edit(page, doc, placement,
        [&](const pl::Affine& P, const pl::UnitCrop& C, pl::Affine& newP, pl::UnitCrop& newC) {
            pl::Affine Pinv;
            if (!pl::invert(P, Pinv)) return false;
            // The visible upright frame is the fitz frame turned by
            // +rotationRadians about the visible centre; a point q of it is
            // on the page at rotateAbout(q, centre, -rotationRadians) (Y-down
            // math convention, GitHub #342/#376).
            const pl::Decomposition dec = pl::decompose(P);
            const QPointF centre = pl::visibleCentre(P, C);
            double u0 = 1e300, v0 = 1e300, u1 = -1e300, v1 = -1e300;
            for (const QPointF q : {cropRect.topLeft(), cropRect.topRight(),
                                    cropRect.bottomLeft(), cropRect.bottomRight()}) {
                const QPointF unit = Pinv.apply(pl::rotateAbout(q, centre, -dec.rotation));
                u0 = std::min(u0, unit.x()); u1 = std::max(u1, unit.x());
                v0 = std::min(v0, unit.y()); v1 = std::max(v1, unit.y());
            }
            // Crops compose: keep only what is visible now AND inside the rect.
            newC = C.intersected(pl::UnitCrop{u0, v0, u1, v1}).clamped();
            newP = P;
            return !newC.isEmpty();
        });
}

} // namespace pdfe::image
