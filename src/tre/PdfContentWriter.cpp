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

#include <QBuffer>
#include <QDebug>
#include <QImage>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace pdfe::tre {

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

    try {
        return pImpl_->context_->executeReturn<std::optional<PageImageInfo>>(
            [&](fz_context* ctx) -> std::optional<PageImageInfo> {
                fz_page* fzPage = page->fzPage();
                fz_document* fzDoc = document->fzDocument();
                pdf_page* pdfPage = pdf_page_from_fz_page(ctx, fzPage);
                const pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, fzDoc);

                if (!pdfPage || !pdfDoc) {
                    return std::nullopt;
                }

                pdf_obj* resources = pdf_page_resources(ctx, pdfPage);
                if (!resources) {
                    return std::nullopt;
                }

                pdf_obj* xobjectDict = pdf_dict_get(ctx, resources, PDF_NAME(XObject));
                if (!xobjectDict) {
                    return std::nullopt;
                }

                // Find largest image XObject
                PageImageInfo bestInfo;
                int bestArea = 0;

                int len = pdf_dict_len(ctx, xobjectDict);
                for (int i = 0; i < len; ++i) {
                    pdf_obj* key = pdf_dict_get_key(ctx, xobjectDict, i);
                    pdf_obj* val = pdf_dict_get_val(ctx, xobjectDict, i);
                    val = pdf_resolve_indirect(ctx, val);

                    if (!val) continue;

                    pdf_obj* subtype = pdf_dict_get(ctx, val, PDF_NAME(Subtype));
                    if (!pdf_name_eq(ctx, subtype, PDF_NAME(Image))) {
                        continue;
                    }

                    int w = pdf_dict_get_int(ctx, val, PDF_NAME(Width));
                    int h = pdf_dict_get_int(ctx, val, PDF_NAME(Height));
                    int area = w * h;

                    if (area > bestArea) {
                        bestArea = area;
                        bestInfo.xobjectName = pdf_to_name(ctx, key);
                        bestInfo.width = w;
                        bestInfo.height = h;

                        pdf_obj* filter = pdf_dict_get(ctx, val, PDF_NAME(Filter));
                        bestInfo.isJpeg = filter && pdf_name_eq(ctx, filter, PDF_NAME(DCTDecode));
                    }
                }

                if (bestArea == 0) {
                    return std::nullopt;
                }
                return bestInfo;
            });
    } catch (const std::exception& e) {
        qWarning("PdfContentWriter::findPageImageXObject: %s", e.what());
        return std::nullopt;
    } catch (...) {
        qWarning("PdfContentWriter::findPageImageXObject: unknown exception");
        return std::nullopt;
    }
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

    // DECLARED IN THIS FRAME, FILLED BY THE LAMBDA (GitHub #118's hazard class).
    //
    // `info` used to be a local inside the executeReturn lambda, whose body runs
    // inside MuPdfContext's fz_try. PageImageInfo owns a std::string
    // (xobjectName), and MuPDF unwinds by longjmp, so an fz_throw from any of
    // the pdf_* calls that follow its declaration would skip its destructor and
    // leak that string's buffer. Hoisting it here means ordinary C++ unwinding
    // owns it no matter how MuPDF leaves the region.
    //
    // The lambda therefore returns bool (did we find exactly one match?) rather
    // than std::optional<PageImageInfo>, which is what kept a PageImageInfo
    // being materialised inside the guarded region. Behaviour is unchanged:
    // every former `return std::nullopt` is a `return false`, and the single
    // success path is `return true` with `info` already filled.
    PageImageInfo info;

    try {
        const bool matched = pImpl_->context_->executeReturn<bool>(
            [&](fz_context* ctx) -> bool {
                fz_page* fzPage = page->fzPage();
                fz_document* fzDoc = document->fzDocument();
                pdf_page* pdfPage = pdf_page_from_fz_page(ctx, fzPage);
                const pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, fzDoc);
                if (!pdfPage || !pdfDoc) {
                    return false;
                }

                // --- Step 1: which IMAGE is under the region? ----------------
                // Deliberately the same rule as Page::sourceImageDpiAt(): most
                // of the region covered wins; ties go to the tightest bbox,
                // which is the image drawn last and therefore on top. The two
                // must agree — a region whose DPI comes from the thumbnail but
                // whose pixels are written to the photo is exactly #262.
                fz_stext_options opts =
                    pdfe::core::pdfeStextOptions(FZ_STEXT_PRESERVE_IMAGES);
                fz_stext_page* stextPage =
                    fz_new_stext_page_from_page(ctx, fzPage, &opts);
                if (!stextPage) {
                    return false;
                }

                const double regionArea = std::max(0.0, regionFitz.width()) *
                                          std::max(0.0, regionFitz.height());
                const QPointF regionCenter = regionFitz.center();

                double bestFrac = 0.0;
                double bestBboxArea = 0.0;
                int bestW = 0;
                int bestH = 0;
                QRectF bestRect;
                bool found = false;

                for (fz_stext_block* block = stextPage->first_block; block;
                     block = block->next) {
                    if (block->type != FZ_STEXT_BLOCK_IMAGE) continue;
                    fz_image* img = block->u.i.image;
                    if (!img || img->w <= 0 || img->h <= 0) continue;

                    const double bw = block->bbox.x1 - block->bbox.x0;
                    const double bh = block->bbox.y1 - block->bbox.y0;
                    if (bw <= 0.0 || bh <= 0.0) continue;

                    const QRectF blockRect(block->bbox.x0, block->bbox.y0, bw, bh);
                    const double bboxArea = bw * bh;
                    const QRectF isect = blockRect.intersected(regionFitz);
                    const double overlap = (isect.isValid() && !isect.isEmpty())
                                               ? isect.width() * isect.height()
                                               : 0.0;
                    const double frac =
                        regionArea > 0.0
                            ? overlap / regionArea
                            : (blockRect.contains(regionCenter) ? 1.0 : 0.0);
                    if (frac <= 0.0) continue;

                    const bool better =
                        !found || frac > bestFrac + 1e-9 ||
                        (std::abs(frac - bestFrac) <= 1e-9 && bboxArea < bestBboxArea);
                    if (better) {
                        found = true;
                        bestFrac = frac;
                        bestBboxArea = bboxArea;
                        bestW = img->w;
                        bestH = img->h;
                        bestRect = blockRect;
                    }
                }

                fz_drop_stext_page(ctx, stextPage);

                if (!found) {
                    return false;
                }

                // --- Step 2: tie that image back to an XObject NAME ----------
                // The stext walk yields pixel dimensions and a placement, not a
                // resource key, and the writer needs the key. Match on (Width,
                // Height). If two image resources share dimensions the match is
                // ambiguous, and an ambiguous answer here is a silent write into
                // the wrong image -- the very failure this function exists to
                // stop -- so report nothing rather than pick.
                pdf_obj* resources = pdf_page_resources(ctx, pdfPage);
                if (!resources) return false;
                pdf_obj* xobjectDict = pdf_dict_get(ctx, resources, PDF_NAME(XObject));
                if (!xobjectDict) return false;

                // `info` is the caller's — see the note above this try block.
                int matches = 0;
                const int len = pdf_dict_len(ctx, xobjectDict);
                for (int i = 0; i < len; ++i) {
                    pdf_obj* key = pdf_dict_get_key(ctx, xobjectDict, i);
                    pdf_obj* val = pdf_resolve_indirect(
                        ctx, pdf_dict_get_val(ctx, xobjectDict, i));
                    if (!val) continue;

                    pdf_obj* subtype = pdf_dict_get(ctx, val, PDF_NAME(Subtype));
                    if (!pdf_name_eq(ctx, subtype, PDF_NAME(Image))) continue;

                    const int w = pdf_dict_get_int(ctx, val, PDF_NAME(Width));
                    const int h = pdf_dict_get_int(ctx, val, PDF_NAME(Height));
                    if (w != bestW || h != bestH) continue;

                    ++matches;
                    if (matches > 1) break;

                    info.xobjectName = pdf_to_name(ctx, key);
                    info.width = w;
                    info.height = h;
                    pdf_obj* filter = pdf_dict_get(ctx, val, PDF_NAME(Filter));
                    info.isJpeg = filter && pdf_name_eq(ctx, filter, PDF_NAME(DCTDecode));
                    info.placementFitz = bestRect;
                }

                // An ambiguous match (more than one image of these dimensions)
                // reports nothing rather than guessing — unchanged, it is just
                // spelled `false` now and `info` is left for the caller to drop.
                if (matches != 1) {
                    return false;
                }
                return true;
            });
        if (!matched) {
            return std::nullopt;
        }
        return info;
    } catch (const std::exception& e) {
        qWarning("PdfContentWriter::findImageXObjectForRegion: %s", e.what());
        return std::nullopt;
    } catch (...) {
        qWarning("PdfContentWriter::findImageXObjectForRegion: unknown exception");
        return std::nullopt;
    }
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

    // DECLARED IN THIS FRAME, FILLED BY THE LAMBDA — same reasoning as
    // findImageXObjectForRegion above: PageImageInfo owns a std::string, and a
    // longjmp out of the guarded region would skip its destructor. The lambda
    // returns bool so no PageImageInfo is ever materialised inside the region.
    PageImageInfo top;

    try {
        const bool found = pImpl_->context_->executeReturn<bool>(
            [&](fz_context* ctx) -> bool {
                fz_page* fzPage = page->fzPage();
                if (!fzPage) return false;

                fz_stext_options opts =
                    pdfe::core::pdfeStextOptions(FZ_STEXT_PRESERVE_IMAGES);
                fz_stext_page* stextPage =
                    fz_new_stext_page_from_page(ctx, fzPage, &opts);
                if (!stextPage) return false;

                // stext blocks come in content-stream order, so the LAST image
                // block covering the point is the one drawn on top of it.
                const QPointF centre = regionFitz.center();
                bool hit = false;

                for (fz_stext_block* block = stextPage->first_block; block;
                     block = block->next) {
                    if (block->type != FZ_STEXT_BLOCK_IMAGE) continue;
                    fz_image* img = block->u.i.image;
                    if (!img || img->w <= 0 || img->h <= 0) continue;

                    const double bw = block->bbox.x1 - block->bbox.x0;
                    const double bh = block->bbox.y1 - block->bbox.y0;
                    if (bw <= 0.0 || bh <= 0.0) continue;

                    const QRectF rect(block->bbox.x0, block->bbox.y0, bw, bh);
                    if (!rect.contains(centre)) continue;

                    hit = true;
                    top.width = img->w;
                    top.height = img->h;
                    top.placementFitz = rect;
                }

                fz_drop_stext_page(ctx, stextPage);
                return hit;
            });
        if (!found) {
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

    return replacePageImage(page, document, newImage, *imageInfo);
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

    try {
        pImpl_->context_->execute([&](fz_context* ctx) {
            fz_page* fzPage = page->fzPage();
            fz_document* fzDoc = document->fzDocument();
            pdf_page* pdfPage = pdf_page_from_fz_page(ctx, fzPage);
            pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, fzDoc);

            if (!pdfPage || !pdfDoc) {
                fz_throw(ctx, FZ_ERROR_GENERIC, "Invalid page or document");
            }

            // Create fz_pixmap from QImage data
            fz_colorspace* cs = fz_device_rgb(ctx);
            fz_pixmap* pix = fz_new_pixmap(ctx, cs, rgbImage.width(), rgbImage.height(), nullptr, 0);
            if (!pix) {
                fz_throw(ctx, FZ_ERROR_GENERIC, "Failed to create pixmap");
            }

            // Copy RGB data
            unsigned char* samples = fz_pixmap_samples(ctx, pix);
            int stride = fz_pixmap_stride(ctx, pix);
            for (int y = 0; y < rgbImage.height(); ++y) {
                const unsigned char* srcLine = rgbImage.constScanLine(y);
                unsigned char* dstLine = samples + y * stride;
                std::memcpy(dstLine, srcLine, static_cast<size_t>(rgbImage.width() * 3));
            }

            // Create fz_image from pixmap
            fz_image* fzImage = fz_new_image_from_pixmap(ctx, pix, nullptr);
            fz_drop_pixmap(ctx, pix);
            if (!fzImage) {
                fz_throw(ctx, FZ_ERROR_GENERIC, "Failed to create image from pixmap");
            }

            // Add to PDF
            pdf_obj* newImageObj = pdf_add_image(ctx, pdfDoc, fzImage);
            fz_drop_image(ctx, fzImage);
            if (!newImageObj) {
                fz_throw(ctx, FZ_ERROR_GENERIC, "Failed to add image to PDF");
            }

            // Replace in XObject dict
            pdf_obj* resources = pdf_page_resources(ctx, pdfPage);
            if (!resources) {
                fz_throw(ctx, FZ_ERROR_GENERIC, "Failed to get page resources");
            }

            pdf_obj* xobjectDict = pdf_dict_get(ctx, resources, PDF_NAME(XObject));
            if (!xobjectDict) {
                fz_throw(ctx, FZ_ERROR_GENERIC, "No XObject dictionary found");
            }

            pdf_dict_puts(ctx, xobjectDict, imageInfo->xobjectName.c_str(), newImageObj);
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

    try {
        return pImpl_->context_->executeReturn<std::vector<uint8_t>>(
            [&](fz_context* ctx) -> std::vector<uint8_t> {
                fz_page* fzPage = page->fzPage();
                fz_document* fzDoc = document->fzDocument();
                pdf_page* pdfPage = pdf_page_from_fz_page(ctx, fzPage);
                const pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, fzDoc);

                if (!pdfPage || !pdfDoc) {
                    return {};
                }

                pdf_obj* resources = pdf_page_resources(ctx, pdfPage);
                if (!resources) {
                    return {};
                }

                pdf_obj* xobjectDict = pdf_dict_get(ctx, resources, PDF_NAME(XObject));
                if (!xobjectDict) {
                    return {};
                }

                pdf_obj* imageObj = pdf_dict_gets(ctx, xobjectDict, imageInfo->xobjectName.c_str());
                if (!imageObj) {
                    return {};
                }

                // pdf_load_stream needs the indirect reference (not resolved)
                // to locate the stream data by object number
                fz_buffer* buf = pdf_load_stream(ctx, imageObj);
                if (!buf) {
                    return {};
                }

                size_t bufSize = fz_buffer_storage(ctx, buf, nullptr);
                std::vector<uint8_t> result(bufSize);

                unsigned char* data = nullptr;
                fz_buffer_storage(ctx, buf, &data);
                if (data) {
                    std::memcpy(result.data(), data, bufSize);
                }

                fz_drop_buffer(ctx, buf);
                return result;
            });
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

    try {
        return pImpl_->context_->executeReturn<QImage>(
            [&](fz_context* ctx) -> QImage {
                fz_page* fzPage = page->fzPage();
                fz_document* fzDoc = document->fzDocument();
                pdf_page* pdfPage = pdf_page_from_fz_page(ctx, fzPage);
                pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, fzDoc);

                if (!pdfPage || !pdfDoc) {
                    return {};
                }

                pdf_obj* resources = pdf_page_resources(ctx, pdfPage);
                if (!resources) return {};

                pdf_obj* xobjectDict = pdf_dict_get(ctx, resources, PDF_NAME(XObject));
                if (!xobjectDict) return {};

                pdf_obj* imageObj = pdf_dict_gets(ctx, xobjectDict,
                                                   imageInfo->xobjectName.c_str());
                if (!imageObj) return {};

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

                QImage result(w, h, QImage::Format_RGB888);
                for (int y = 0; y < h; ++y) {
                    const unsigned char* srcLine = samples + y * stride;
                    unsigned char* dstLine = result.scanLine(y);
                    if (n >= 3) {
                        for (int x = 0; x < w; ++x) {
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
                return result;
            });
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

    try {
        return pImpl_->context_->executeReturn<std::vector<uint8_t>>(
            [&](fz_context* ctx) -> std::vector<uint8_t> {
                fz_page* fzPage = page->fzPage();
                fz_document* fzDoc = document->fzDocument();
                pdf_page* pdfPage = pdf_page_from_fz_page(ctx, fzPage);
                const pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, fzDoc);

                if (!pdfPage || !pdfDoc) {
                    return {};
                }

                pdf_obj* contents = pdf_dict_get(ctx, pdfPage->obj, PDF_NAME(Contents));
                if (!contents) {
                    return {};
                }

                std::vector<uint8_t> result;

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

                return result;
            });
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
