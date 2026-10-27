#include "tre/SynthesizedType3FontBundler.h"

#include "fonts/Type3FontBuilder.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "tre/EnrichedAtlas.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <QImage>

#include <QDebug>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>

namespace pdfe::tre {

using pdfe::fonts::Type3Glyph;
using pdfe::fonts::Type3FontBuilder;

namespace {

/// Pick a unique `/Font` resource name on the page following the
/// `T3subN` convention (T3sub1, T3sub2, ...). The scan finds the
/// highest existing N and returns N+1, so two back-to-back calls
/// always produce distinct names.
std::string pickFreshFontName(fz_context* ctx, pdf_obj* fontDict) {
    int maxIndex = 0;
    if (fontDict) {
        const int n = pdf_dict_len(ctx, fontDict);
        for (int i = 0; i < n; ++i) {
            pdf_obj* key = pdf_dict_get_key(ctx, fontDict, i);
            if (!key)
                continue;
            const char* nm = pdf_to_name(ctx, key);
            if (!nm || std::strncmp(nm, "T3sub", 5) != 0)
                continue;
            int idx = std::atoi(nm + 5);
            if (idx > maxIndex)
                maxIndex = idx;
        }
    }
    return std::string("T3sub") + std::to_string(maxIndex + 1);
}

/// Compute the glyph bbox + advance for a Type 3 glyph from its
/// rendered `QImage`. The Type 3 font matrix is `[0.001 0 0 0.001 0 0]`
/// so glyph-space units are 1/1000 em. We size every glyph so its
/// height equals 1000 (full em) and width is proportional to the
/// image's aspect ratio. Advance width matches the box width plus a
/// small side-bearing of 50/1000 em on each side. This is approximate
/// — phase 6 will tune advance widths from line metrics, but the
/// approximation is close enough for legible output.
Type3Glyph makeType3Glyph(char32_t cp, const QImage& src) {
    Type3Glyph g;
    g.codepoint = cp;
    g.image = src;
    if (src.isNull() || src.height() <= 0 || src.width() <= 0) {
        g.advanceWidth = 0;
        g.bbox = QRect();
        return g;
    }
    const double aspect = static_cast<double>(src.width()) /
                          static_cast<double>(src.height());
    const int boxH = 1000;
    const int boxW = std::max(1, static_cast<int>(boxH * aspect + 0.5));
    g.bbox = QRect(0, 0, boxW, boxH);
    g.advanceWidth = boxW + 100;  // 50/1000 em side bearing on each side
    return g;
}

} // namespace

SynthesizedFontBundle SynthesizedType3FontBundler::build(
    pdfe::core::MuPdfContext* context,
    pdfe::core::Page* page,
    pdfe::core::Document* document,
    const EnrichedAtlas& atlas,
    const std::vector<char32_t>& missingCodepoints)
{
    SynthesizedFontBundle bundle;
    if (!context || !page || !document || missingCodepoints.empty())
        return bundle;

    // Step 1: collect synthesizable codepoints from the EnrichedAtlas
    // and build their Type3Glyph records. Skip duplicates (caller may
    // pass repeats) and codepoints the atlas can't supply.
    std::vector<Type3Glyph> glyphs;
    std::set<char32_t> seen;
    glyphs.reserve(missingCodepoints.size());
    for (char32_t cp : missingCodepoints) {
        if (cp == 0 || cp == U' ')
            continue;
        if (!seen.insert(cp).second)
            continue;
        if (!atlas.contains(cp))
            continue;
        QImage img = atlas.glyph(cp);
        if (img.isNull())
            continue;
        glyphs.push_back(makeType3Glyph(cp, img));
    }
    if (glyphs.empty())
        return bundle;

    // Step 2: build the Type 3 font dict via Type3FontBuilder.
    void* fontRef = Type3FontBuilder::build(context, document, glyphs);
    if (!fontRef)
        return bundle;

    // Step 3: install on the page's /Resources/Font under a fresh name.
    bool installed = false;
    std::string chosenName;
    try {
        context->execute([&](fz_context* ctx) {
            fz_page* fz_pg = page->fzPage();
            if (!fz_pg)
                return;
            pdf_page* pdf_pg = pdf_page_from_fz_page(ctx, fz_pg);
            if (!pdf_pg)
                return;

            pdf_obj* resources = pdf_page_resources(ctx, pdf_pg);
            if (!resources) {
                // Page has no /Resources dict at all (possible for an
                // otherwise-empty page like test_blank.pdf). Create one
                // and attach it to the page object so we have somewhere
                // to put the /Font sub-dict.
                pdf_document* pdfDoc =
                    pdf_document_from_fz_document(ctx, document->fzDocument());
                if (!pdfDoc)
                    return;
                pdf_obj* pageObj = pdf_pg->obj;
                if (!pageObj)
                    return;
                pdf_obj* newRes = pdf_new_dict(ctx, pdfDoc, 4);
                pdf_dict_put(ctx, pageObj, PDF_NAME(Resources), newRes);
                pdf_drop_obj(ctx, newRes);
                resources = pdf_page_resources(ctx, pdf_pg);
                if (!resources)
                    return;
            }

            // Get or create /Font sub-dict.
            pdf_obj* fontDict = pdf_dict_get(ctx, resources, PDF_NAME(Font));
            if (!fontDict) {
                pdf_document* pdfDoc =
                    pdf_document_from_fz_document(ctx, document->fzDocument());
                if (!pdfDoc)
                    return;
                fontDict = pdf_new_dict(ctx, pdfDoc, 4);
                pdf_dict_put(ctx, resources, PDF_NAME(Font), fontDict);
                pdf_drop_obj(ctx, fontDict);
                fontDict = pdf_dict_get(ctx, resources, PDF_NAME(Font));
            }

            chosenName = pickFreshFontName(ctx, fontDict);
            pdf_obj* nm = pdf_new_name(ctx, chosenName.c_str());
            pdf_dict_put(ctx, fontDict, nm,
                         static_cast<pdf_obj*>(fontRef));
            pdf_drop_obj(ctx, nm);
            installed = true;
        });
    } catch (...) {
        installed = false;
    }

    if (!installed)
        return bundle;

    // Step 4: build the codepoint→code map. Type3FontBuilder packs
    // codes [1..N] in input order. After dedup in step 1, glyphs[i]
    // has code i+1.
    bundle.fontIndirectRef = fontRef;
    bundle.resourceName = chosenName;
    bundle.codepointsBundled.reserve(glyphs.size());
    for (size_t i = 0; i < glyphs.size(); ++i) {
        bundle.codepointsBundled.push_back(glyphs[i].codepoint);
        bundle.codepointToCode[glyphs[i].codepoint] =
            static_cast<uint8_t>(i + 1);
    }

    return bundle;
}

} // namespace pdfe::tre
