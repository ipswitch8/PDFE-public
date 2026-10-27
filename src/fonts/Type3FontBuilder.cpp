#include "fonts/Type3FontBuilder.h"

#include "core/Document.h"
#include "core/MuPdfContext.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>
#include <sstream>

namespace pdfe::fonts {

namespace {

/// Convert a `QImage` to an 8-bit grayscale buffer where 0 = full ink
/// (fully-opaque foreground pixel) and 255 = paper. The Type 3 inline
/// image then renders this grayscale through `/CS /DeviceGray /BPC 8`,
/// producing a smooth anti-aliased glyph at the current text-state fill
/// color when composited over the page background.
std::vector<uint8_t> imageToGrayInk(const QImage& src) {
    std::vector<uint8_t> bytes;
    if (src.isNull())
        return bytes;
    QImage img = src.format() == QImage::Format_ARGB32_Premultiplied
                     ? src
                     : src.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const int w = img.width();
    const int h = img.height();
    bytes.reserve(static_cast<size_t>(w) * static_cast<size_t>(h));
    for (int y = 0; y < h; ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < w; ++x) {
            uint8_t alpha = static_cast<uint8_t>(qAlpha(row[x]));
            // Invert: opaque ink (alpha=255) → 0 (paint with ink color).
            // Transparent (alpha=0) → 255 (paper, no ink contribution).
            bytes.push_back(static_cast<uint8_t>(255 - alpha));
        }
    }
    return bytes;
}

/// ASCIIHex-encode raw bytes and append the standard `>` EOD terminator.
/// Uppercase hex; one space inserted every 64 chars so a viewer's parser
/// doesn't have to handle a single multi-thousand-character line.
std::string asciiHexEncode(const std::vector<uint8_t>& bytes) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(bytes.size() * 2 + bytes.size() / 32 + 4);
    for (size_t i = 0; i < bytes.size(); ++i) {
        out.push_back(kHex[(bytes[i] >> 4) & 0x0F]);
        out.push_back(kHex[bytes[i] & 0x0F]);
        if ((i & 31u) == 31u && i + 1 < bytes.size())
            out.push_back('\n');
    }
    out.push_back('>');
    return out;
}

/// Render a CharProc content stream for one glyph. Format:
///   `d0 wx 0\n`
///   `q  bboxW 0 0 bboxH bboxX bboxY cm\n`
///   `BI /W <px> /H <py> /CS /DeviceGray /BPC 8 /F /AHx ID\n`
///   `<hex bytes>>\nEI\nQ\n`
///
/// `d0` (rather than `d1`) selects the "color from text state" rendering
/// mode (PDF §9.6.5.2) — glyph ink is painted in whatever fill color the
/// content stream's text state has at glyph-show time.
std::string buildCharProcStream(const Type3Glyph& g) {
    const int pxW = g.image.width();
    const int pxH = g.image.height();
    if (pxW <= 0 || pxH <= 0)
        return {};

    const int bx = g.bbox.x();
    const int by = g.bbox.y();
    const int bw = g.bbox.width() > 0 ? g.bbox.width() : 1000;
    const int bh = g.bbox.height() > 0 ? g.bbox.height() : 1000;

    std::vector<uint8_t> gray = imageToGrayInk(g.image);
    std::string hex = asciiHexEncode(gray);

    std::ostringstream s;
    s << g.advanceWidth << " 0 d0\n"
      << "q\n"
      << bw << " 0 0 " << bh << " " << bx << " " << by << " cm\n"
      << "BI\n"
      << "/W " << pxW << " /H " << pxH
      << " /CS /DeviceGray /BPC 8 /F /AHx\n"
      << "ID\n"
      << hex << "\n"
      << "EI\n"
      << "Q\n";
    return s.str();
}

} // namespace

std::string Type3FontBuilder::nameForCodepoint(char32_t cp) {
    char buf[16];
    if (cp < 0x10000)
        std::snprintf(buf, sizeof buf, "u%04X", static_cast<unsigned>(cp));
    else
        std::snprintf(buf, sizeof buf, "u%08X", static_cast<unsigned>(cp));
    return std::string(buf);
}

