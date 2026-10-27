#include "Page.h"
#include "TextExtractor.h"
#include "MuPdfContext.h"
#include "StextOptions.h"
#include "CoverageGeometry.h"
#include "ToUnicodeRecovery.h"
#include "Error.h"

#include <algorithm>

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

namespace pdfe::core {

class Page::Impl {
public:
    std::shared_ptr<MuPdfContext> context;
    fz_page* fzPage = nullptr;
    pdf_page* pdfPage = nullptr;
    int pageIndex = 0;
    Document* ownerDoc = nullptr;  // non-owning; Document outlives its Pages

    Impl(std::shared_ptr<MuPdfContext> ctx, fz_page* page, pdf_page* pdfPg, int index,
         Document* owner)
        : context(std::move(ctx)), fzPage(page), pdfPage(pdfPg), pageIndex(index),
          ownerDoc(owner) {}

    ~Impl() {
        if (fzPage && context) {
            context->execute([this](fz_context* ctx) {
                fz_drop_page(ctx, fzPage);
            });
        }
    }
};

Page::Page(
    std::shared_ptr<MuPdfContext> context,
    fz_page* fzPage,
    pdf_page* pdfPage,
    int index,
    Document* ownerDoc
)
    : pImpl(std::make_unique<Impl>(std::move(context), fzPage, pdfPage, index, ownerDoc))
{
    // GLYPH-NAME / StandardEncoding /ToUnicode RECOVERY (gap PDF-72, GitHub #23).
    //
    // Runs HERE, at page construction, rather than inside TextExtractor, because
    // the recovery has to be in place before MuPDF loads any of this page's
    // fonts — and a font is loaded by the first RENDER just as much as by the
    // first extraction. Doing it at the single point where a Page comes into
    // existence makes "was the page painted first?" irrelevant to whether text
    // extracts correctly. (ToUnicodeRecovery additionally purges each patched
    // font from the resource store, so a font shared with an already-rendered
    // sibling page is re-read rather than served stale from the cache.)
    //
    // It is best-effort and silent: recoverMissingToUnicode never throws, never
    // longjmps, and returns 0 for a non-PDF document or an unwalkable resource
    // tree. Opening a page must never fail because a font was odd.
    if (pImpl->pdfPage && pImpl->context && pImpl->fzPage) {
        pImpl->context->execute([this](fz_context* ctx) {
            recoverMissingToUnicode(ctx, pImpl->fzPage);
        });
    }
}

Document* Page::ownerDocument() const {
    return pImpl->ownerDoc;
}

Page::~Page() = default;

Page::Page(Page&&) noexcept = default;

Page& Page::operator=(Page&&) noexcept = default;

QImage Page::render(const QRectF& region, float scale) {
    (void)region;

    // Rasterize to a pixmap in a MINIMAL pure-C lambda: a single fz call whose
    // only outcome is either "returns the pixmap" or "fz_throws and returns
    // nothing" — it cannot leak (R2-05).
    fz_pixmap* pixmap = pImpl->context->executeReturn<fz_pixmap*>(
        [this, scale](fz_context* ctx) -> fz_pixmap* {
            fz_matrix transform = fz_scale(scale, scale);
            fz_colorspace* colorspace = fz_device_rgb(ctx);
            return fz_new_pixmap_from_page(
                ctx,
                pImpl->fzPage,
                transform,
                colorspace,
                1  // alpha = 1 (include alpha channel)
            );
        }
    );
    if (!pixmap) {
        return QImage();
    }

    // RAII guard so the pixmap is dropped on EVERY path out of here — including
    // if the QImage allocation/copy below throws (R2-05 defense-in-depth). This
    // is pure C++ OUTSIDE any fz_try; the old single-lambda version held the
    // live pixmap through this C++ work with no drop guard, so a throw here
    // leaked it.
    struct PixmapGuard {
        MuPdfContext* ctx;
        fz_pixmap* p;
        ~PixmapGuard() {
            if (p) {
                ctx->execute([q = p](fz_context* c) { fz_drop_pixmap(c, q); });
            }
        }
    } guard{pImpl->context.get(), pixmap};

    // Convert pixmap -> QImage. MuPDF pixmap format is RGBA when alpha=1; the
    // stride may include padding, so copy row by row. No MuPDF call here can
    // fz_throw.
    const int width = pixmap->w;
    const int height = pixmap->h;
    const int stride = pixmap->stride;
    const unsigned char* samples = pixmap->samples;

    QImage image(width, height, QImage::Format_RGBA8888);
    for (int y = 0; y < height; ++y) {
        const unsigned char* srcLine = samples + y * stride;
        unsigned char* dstLine = image.scanLine(y);
        memcpy(dstLine, srcLine, static_cast<size_t>(width) * 4);  // 4 bytes/pixel
    }

    return image;
}

QRectF Page::boundingBox() const {
    return pImpl->context->executeReturn<QRectF>(
        [this](fz_context* ctx) -> QRectF {
            fz_rect bounds = fz_bound_page(ctx, pImpl->fzPage);
            return QRectF(
                bounds.x0,
                bounds.y0,
                bounds.x1 - bounds.x0,
                bounds.y1 - bounds.y0
            );
        }
    );
}

int Page::rotation() const {
    return pImpl->context->executeReturn<int>(
        [this](fz_context* ctx) -> int {
            if (pImpl->pdfPage) {
                pdf_obj* rotate = pdf_dict_get(ctx, pImpl->pdfPage->obj, PDF_NAME(Rotate));
                if (rotate) {
                    return pdf_to_int(ctx, rotate);
                }
            }
            return 0;
        }
    );
}

int Page::index() const {
    return pImpl->pageIndex;
}

double Page::sourceImageDpi() const {
    // Walk Resources/XObject, find the largest image (by pixel area),
    // divide its pixel dimensions by the page's bounding box in
    // inches (points/72). Returns 0.0 if no image XObject found.
    if (!pImpl->pdfPage) {
        return 0.0;
    }

    return pImpl->context->executeReturn<double>(
        [this](fz_context* ctx) -> double {
            pdf_obj* resources = pdf_page_resources(ctx, pImpl->pdfPage);
            if (!resources) return 0.0;

            pdf_obj* xobjectDict = pdf_dict_get(ctx, resources, PDF_NAME(XObject));
            if (!xobjectDict) return 0.0;

            int bestArea = 0;
            int bestW = 0;
            int bestH = 0;

            int len = pdf_dict_len(ctx, xobjectDict);
            for (int i = 0; i < len; ++i) {
                pdf_obj* val = pdf_dict_get_val(ctx, xobjectDict, i);
                val = pdf_resolve_indirect(ctx, val);
                if (!val) continue;

                pdf_obj* subtype = pdf_dict_get(ctx, val, PDF_NAME(Subtype));
                if (!pdf_name_eq(ctx, subtype, PDF_NAME(Image))) {
                    continue;
                }

                int w = pdf_dict_get_int(ctx, val, PDF_NAME(Width));
                int h = pdf_dict_get_int(ctx, val, PDF_NAME(Height));
                if (w <= 0 || h <= 0) continue;

                int area = w * h;
                if (area > bestArea) {
                    bestArea = area;
                    bestW = w;
                    bestH = h;
                }
            }

            if (bestArea == 0) return 0.0;

            fz_rect bounds = fz_bound_page(ctx, pImpl->fzPage);
            double pageWPt = bounds.x1 - bounds.x0;
            double pageHPt = bounds.y1 - bounds.y0;
            if (pageWPt <= 0.0 || pageHPt <= 0.0) return 0.0;

            // Account for page rotation: if the page is rotated 90 or 270,
            // the image pixel dimensions correspond to the rotated layout,
            // so we should match image width to page height and vice versa.
            pdf_obj* rotateObj = pdf_dict_get(ctx, pImpl->pdfPage->obj, PDF_NAME(Rotate));
            int rot = rotateObj ? pdf_to_int(ctx, rotateObj) : 0;
            rot = ((rot % 360) + 360) % 360;
            double pageWIn, pageHIn;
            if (rot == 90 || rot == 270) {
                pageWIn = pageHPt / 72.0;
                pageHIn = pageWPt / 72.0;
            } else {
                pageWIn = pageWPt / 72.0;
                pageHIn = pageHPt / 72.0;
            }

            double dpiW = static_cast<double>(bestW) / pageWIn;
            double dpiH = static_cast<double>(bestH) / pageHIn;
            // Take the larger — matches how the image was sampled in its
            // longer direction, rejects slight skew from rounding.
            return std::max(dpiW, dpiH);
        }
    );
}

double Page::sourceImageDpiAt(const QRectF& regionFitz) const {
    // Per-region native DPI (defect #2). Walk the page's image blocks WITH
    // their placement bboxes, pick the image overlapping the region the most,
    // and compute its own DPI. Unlike sourceImageDpi() (page maximum), this lets
    // a 150-DPI thumbnail and a 300-DPI photo on one page each report the
    // resolution of the image actually under the edited region.
    //
    // The lambda returns: a positive DPI when an image overlaps the region;
    // -1.0 when the page has images but none overlap (caller falls back to the
    // page-maximum DPI); 0.0 when the page has no image XObject. The fallback is
    // resolved OUTSIDE the lambda to avoid re-entering the MuPDF context.
    const double dpi = pImpl->context->executeReturn<double>(
        [this, &regionFitz](fz_context* ctx) -> double {
            fz_stext_options opts = pdfe::core::pdfeStextOptions(FZ_STEXT_PRESERVE_IMAGES);
            fz_stext_page* stextPage =
                fz_new_stext_page_from_page(ctx, pImpl->fzPage, &opts);
            if (!stextPage) return 0.0;

            // Region area (fitz points^2); 0 for a point-like (click) region.
            const double regionArea = std::max(0.0, regionFitz.width()) *
                                      std::max(0.0, regionFitz.height());
            const QPointF regionCenter = regionFitz.center();

            // Selection: among images overlapping the region, prefer the one
            // covering the LARGEST fraction of the region; on ties prefer the
            // tightest (smallest) bbox, which — because it is also drawn last —
            // is the topmost image. This picks a small 150-DPI thumbnail drawn
            // ON TOP of a big 300-DPI photo when the region sits over the
            // thumbnail, while still picking the photo for a region over only
            // the photo. (A page where one image overlays another is exactly the
            // mixed-DPI case the page-maximum heuristic gets wrong.)
            double bestFrac = 0.0;
            double bestBboxArea = 0.0;
            double bestDpi = 0.0;
            bool anyImage = false;

            for (fz_stext_block* block = stextPage->first_block; block;
                 block = block->next) {
                if (block->type != FZ_STEXT_BLOCK_IMAGE) continue;
                fz_image* img = block->u.i.image;
                if (!img || img->w <= 0 || img->h <= 0) continue;

                const double bx0 = block->bbox.x0;
                const double by0 = block->bbox.y0;
                const double bw = block->bbox.x1 - block->bbox.x0;
                const double bh = block->bbox.y1 - block->bbox.y0;
                if (bw <= 0.0 || bh <= 0.0) continue;

                anyImage = true;

                // Native DPI of this image: pixel dimensions over displayed size
                // in inches. Take the larger of the two axes (matches
                // sourceImageDpi()'s longer-direction convention, and is robust
                // to axis swaps for quarter-rotated placements).
                const double dpiW = static_cast<double>(img->w) / (bw / 72.0);
                const double dpiH = static_cast<double>(img->h) / (bh / 72.0);
                const double imgDpi = std::max(dpiW, dpiH);

                const QRectF blockRect(bx0, by0, bw, bh);
                const double bboxArea = bw * bh;
                const QRectF isect = blockRect.intersected(regionFitz);
                const double overlap = (isect.isValid() && !isect.isEmpty())
                                           ? isect.width() * isect.height()
                                           : 0.0;

                // Fraction of the region this image covers. For a point-like
                // region, "contains the centre" counts as full coverage.
                const double frac = regionArea > 0.0
                                        ? overlap / regionArea
                                        : (blockRect.contains(regionCenter) ? 1.0 : 0.0);
                if (frac <= 0.0) continue;

                const bool better =
                    frac > bestFrac + 1e-9 ||
                    (std::abs(frac - bestFrac) <= 1e-9 &&
                     (bestDpi == 0.0 || bboxArea <= bestBboxArea));
                if (better) {
                    bestFrac = frac;
                    bestBboxArea = bboxArea;
                    bestDpi = imgDpi;
                }
            }

            fz_drop_stext_page(ctx, stextPage);

            if (bestDpi > 0.0) return bestDpi;
            // No image overlapped the region: signal the caller to fall back to
            // the page-maximum DPI (-1.0), or report vector-only (0.0).
            return anyImage ? -1.0 : 0.0;
        }
    );

    if (dpi > 0.0) return dpi;
    if (dpi < 0.0) return sourceImageDpi();  // images present, none under region
    return 0.0;                              // vector-only page
}

bool Page::hasImageContentAt(const QPointF& fitzPoint) const {
    return pImpl->context->executeReturn<bool>(
        [this, &fitzPoint](fz_context* ctx) -> bool {
            fz_stext_options opts = pdfe::core::pdfeStextOptions(FZ_STEXT_PRESERVE_IMAGES);
            fz_stext_page* stextPage =
                fz_new_stext_page_from_page(ctx, pImpl->fzPage, &opts);
            if (!stextPage) return false;

            bool hit = false;
            const double px = fitzPoint.x();
            const double py = fitzPoint.y();
            for (fz_stext_block* block = stextPage->first_block; block;
                 block = block->next) {
                if (block->type != FZ_STEXT_BLOCK_IMAGE) continue;
                if (px >= block->bbox.x0 && px <= block->bbox.x1 &&
                    py >= block->bbox.y0 && py <= block->bbox.y1) {
                    hit = true;
                    break;
                }
            }
            fz_drop_stext_page(ctx, stextPage);
            return hit;
        }
    );
}

fz_page* Page::fzPage() const {
    return pImpl->fzPage;
}

std::vector<TextBlock> Page::extractText() {
    TextExtractor extractor(pImpl->context);
    return extractor.extract(this);
}

std::string Page::extractPlainText() {
    TextExtractor extractor(pImpl->context);
    return extractor.extractPlainText(this);
}

bool Page::hasTextContent() const {
    return pImpl->context->executeReturn<bool>(
        [this](fz_context* ctx) -> bool {
            fz_stext_options opts = pdfe::core::pdfeStextOptions();
            fz_stext_page* stextPage = fz_new_stext_page_from_page(ctx, pImpl->fzPage, &opts);
            bool hasText = false;
            fz_stext_block* block = stextPage->first_block;
            while (block) {
                if (block->type == FZ_STEXT_BLOCK_TEXT) {
                    const fz_stext_line* line = block->u.t.first_line;
                    while (line) {
                        if (line->first_char) {
                            hasText = true;
                            break;
                        }
                        line = line->next;
                    }
                    if (hasText) break;
                }
                block = block->next;
            }
            fz_drop_stext_page(ctx, stextPage);
            return hasText;
        }
    );
}

bool Page::isImageDominated() {
    return pImpl->context->executeReturn<bool>(
        [this](fz_context* ctx) -> bool {
            fz_rect pageBounds = fz_bound_page(ctx, pImpl->fzPage);
            float pageWidth = pageBounds.x1 - pageBounds.x0;
            float pageHeight = pageBounds.y1 - pageBounds.y0;
            float pageArea = pageWidth * pageHeight;
            if (pageArea <= 0) return false;

            // FZ_STEXT_PRESERVE_IMAGES is required to include image blocks
            fz_stext_options opts = pdfe::core::pdfeStextOptions(FZ_STEXT_PRESERVE_IMAGES);
            fz_stext_page* stextPage = fz_new_stext_page_from_page(ctx, pImpl->fzPage, &opts);

            float imageArea = 0;
            bool hasText = false;

            fz_stext_block* block = stextPage->first_block;
            while (block) {
                if (block->type == FZ_STEXT_BLOCK_IMAGE) {
                    // NORMALISE, then CLAMP TO THE PAGE. The previous
                    // `(x1-x0)*(y1-y0)` was signed: an INVERTED bbox -- what a
                    // negative-determinant CTM produces, i.e. a flipped or
                    // mirrored image placement, which is ordinary in scanner
                    // output -- SUBTRACTED coverage instead of adding it.
                    //
                    // Measured on a real 16-page scanned manual: 7 pages carried
                    // at least one inverted bbox (one carried six), and on three
                    // pages the running total went NEGATIVE, at which point no
                    // amount of image coverage could ever pass the ratio test
                    // below. See docs/audit/field-report-2026-08-28-scanned-manual.md.
                    //
                    // Clamping is the stricter normalisation and the correct
                    // one: an image hanging off the edge of the page is not
                    // coverage a reader can see, so it must not count as such.
                    //
                    // The arithmetic lives in pdfe::core::clampedCoverageArea
                    // (core/CoverageGeometry.h) so it can be TESTED as what it
                    // is: a pure geometric function. Two attempts to cover it
                    // through a whole PDF failed because synthetic `cm`
                    // placements do not produce inverted bboxes in MuPDF's
                    // stext path at all -- see the gap note in
                    // tests/test_ocr_routing_fix.cpp. An inverted bbox is
                    // trivial to construct as DATA, so the test does that.
                    // This call site is the shipped one, so the tested function
                    // and the used function cannot drift apart.
                    imageArea += static_cast<float>(clampedCoverageArea(
                        block->bbox.x0, block->bbox.y0,
                        block->bbox.x1, block->bbox.y1,
                        pageBounds.x0, pageBounds.y0,
                        pageBounds.x1, pageBounds.y1));
                } else if (block->type == FZ_STEXT_BLOCK_TEXT && !hasText) {
                    const fz_stext_line* line = block->u.t.first_line;
                    while (line) {
                        if (line->first_char) {
                            hasText = true;
                            break;
                        }
                        line = line->next;
                    }
                }
                block = block->next;
            }

            fz_drop_stext_page(ctx, stextPage);

            // A page is image-dominated if EITHER it has no extractable text
            // and any real image content (a pure scan), OR images cover >50%
            // of the page. The single-threshold rule this line used to describe
            // is no longer the whole condition -- see below.
            //
            // Console-session retry (2026-05-06): the original
            // ``return hasText && imageRatio > 0.5f`` excluded pure-scan
            // PDFs (image-only, no embedded text layer) from the
            // TRE/OCR pipeline. ``hasText`` was meant as a heuristic
            // for "this scan already has an OCR text layer", but it
            // had the perverse effect of disabling the OCR pipeline
            // for the exact PDFs that need OCR most. The TRE pipeline
            // (``ocrPipeline_->ocrPage`` etc.) is idempotent — calling
            // it on a page with an existing OCR layer just returns
            // cached results — so the gating was wrong on both branches.
            const float imageRatio = imageArea / pageArea;

            // A page with NO extractable text at all and real image content is
            // a SCAN, whatever fraction the images happen to cover -- there is
            // nothing on it to edit except pixels, so the only way to edit its
            // text is to OCR it.
            //
            // `hasText` returns here, INVERTED, and the direction is the whole
            // point. The original condition was `hasText && imageRatio > 0.5f`,
            // and the 2026-05-06 note below records dropping the `hasText` half
            // because it excluded pure scans from OCR. That was right about the
            // bug and wrong about the remedy: the variable was not useless, it
            // was backwards.
            //
            // Dropping it removed one blocker of two. On the reported 16-page
            // scan EVERY page has `hasText == false`, and with the area fix
            // above only 5 of 16 still cleared 0.5 -- so 11 pages that are
            // nothing but pixels would still have silently refused to OCR.
            //
            // The floor keeps a page that merely carries a small decorative
            // mark from being treated as a scan; it is not a coverage test.
            constexpr float kPureScanMinImageRatio = 0.05f;
            if (!hasText && imageRatio > kPureScanMinImageRatio)
                return true;

            return imageRatio > 0.5f;
        }
    );
}

} // namespace pdfe::core