void* Type3FontBuilder::build(
    pdfe::core::MuPdfContext* context,
    pdfe::core::Document* document,
    const std::vector<Type3Glyph>& glyphs)
{
    if (!context || !document || glyphs.empty())
        return nullptr;

    // De-dupe by codepoint: the first occurrence wins, later ones are
    // ignored. This keeps /CharProcs unambiguous (one entry per name).
    std::vector<Type3Glyph> uniq;
    {
        std::set<char32_t> seen;
        uniq.reserve(glyphs.size());
        for (const auto& g : glyphs) {
            if (g.image.isNull() || g.codepoint == 0)
                continue;
            if (!seen.insert(g.codepoint).second)
                continue;
            uniq.push_back(g);
        }
    }
    if (uniq.empty())
        return nullptr;

    // Encoding code assignment: pack glyphs into [1, N] so /FirstChar=1
    // and /LastChar=N. Code 0 is reserved for .notdef per spec. The
    // PDF caller (phase 5) maps Unicode → code at content-stream time.
    // The /Encoding /Differences entries map each code to the glyph's
    // /uXXXX name.

    void* result = nullptr;

    try {
        result = context->executeReturn<void*>([&](fz_context* ctx) -> void* {
            fz_document* fzDoc = document->fzDocument();
            pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, fzDoc);
            if (!pdfDoc)
                fz_throw(ctx, FZ_ERROR_GENERIC, "Type3FontBuilder: not a PDF document");

            const int n = static_cast<int>(uniq.size());

            // Compute font BBox = union of all glyph bboxes.
            int fbx0 = 0, fby0 = 0, fbx1 = 0, fby1 = 0;
            bool anyBBox = false;
            for (const auto& g : uniq) {
                if (g.bbox.isEmpty())
                    continue;
                int x0 = g.bbox.x();
                int y0 = g.bbox.y();
                int x1 = x0 + g.bbox.width();
                int y1 = y0 + g.bbox.height();
                if (!anyBBox) {
                    fbx0 = x0; fby0 = y0; fbx1 = x1; fby1 = y1;
                    anyBBox = true;
                } else {
                    fbx0 = std::min(fbx0, x0);
                    fby0 = std::min(fby0, y0);
                    fbx1 = std::max(fbx1, x1);
                    fby1 = std::max(fby1, y1);
                }
            }
            if (!anyBBox) {
                fbx0 = 0; fby0 = 0; fbx1 = 1000; fby1 = 1000;
            }

            // Per-glyph CharProc indirect refs in the same order as uniq.
            std::vector<pdf_obj*> charProcRefs;
            charProcRefs.reserve(n);

            // Stream extras dict — empty for inline-image CharProcs.
            // pdf_add_stream takes (ctx, doc, buffer, ext_dict, compressed).
            for (const auto& g : uniq) {
                std::string body = buildCharProcStream(g);
                fz_buffer* buf = fz_new_buffer_from_copied_data(
                    ctx,
                    reinterpret_cast<const unsigned char*>(body.data()),
                    body.size());
                pdf_obj* sref = nullptr;
                fz_try(ctx) {
                    sref = pdf_add_stream(ctx, pdfDoc, buf, nullptr, /*compressed=*/0);
                }
                fz_always(ctx) {
                    fz_drop_buffer(ctx, buf);
                }
                fz_catch(ctx) {
                    fz_rethrow(ctx);
                }
                if (!sref)
                    fz_throw(ctx, FZ_ERROR_GENERIC, "Failed to add CharProc stream");
                charProcRefs.push_back(sref);
            }

            // /CharProcs dict: name → stream ref
            pdf_obj* charProcs = pdf_new_dict(ctx, pdfDoc, n);
            for (int i = 0; i < n; ++i) {
                std::string nm = nameForCodepoint(uniq[i].codepoint);
                pdf_obj* keyName = pdf_new_name(ctx, nm.c_str());
                pdf_dict_put(ctx, charProcs, keyName, charProcRefs[i]);
                pdf_drop_obj(ctx, keyName);
            }

            // /Differences array: [1 /uA 2 /uB ...]
            pdf_obj* diffs = pdf_new_array(ctx, pdfDoc, n + 1);
            pdf_array_push_int(ctx, diffs, 1);
            for (int i = 0; i < n; ++i) {
                std::string nm = nameForCodepoint(uniq[i].codepoint);
                pdf_obj* nameObj = pdf_new_name(ctx, nm.c_str());
                pdf_array_push(ctx, diffs, nameObj);
                pdf_drop_obj(ctx, nameObj);
            }

            // /Encoding dict
            pdf_obj* encoding = pdf_new_dict(ctx, pdfDoc, 2);
            pdf_dict_put(ctx, encoding, PDF_NAME(Type), PDF_NAME(Encoding));
            pdf_dict_put(ctx, encoding, PDF_NAME(Differences), diffs);
            pdf_drop_obj(ctx, diffs);

            // /Widths array, length = LastChar - FirstChar + 1 = n
            pdf_obj* widths = pdf_new_array(ctx, pdfDoc, n);
            for (const auto& g : uniq) {
                pdf_array_push_int(ctx, widths, g.advanceWidth);
            }

            // /FontBBox
            pdf_obj* fontBBox = pdf_new_array(ctx, pdfDoc, 4);
            pdf_array_push_int(ctx, fontBBox, fbx0);
            pdf_array_push_int(ctx, fontBBox, fby0);
            pdf_array_push_int(ctx, fontBBox, fbx1);
            pdf_array_push_int(ctx, fontBBox, fby1);

            // /FontMatrix [0.001 0 0 0.001 0 0]
            pdf_obj* fontMatrix = pdf_new_array(ctx, pdfDoc, 6);
            pdf_array_push_real(ctx, fontMatrix, 0.001);
            pdf_array_push_int(ctx, fontMatrix, 0);
            pdf_array_push_int(ctx, fontMatrix, 0);
            pdf_array_push_real(ctx, fontMatrix, 0.001);
            pdf_array_push_int(ctx, fontMatrix, 0);
            pdf_array_push_int(ctx, fontMatrix, 0);

            // /Resources sub-dict — only need /ProcSet [/PDF /ImageB] for
            // the inline-image painting in CharProcs.
            pdf_obj* procSet = pdf_new_array(ctx, pdfDoc, 2);
            pdf_array_push(ctx, procSet, PDF_NAME(PDF));
            pdf_array_push(ctx, procSet, PDF_NAME(ImageB));
            pdf_obj* resources = pdf_new_dict(ctx, pdfDoc, 1);
            pdf_dict_put(ctx, resources, PDF_NAME(ProcSet), procSet);
            pdf_drop_obj(ctx, procSet);

            // Root font dict
            pdf_obj* fontDict = pdf_new_dict(ctx, pdfDoc, 10);
            pdf_dict_put(ctx, fontDict, PDF_NAME(Type), PDF_NAME(Font));
            pdf_dict_put(ctx, fontDict, PDF_NAME(Subtype), PDF_NAME(Type3));
            pdf_dict_put(ctx, fontDict, PDF_NAME(FontBBox), fontBBox);
            pdf_dict_put(ctx, fontDict, PDF_NAME(FontMatrix), fontMatrix);
            pdf_dict_put(ctx, fontDict, PDF_NAME(CharProcs), charProcs);
            pdf_dict_put(ctx, fontDict, PDF_NAME(Encoding), encoding);
            pdf_dict_put_int(ctx, fontDict, PDF_NAME(FirstChar), 1);
            pdf_dict_put_int(ctx, fontDict, PDF_NAME(LastChar), n);
            pdf_dict_put(ctx, fontDict, PDF_NAME(Widths), widths);
            pdf_dict_put(ctx, fontDict, PDF_NAME(Resources), resources);

            pdf_drop_obj(ctx, fontBBox);
            pdf_drop_obj(ctx, fontMatrix);
            pdf_drop_obj(ctx, charProcs);
            pdf_drop_obj(ctx, encoding);
            pdf_drop_obj(ctx, widths);
            pdf_drop_obj(ctx, resources);

            // Add as indirect object → return the indirect reference.
            pdf_obj* indirectRef = pdf_add_object_drop(ctx, pdfDoc, fontDict);

            // Per-stream refs were already added as indirect via
            // pdf_add_stream, so the dict's references hold them alive.
            // The local sref pointers in `charProcRefs` are owning
            // pdf_obj* handles — drop them now that the dict has a ref.
            for (pdf_obj* sref : charProcRefs) {
                pdf_drop_obj(ctx, sref);
            }

            return static_cast<void*>(indirectRef);
        });
    } catch (...) {
        result = nullptr;
    }

    return result;
}

} // namespace pdfe::fonts
