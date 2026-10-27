#include "fonts/EmbeddedFontAnalyzer.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/Document.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SFNT_NAMES_H
#include FT_TRUETYPE_TABLES_H
#include FT_OUTLINE_H
#include FT_BITMAP_H

#include <QDebug>

#include <algorithm>
#include <iterator>
#include <numeric>
#include <cmath>
#include <cstring>

namespace pdfe::fonts {

EmbeddedFontAnalyzer::EmbeddedFontAnalyzer(pdfe::core::MuPdfContext* context)
    : context_(context)
{
    FT_Init_FreeType(&ftLibrary_);
}

EmbeddedFontAnalyzer::~EmbeddedFontAnalyzer()
{
    if (ftLibrary_) {
        FT_Done_FreeType(ftLibrary_);
        ftLibrary_ = nullptr;
    }
}

std::optional<FontMetricProfile> EmbeddedFontAnalyzer::analyzeFont(void* fzFont)
{
    if (!fzFont || !context_)
        return std::nullopt;

    // Get font name from MuPDF
    std::string psName;
    try {
        psName = context_->executeReturn<std::string>([fzFont](fz_context* ctx) -> std::string {
            const char* n = fz_font_name(ctx, static_cast<fz_font*>(fzFont));
            return n ? std::string(n) : std::string();
        });
    } catch (...) {
        return std::nullopt;
    }

    // Get FT_Face from MuPDF to read struct fields only (no FT function calls)
    void* ftFaceVoid = nullptr;
    try {
        ftFaceVoid = context_->executeReturn<void*>([fzFont](fz_context* ctx) -> void* {
            return fz_font_ft_face(ctx, static_cast<fz_font*>(fzFont));
        });
    } catch (...) {
        return std::nullopt;
    }

    if (!ftFaceVoid)
        return std::nullopt;

    auto* face = static_cast<FT_Face>(ftFaceVoid);

    // MuPDF's fz_font.name is a char[32] buffer, so fz_font_name() truncates a
    // /BaseFont longer than 31 chars. When the name is at that cap it was very
    // likely clipped — recover the full PostScript name from the font program.
    // FT_Get_Postscript_Name is a read-only accessor (returns an internal const
    // string); unlike FT_Load_Glyph / FT_Set_*_Size it does not mutate the
    // shared face's glyph/size/transform state, so it is safe on MuPDF's face.
    // Guarded on the 31-char cap so non-truncated fonts are byte-identical.
    if (psName.size() >= 31) {
        const char* ftPs = FT_Get_Postscript_Name(face);
        if (ftPs && ftPs[0])
            psName = ftPs;
    }

    // Only read struct fields -- do NOT call other FT_ functions on MuPDF's face
    FontMetricProfile profile;
    profile.postScriptName = psName.empty() ? (face->family_name ? face->family_name : "") : psName;
    if (face->family_name)
        profile.familyName = face->family_name;
    if (face->style_name)
        profile.styleName = face->style_name;
    profile.unitsPerEm = face->units_per_EM;
    if (face->units_per_EM > 0) {
        profile.ascender = static_cast<float>(face->ascender) /
                          static_cast<float>(face->units_per_EM);
        profile.descender = static_cast<float>(face->descender) /
                           static_cast<float>(face->units_per_EM);
    }
    profile.glyphCount = static_cast<int>(face->num_glyphs);
    profile.isItalic = (face->style_flags & FT_STYLE_FLAG_ITALIC) != 0;

    // Basic weight estimate from style flags
    profile.weightClass = (face->style_flags & FT_STYLE_FLAG_BOLD) ? 700 : 400;

    // Coverage heuristic from charmap (struct field reads only, no FT calls)
    for (int i = 0; i < face->num_charmaps; ++i) {
        if (face->charmaps[i]->encoding == FT_ENCODING_UNICODE) {
            // Font has Unicode charmap -- standard fonts with many glyphs cover Latin Basic
            if (face->num_glyphs >= 90)
                profile.hasLatinBasic = true;
            if (face->num_glyphs >= 300)
                profile.hasLatinExtA = true;
            break;
        }
    }

    // Script heuristic from font name (cannot call FT_ functions on MuPDF's face).
    // Common PDF fonts are Latin-primary; detect non-Latin by name patterns.
    auto nameContains = [&](const char* substr) {
        return profile.familyName.find(substr) != std::string::npos ||
               profile.postScriptName.find(substr) != std::string::npos;
    };
    if (nameContains("Arabic") || nameContains("Naskh") || nameContains("Scheherazade") ||
        nameContains("Amiri") || nameContains("Sakkal")) {
        profile.primaryScript = FontScript::Arabic;
    } else if (nameContains("CJK") || nameContains("SimSun") || nameContains("SimHei") ||
               nameContains("Ming") || nameContains("Gothic") || nameContains("Mincho")) {
        profile.primaryScript = FontScript::CJK;
    } else if (nameContains("Cyrillic")) {
        profile.primaryScript = FontScript::Cyrillic;
    } else if (nameContains("Devanagari") || nameContains("Mangal") || nameContains("Kokila")) {
        profile.primaryScript = FontScript::Devanagari;
    } else if (profile.hasLatinBasic) {
        profile.primaryScript = FontScript::Latin;
    }
    // else remains Unknown

    // Defect #8: this quick (MuPDF-owned face) path cannot walk the cmap, so it
    // derives coverage from the name signal plus the Latin-basic heuristic. Seed
    // supportedScripts from what we can determine: the detected primary script
    // and (when present) Latin-basic interop coverage that almost all PDF text
    // fonts carry. This stays a coarse hint here; the deep analyzeOwnedFace path
    // (classifyCoverage) produces the authoritative per-range set.
    profile.supportedScripts.clear();
    if (profile.primaryScript != FontScript::Unknown)
        profile.supportedScripts.insert(profile.primaryScript);
    if (profile.hasLatinBasic)
        profile.supportedScripts.insert(FontScript::Latin);

    return profile;
}

std::vector<FontMetricProfile> EmbeddedFontAnalyzer::analyzePageFonts(
    pdfe::core::Page* page,
    const pdfe::core::Document* document)
{
    std::vector<FontMetricProfile> results;
    if (!page || !document || !context_ || !ftLibrary_)
        return results;

    // Step 1: Get page resource pointers (pdf_obj* is valid while doc is open)
    struct PageFontInfo {
        pdf_obj* fontDict = nullptr;
        pdf_obj* resources = nullptr;
        pdf_document* pdfDoc = nullptr;
        int fontCount = 0;
    };
    PageFontInfo pageInfo{};

    try {
        context_->execute([&](fz_context* ctx) {
            fz_page* fzPage = page->fzPage();
            pdf_page* pdfPage = pdf_page_from_fz_page(ctx, fzPage);
            if (!pdfPage)
                return;

            pageInfo.resources = pdf_page_resources(ctx, pdfPage);
            if (!pageInfo.resources)
                return;

            pageInfo.fontDict = pdf_dict_get(ctx, pageInfo.resources, PDF_NAME(Font));
            if (pageInfo.fontDict) {
                pageInfo.fontCount = pdf_dict_len(ctx, pageInfo.fontDict);
                pageInfo.pdfDoc = pdf_document_from_fz_document(ctx, document->fzDocument());
            }
        });
    } catch (...) {
        return results;
    }

    if (pageInfo.fontCount == 0 || !pageInfo.fontDict)
        return results;

    // Step 2: Extract each font's data in isolated execute() calls.
    // For embedded fonts: extract font file data and create our own FT_Face.
    // For non-embedded fonts: use pdf_load_font fallback with struct-field-only analysis.
    struct FontInfo {
        std::string psName;
        std::vector<unsigned char> fontData; // Empty if not embedded
        bool hasEmbeddedData = false;
    };
    std::vector<FontInfo> fonts;

    for (int i = 0; i < pageInfo.fontCount; ++i) {
        FontInfo info;

        try {
            context_->execute([&](fz_context* ctx) {
                pdf_obj* fontRef = pdf_dict_get_val(ctx, pageInfo.fontDict, i);
                if (!fontRef)
                    return;

                // Resolve indirect reference
                fontRef = pdf_resolve_indirect(ctx, fontRef);
                if (!fontRef)
                    return;

                // Get font name from the PDF dict
                pdf_obj* baseFont = pdf_dict_get(ctx, fontRef, PDF_NAME(BaseFont));
                if (baseFont)
                    info.psName = pdf_to_name(ctx, baseFont);

                // Type0 (composite) fonts wrap a descendant CIDFont, and
                // the FontDescriptor + embedded FontFile live on the
                // descendant — not the top-level Type0 font dict. Walk
                // /DescendantFonts[0] when present so we find the actual
                // embedded font stream for CharisSIL, AdobeIdentityCMap,
                // and other CID-keyed fonts. (Without this, Type0 fonts
                // fall through to the "non-embedded fallback" path even
                // though the file IS present in the PDF.)
                pdf_obj* fontDescriptor = pdf_dict_get(ctx, fontRef, PDF_NAME(FontDescriptor));
                if (!fontDescriptor) {
                    pdf_obj* descendants = pdf_dict_get(ctx, fontRef, PDF_NAME(DescendantFonts));
                    if (descendants && pdf_array_len(ctx, descendants) > 0) {
                        pdf_obj* cidFont = pdf_resolve_indirect(
                            ctx, pdf_array_get(ctx, descendants, 0));
                        if (cidFont) {
                            fontDescriptor = pdf_dict_get(ctx, cidFont, PDF_NAME(FontDescriptor));
                        }
                    }
                }
                if (!fontDescriptor) {
                    // No FontDescriptor anywhere -- mark as non-embedded
                    return;
                }

                // Try FontFile2 (TrueType), FontFile3 (CFF/OpenType), FontFile (Type1)
                pdf_obj* fontFile = pdf_dict_get(ctx, fontDescriptor, PDF_NAME(FontFile2));
                if (!fontFile)
                    fontFile = pdf_dict_get(ctx, fontDescriptor, PDF_NAME(FontFile3));
                if (!fontFile)
                    fontFile = pdf_dict_get(ctx, fontDescriptor, PDF_NAME(FontFile));

                if (fontFile) {
                    // Embedded font -- extract data
                    fz_buffer* buf = pdf_load_stream(ctx, fontFile);
                    if (buf) {
                        unsigned char* data = nullptr;
                        size_t len = fz_buffer_storage(ctx, buf, &data);
                        if (data && len > 0) {
                            info.fontData.assign(data, data + len);
                            info.hasEmbeddedData = true;
                        }
                        fz_drop_buffer(ctx, buf);
                    }
                }
                // If no fontFile, psName is still set for fallback
            });
        } catch (...) {
            continue; // Skip this font on MuPDF error
        }

        if (!info.psName.empty())
            fonts.push_back(std::move(info));
    }

    // Step 3: Analyze each font
    for (auto& fi : fonts) {
        if (fi.hasEmbeddedData) {
            // Embedded font -- create our own FT_Face for full analysis
            FT_Face face = nullptr;
            FT_Error err = FT_New_Memory_Face(
                ftLibrary_,
                fi.fontData.data(),
                static_cast<FT_Long>(fi.fontData.size()),
                0,
                &face
            );

            if (err != 0 || !face)
                continue;

            auto profile = analyzeOwnedFace(face, fi.psName);
            if (profile.isValid())
                results.push_back(std::move(profile));

            FT_Done_Face(face);
        } else {
            // Non-embedded font -- use MuPDF's pdf_load_font for basic profile
            fz_font* keptFont = nullptr;
            try {
                context_->execute([&](fz_context* ctx) {
                    // Find the font ref again
                    for (int j = 0; j < pageInfo.fontCount; ++j) {
                        pdf_obj* ref = pdf_dict_get_val(ctx, pageInfo.fontDict, j);
                        if (!ref) continue;
                        ref = pdf_resolve_indirect(ctx, ref);
                        if (!ref) continue;
                        pdf_obj* bf = pdf_dict_get(ctx, ref, PDF_NAME(BaseFont));
                        if (bf && fi.psName == pdf_to_name(ctx, bf)) {
                            pdf_font_desc* fontDesc = pdf_load_font(
                                ctx, pageInfo.pdfDoc, pageInfo.resources, ref);
                            if (fontDesc && fontDesc->font) {
                                keptFont = fz_keep_font(ctx, fontDesc->font);
                                pdf_drop_font(ctx, fontDesc);
                            } else if (fontDesc) {
                                pdf_drop_font(ctx, fontDesc);
                            }
                            break;
                        }
                    }
                });
            } catch (...) {
                continue;
            }

            if (!keptFont)
                continue;

            auto profile = analyzeFont(keptFont);
            if (profile.has_value() && profile->isValid())
                results.push_back(std::move(*profile));

            // Release font reference
            try {
                context_->execute([&](fz_context* ctx) {
                    fz_drop_font(ctx, keptFont);
                });
            } catch (...) {}
        }
    }

    return results;
}

std::optional<FontMetricProfile> EmbeddedFontAnalyzer::analyzeFromFile(const std::string& filePath)
{
    if (!ftLibrary_ || filePath.empty())
        return std::nullopt;

    FT_Face face = nullptr;
    FT_Error err = FT_New_Face(ftLibrary_, filePath.c_str(), 0, &face);

    if (err != 0 || !face)
        return std::nullopt;

    // Get PostScript name from face (family_name as fallback)
    std::string psName;
    if (face->family_name)
        psName = face->family_name;

    // Try to get actual PostScript name from name table if available
    const TT_Postscript* post = static_cast<TT_Postscript*>(
        FT_Get_Sfnt_Table(face, FT_SFNT_POST));
    if (post && face->family_name) {
        // PostScript name typically combines family and style
        psName = face->family_name;
        if (face->style_name && face->style_name[0] != '\0') {
            psName += "-";
            psName += face->style_name;
        }
    }

    // Perform full analysis using our owned face
    auto profile = analyzeOwnedFace(face, psName);

    // Set the file path in the profile
    profile.filePath = filePath;

    FT_Done_Face(face);

    if (!profile.isValid())
        return std::nullopt;

    return profile;
}

std::optional<FontMetricProfile> EmbeddedFontAnalyzer::analyzeFace(
    FT_Face face, const std::string& psName)
{
    if (!face)
        return std::nullopt;

    // Derive a PostScript name when the caller didn't supply one, mirroring the
    // analyzeFromFile fallback (family[-style]).
    std::string name = psName;
    if (name.empty() && face->family_name) {
        name = face->family_name;
        if (face->style_name && face->style_name[0] != '\0') {
            name += "-";
            name += face->style_name;
        }
    }

    // Full deep analysis on the caller-owned face. The caller retains ownership
    // of @p face, so we do NOT FT_Done_Face here.
    auto profile = analyzeOwnedFace(face, name);

    if (!profile.isValid())
        return std::nullopt;

    return profile;
}

std::vector<char32_t> EmbeddedFontAnalyzer::findMissingGlyphs(
    const std::string& fontFilePath,
    const std::u32string& text)
{
    std::vector<char32_t> missing;

    if (!ftLibrary_ || fontFilePath.empty())
        return missing; // Return empty if invalid inputs

    FT_Face face = nullptr;
    FT_Error err = FT_New_Face(ftLibrary_, fontFilePath.c_str(), 0, &face);

    if (err != 0 || !face) {
        // If we can't load the font, all codepoints are missing
        std::copy_if(text.begin(), text.end(),
            std::back_inserter(missing),
            [](char32_t cp) { return cp != 0x20; });
        return missing;
    }

    // Check each codepoint in the text
    for (char32_t cp : text) {
        // Skip space (0x20) - it's always renderable even if no glyph
        if (cp == 0x20)
            continue;

        FT_UInt glyphIndex = FT_Get_Char_Index(face, static_cast<FT_ULong>(cp));
        if (glyphIndex == 0) {
            // No glyph for this codepoint
            missing.push_back(cp);
        }
    }

    FT_Done_Face(face);

    return missing;
}

FontMetricProfile EmbeddedFontAnalyzer::analyzeOwnedFace(FT_Face face, const std::string& psName)
{
    FontMetricProfile profile = extractTableMetrics(face);

    // Identity
    if (face->family_name)
        profile.familyName = face->family_name;
    if (face->style_name)
        profile.styleName = face->style_name;
    profile.postScriptName = psName;
    if (profile.postScriptName.empty() && !profile.familyName.empty())
        profile.postScriptName = profile.familyName;

    // Measured metrics
    profile.stemWeightV = measureStemWeightV(face);
    profile.stemWeightH = measureStemWeightH(face);
    profile.serifExtent = measureSerifExtent(face);
    // Serif classification: prefer PANOSE byte 1 (serifStyle) from OS/2 table,
    // which is authoritative for Latin text fonts. Fall back to measured
    // serifExtent for fonts without valid PANOSE data.
    //
    // PANOSE serifStyle (byte index 1, for Family Kind 2 = Latin Text):
    //   0  = Any/unspecified
    //   1  = No Fit
    //   2-10  = Serif variants (Cove, Square, Thin, Bone, etc.)
    //   11-15 = Sans-serif variants (Normal, Obtuse, Perpendicular, Flared, Rounded)
    uint8_t panoseFamily = profile.panose[0];
    uint8_t panoseSerif  = profile.panose[1];
    if (panoseFamily == 2 && panoseSerif >= 2) {
        // Latin Text font with valid serif classification
        profile.isSerif = (panoseSerif <= 10);  // 2-10 = serif, 11-15 = sans
    } else {
        // No PANOSE or non-Latin: fall back to glyph measurement
        static constexpr float kSerifThresholdClean = 0.3f;
        profile.isSerif = profile.serifExtent > kSerifThresholdClean;
    }
    profile.avgCharWidth = measureAvgCharWidth(face);

    // Computed ratios
    if (profile.capHeight > 0.0f) {
        profile.xToCapRatio = profile.xHeight / profile.capHeight;
        profile.ascToCapRatio = profile.ascender / profile.capHeight;
    }

    // Italic detection
    profile.isItalic = (face->style_flags & FT_STYLE_FLAG_ITALIC) != 0;
    {
        const TT_Postscript* post = static_cast<TT_Postscript*>(
            FT_Get_Sfnt_Table(face, FT_SFNT_POST));
        if (post) {
            profile.italicAngle = static_cast<float>(post->italicAngle) / 65536.0f;
        }
    }
    // Reconcile: if post table reports non-zero italic angle, mark as italic
    // regardless of whether FT_STYLE_FLAG_ITALIC is set. Some fonts omit the
    // style flag for oblique/slanted variants.
    if (!profile.isItalic && std::abs(profile.italicAngle) > 2.0f) {
        profile.isItalic = true;
    }

    // Coverage and monospace
    classifyCoverage(face, profile);
    measureMonospace(face, profile);

    // Glyph count
    profile.glyphCount = static_cast<int>(face->num_glyphs);

    // Actually-rendered visible extent (measured, not OS/2-declared).
    // Bypasses font designer declarations that disagree with the
    // rendered output — e.g., CharisSIL declares ascender=0.73 +
    // |descender|=0.27 = 1.0 em, but its rendered glyphs only span
    // 0.61 of em. We need the rendered value for sizing overlays.
    // measureRenderedExtent now also populates renderedMaxTop and
    // renderedMinBottom on the profile so callers can position the
    // overlay baseline using the source font's actual ascender ratio.
    profile.renderedVisibleExtent = measureRenderedExtent(face, profile);

    return profile;
}

FontMetricProfile EmbeddedFontAnalyzer::extractTableMetrics(FT_Face face)
{
    FontMetricProfile profile;
    if (!face)
        return profile;

    profile.unitsPerEm = face->units_per_EM;

    // Normalize ascender/descender to 0.0-1.0 of UPM
    if (face->units_per_EM > 0) {
        profile.ascender = static_cast<float>(face->ascender) /
                          static_cast<float>(face->units_per_EM);
        profile.descender = static_cast<float>(face->descender) /
                           static_cast<float>(face->units_per_EM);
    }

    // OS/2 table provides the most reliable metrics
    const TT_OS2* os2 = static_cast<TT_OS2*>(FT_Get_Sfnt_Table(face, FT_SFNT_OS2));
    if (os2) {
        profile.weightClass = os2->usWeightClass;
        profile.widthClass = os2->usWidthClass;
        std::memcpy(profile.panose, os2->panose, 10);

        if (face->units_per_EM > 0) {
            if (os2->sxHeight > 0)
                profile.xHeight = static_cast<float>(os2->sxHeight) /
                                 static_cast<float>(face->units_per_EM);
            if (os2->sCapHeight > 0)
                profile.capHeight = static_cast<float>(os2->sCapHeight) /
                                   static_cast<float>(face->units_per_EM);
        }
    }

    // Fallback: measure x-height from glyph 'x' if not in OS/2
    if (profile.xHeight <= 0.0f && face->units_per_EM > 0) {
        FT_UInt gidx = FT_Get_Char_Index(face, 'x');
        if (gidx != 0 && FT_Load_Glyph(face, gidx, FT_LOAD_NO_SCALE) == 0) {
            profile.xHeight = static_cast<float>(face->glyph->metrics.height) /
                             static_cast<float>(face->units_per_EM);
        }
    }

    // Fallback: measure cap-height from glyph 'H' if not in OS/2
    if (profile.capHeight <= 0.0f && face->units_per_EM > 0) {
        FT_UInt gidx = FT_Get_Char_Index(face, 'H');
        if (gidx != 0 && FT_Load_Glyph(face, gidx, FT_LOAD_NO_SCALE) == 0) {
            profile.capHeight = static_cast<float>(face->glyph->metrics.height) /
                               static_cast<float>(face->units_per_EM);
        }
    }

    return profile;
}

float EmbeddedFontAnalyzer::measureStemWeightV(FT_Face face)
{
    if (!face || face->units_per_EM == 0)
        return 0.0f;

    if (!(face->face_flags & FT_FACE_FLAG_SCALABLE))
        return 0.0f;

    for (char32_t ch : {U'l', U'I', U'i'}) {
        FT_UInt gidx = FT_Get_Char_Index(face, static_cast<FT_ULong>(ch));
        if (gidx == 0)
            continue;

        if (FT_Set_Char_Size(face, 0, 1000 * 64, 72, 72) != 0)
            continue;

        if (FT_Load_Glyph(face, gidx, FT_LOAD_RENDER) != 0)
            continue;

        const FT_Bitmap& bmp = face->glyph->bitmap;
        if (bmp.rows == 0 || bmp.width == 0 || !bmp.buffer)
            continue;

        // Cast horizontal ray at 50% of glyph height
        int midRow = static_cast<int>(bmp.rows) / 2;
        if (midRow >= static_cast<int>(bmp.rows))
            continue;

        const unsigned char* row = bmp.buffer + midRow * bmp.pitch;

        int blackPixels = 0;
        for (unsigned int x = 0; x < bmp.width; ++x) {
            if (row[x] > 127)
                ++blackPixels;
        }

        float normalized = static_cast<float>(blackPixels) / 1000.0f;
        return normalized;
    }

    return 0.0f;
}

float EmbeddedFontAnalyzer::measureStemWeightH(FT_Face face)
{
    if (!face || face->units_per_EM == 0)
        return 0.0f;

    if (!(face->face_flags & FT_FACE_FLAG_SCALABLE))
        return 0.0f;

    for (char32_t ch : {U'e', U'z'}) {
        FT_UInt gidx = FT_Get_Char_Index(face, static_cast<FT_ULong>(ch));
        if (gidx == 0)
            continue;

        if (FT_Set_Char_Size(face, 0, 1000 * 64, 72, 72) != 0)
            continue;

        if (FT_Load_Glyph(face, gidx, FT_LOAD_RENDER) != 0)
            continue;

        const FT_Bitmap& bmp = face->glyph->bitmap;
        if (bmp.rows == 0 || bmp.width == 0 || !bmp.buffer)
            continue;

        int midCol = static_cast<int>(bmp.width) / 2;
        if (midCol >= static_cast<int>(bmp.width))
            continue;

        int blackPixels = 0;
        for (unsigned int y = 0; y < bmp.rows; ++y) {
            if (bmp.buffer[y * bmp.pitch + midCol] > 127)
                ++blackPixels;
        }

        float normalized = static_cast<float>(blackPixels) / 1000.0f;
        return normalized;
    }

    return 0.0f;
}

float EmbeddedFontAnalyzer::measureSerifExtent(FT_Face face)
{
    if (!face || face->units_per_EM == 0)
        return 0.0f;

    if (!(face->face_flags & FT_FACE_FLAG_SCALABLE))
        return 0.0f;

    FT_UInt gidx = FT_Get_Char_Index(face, 'I');
    if (gidx == 0)
        return 0.0f;

    if (FT_Set_Char_Size(face, 0, 1000 * 64, 72, 72) != 0)
        return 0.0f;

    if (FT_Load_Glyph(face, gidx, FT_LOAD_RENDER) != 0)
        return 0.0f;

    const FT_Bitmap& bmp = face->glyph->bitmap;
    if (bmp.rows < 4 || bmp.width == 0 || !bmp.buffer)
        return 0.0f;

    int baselineRow = static_cast<int>(bmp.rows) - static_cast<int>(bmp.rows) / 10 - 1;
    if (baselineRow < 0) baselineRow = static_cast<int>(bmp.rows) - 1;
    int midRow = static_cast<int>(bmp.rows) / 2;

    auto measureExtent = [&](int row) -> int {
        if (row < 0 || row >= static_cast<int>(bmp.rows))
            return 0;
        const unsigned char* rowData = bmp.buffer + row * bmp.pitch;
        int count = 0;
        for (unsigned int x = 0; x < bmp.width; ++x) {
            if (rowData[x] > 127)
                ++count;
        }
        return count;
    };

    int baselineExtent = measureExtent(baselineRow);
    int stemExtent = measureExtent(midRow);

    if (stemExtent <= 0)
        return 0.0f;

    float ratio = static_cast<float>(baselineExtent - stemExtent) /
                  static_cast<float>(stemExtent);
    return std::max(0.0f, ratio);
}

void EmbeddedFontAnalyzer::classifyCoverage(FT_Face face, FontMetricProfile& profile)
{
    if (!face)
        return;

    // Count glyphs per script range for primary script detection
    int latinCount = 0;     // U+0020-024F (Basic Latin + Latin Extended A/B)
    int arabicCount = 0;    // U+0600-06FF + U+0750-077F + U+FB50-FDFF + U+FE70-FEFF
    int cyrillicCount = 0;  // U+0400-052F
    int cjkCount = 0;       // U+4E00-9FFF + U+3400-4DBF + U+3000-303F
    int devanagariCount = 0;// U+0900-097F

    FT_ULong charcode;
    FT_UInt glyphIndex;

    charcode = FT_Get_First_Char(face, &glyphIndex);
    while (glyphIndex != 0) {
        if (charcode >= 0x0020 && charcode <= 0x007E) {
            profile.hasLatinBasic = true;
            ++latinCount;
        } else if (charcode >= 0x0080 && charcode <= 0x024F) {
            ++latinCount;
            if (charcode >= 0x0100 && charcode <= 0x017F)
                profile.hasLatinExtA = true;
        } else if (charcode >= 0x0400 && charcode <= 0x052F) {
            profile.hasCyrillic = true;
            ++cyrillicCount;
        } else if ((charcode >= 0x0600 && charcode <= 0x06FF) ||
                   (charcode >= 0x0750 && charcode <= 0x077F) ||
                   (charcode >= 0xFB50 && charcode <= 0xFDFF) ||
                   (charcode >= 0xFE70 && charcode <= 0xFEFF)) {
            ++arabicCount;
        } else if ((charcode >= 0x4E00 && charcode <= 0x9FFF) ||
                   (charcode >= 0x3400 && charcode <= 0x4DBF) ||
                   (charcode >= 0x3000 && charcode <= 0x303F)) {
            profile.hasCJK = true;
            ++cjkCount;
        } else if (charcode >= 0x0900 && charcode <= 0x097F) {
            ++devanagariCount;
        }

        charcode = FT_Get_Next_Char(face, charcode, &glyphIndex);
    }

    // Determine primary script.
    // Strategy: Use font name as a strong signal first, then fall back
    // to glyph-count heuristics. Fonts like "Scheherazade" are Arabic-primary
    // even though they include ~95 Basic Latin glyphs for interoperability.
    // Multi-script fonts like Arial/Calibri are Latin-primary even with
    // large Arabic/Cyrillic blocks.

    // 1. Name-based override: known non-Latin font families
    auto nameContainsCI = [&profile](const char* substr) {
        auto containsCI = [](const std::string& haystack, const char* needle) {
            std::string lower = haystack;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            std::string needleLower(needle);
            std::transform(needleLower.begin(), needleLower.end(), needleLower.begin(), ::tolower);
            return lower.find(needleLower) != std::string::npos;
        };
        return containsCI(profile.familyName, substr) ||
               containsCI(profile.postScriptName, substr);
    };

    bool nameOverride = false;
    if (nameContainsCI("Arabic") || nameContainsCI("Naskh") ||
        nameContainsCI("Scheherazade") || nameContainsCI("Amiri") ||
        nameContainsCI("Sakkal") || nameContainsCI("Tahoma")) {
        if (arabicCount > 0) {
            profile.primaryScript = FontScript::Arabic;
            nameOverride = true;
        }
    } else if (nameContainsCI("SimSun") || nameContainsCI("SimHei") ||
               nameContainsCI("Ming") || nameContainsCI("Mincho") ||
               nameContainsCI("Gothic") || nameContainsCI("Gulim") ||
               nameContainsCI("Batang") || nameContainsCI("Dotum") ||
               nameContainsCI("Malgun") || nameContainsCI("NanumGothic") ||
               nameContainsCI("MS UI Gothic") ||
               (nameContainsCI("CJK") && cjkCount > 0)) {
        profile.primaryScript = FontScript::CJK;
        nameOverride = true;
    } else if (nameContainsCI("Devanagari") || nameContainsCI("Mangal") ||
               nameContainsCI("Kokila")) {
        if (devanagariCount > 0) {
            profile.primaryScript = FontScript::Devanagari;
            nameOverride = true;
        }
    } else if (nameContainsCI("Myanmar") || nameContainsCI("Padauk") ||
               nameContainsCI("Javanese") || nameContainsCI("Khmer") ||
               nameContainsCI("Mongolian") || nameContainsCI("Tibetan") ||
               nameContainsCI("Himalaya") || nameContainsCI("Leelawadee") ||
               nameContainsCI("Nirmala") || nameContainsCI("Lao") ||
               nameContainsCI("Baiti") || nameContainsCI("PhagsPa") ||
               nameContainsCI("Tai Lue") || nameContainsCI("Yi Baiti") ||
               nameContainsCI("Ebrima") || nameContainsCI("Gadugi")) {
        // Southeast Asian / South Asian / Central Asian UI fonts.
        // These have Latin Basic coverage for interop but are primarily
        // designed for non-Latin scripts. Mark as non-Latin so the script
        // mismatch penalty applies when matching Latin text.
        profile.primaryScript = FontScript::Other;
        nameOverride = true;
    }

    // 2. Glyph-count heuristic (only if name didn't override)
    if (!nameOverride) {
        const int latinThreshold = 80;
        const int dominanceRatio = 3;

        if (latinCount >= latinThreshold) {
            profile.primaryScript = FontScript::Latin;

            int maxNonLatin = 0;
            FontScript maxNonLatinScript = FontScript::Unknown;

            if (arabicCount > maxNonLatin) {
                maxNonLatin = arabicCount;
                maxNonLatinScript = FontScript::Arabic;
            }
            if (cyrillicCount > maxNonLatin) {
                maxNonLatin = cyrillicCount;
                maxNonLatinScript = FontScript::Cyrillic;
            }
            if (cjkCount > maxNonLatin) {
                maxNonLatin = cjkCount;
                maxNonLatinScript = FontScript::CJK;
            }
            if (devanagariCount > maxNonLatin) {
                maxNonLatin = devanagariCount;
                maxNonLatinScript = FontScript::Devanagari;
            }

            if (maxNonLatin > latinCount * dominanceRatio) {
                profile.primaryScript = maxNonLatinScript;
            }
        } else {
            // Sparse or no Latin coverage -- use simple max
            int maxCount = latinCount;
            profile.primaryScript = (latinCount > 0) ? FontScript::Latin : FontScript::Unknown;

            if (arabicCount > maxCount) {
                maxCount = arabicCount;
                profile.primaryScript = FontScript::Arabic;
            }
            if (cyrillicCount > maxCount) {
                maxCount = cyrillicCount;
                profile.primaryScript = FontScript::Cyrillic;
            }
            if (cjkCount > maxCount) {
                maxCount = cjkCount;
                profile.primaryScript = FontScript::CJK;
            }
            if (devanagariCount > maxCount) {
                maxCount = devanagariCount;
                profile.primaryScript = FontScript::Devanagari;
            }

            if (maxCount == 0) {
                profile.primaryScript = FontScript::Unknown;
            }
        }
    } // !nameOverride

    // Defect #8: build the FULL supported-script set from glyph-range coverage,
    // not just the single dominant primaryScript. A multi-script font (e.g.
    // NotoSans with Latin+Arabic+CJK) must report every script it actually
    // carries so FontMatcher does not penalize it for the non-majority scripts.
    // A script counts as supported when it has at least kScriptCoverageMin real
    // glyphs in that range (a handful of interop glyphs is not "support").
    profile.supportedScripts.clear();
    constexpr int kScriptCoverageMin = 8;
    if (latinCount >= kScriptCoverageMin)
        profile.supportedScripts.insert(FontScript::Latin);
    if (arabicCount >= kScriptCoverageMin)
        profile.supportedScripts.insert(FontScript::Arabic);
    if (cyrillicCount >= kScriptCoverageMin)
        profile.supportedScripts.insert(FontScript::Cyrillic);
    if (cjkCount >= kScriptCoverageMin)
        profile.supportedScripts.insert(FontScript::CJK);
    if (devanagariCount >= kScriptCoverageMin)
        profile.supportedScripts.insert(FontScript::Devanagari);
    // Always include the resolved primaryScript (covers name-override fonts such
    // as Tahoma->Arabic or SE-Asian->Other whose count thresholds may not trip).
    if (profile.primaryScript != FontScript::Unknown)
        profile.supportedScripts.insert(profile.primaryScript);
}

void EmbeddedFontAnalyzer::measureMonospace(FT_Face face, FontMetricProfile& profile)
{
    if (!face || face->units_per_EM == 0)
        return;

    std::vector<float> advances;
    for (char32_t ch = U'a'; ch <= U'z'; ++ch) {
        FT_UInt gidx = FT_Get_Char_Index(face, static_cast<FT_ULong>(ch));
        if (gidx == 0)
            continue;

        if (FT_Load_Glyph(face, gidx, FT_LOAD_NO_SCALE) != 0)
            continue;

        float adv = static_cast<float>(face->glyph->metrics.horiAdvance) /
                    static_cast<float>(face->units_per_EM);
        advances.push_back(adv);
    }

    if (advances.size() < 10)
        return;

    float sum = std::accumulate(advances.begin(), advances.end(), 0.0f);
    float mean = sum / static_cast<float>(advances.size());

    if (mean <= 0.0f)
        return;

    const bool anyDiverges = std::any_of(advances.begin(), advances.end(),
        [mean](float a) { return std::abs(a - mean) / mean > 0.05f; });
    profile.isMonospace = !anyDiverges;
}

float EmbeddedFontAnalyzer::measureAvgCharWidth(FT_Face face)
{
    if (!face || face->units_per_EM == 0)
        return 0.0f;

    float sum = 0.0f;
    int count = 0;

    for (char32_t ch = U'a'; ch <= U'z'; ++ch) {
        FT_UInt gidx = FT_Get_Char_Index(face, static_cast<FT_ULong>(ch));
        if (gidx == 0)
            continue;

        if (FT_Load_Glyph(face, gidx, FT_LOAD_NO_SCALE) != 0)
            continue;

        sum += static_cast<float>(face->glyph->metrics.horiAdvance) /
               static_cast<float>(face->units_per_EM);
        ++count;
    }

    return count > 0 ? sum / static_cast<float>(count) : 0.0f;
}

float EmbeddedFontAnalyzer::measureRenderedExtent(FT_Face face, FontMetricProfile& profile)
{
    if (!face || face->units_per_EM == 0)
        return 0.0f;

    // Render at a sample em size and measure the actual bitmap bounds.
    // Subset fonts often have no Unicode charmap, so iterate by glyph
    // index and skip glyph 0 (.notdef) and any zero-extent glyphs.
    constexpr int kProbePx = 100;
    if (FT_Set_Pixel_Sizes(face, 0, kProbePx) != 0)
        return 0.0f;

    int maxTop = 0;
    int minBottom = 0;
    int sampled = 0;
    int loadFailed = 0;
    int emptyBitmap = 0;

    // Cap at 256 glyphs to bound cost on huge CJK fonts. Western body
    // fonts and typical PDF subsets are well under that.
    int limit = std::min<int>(static_cast<int>(face->num_glyphs), 256);
    for (int i = 1; i < limit; ++i) {
        FT_Error loadErr = FT_Load_Glyph(face, i, FT_LOAD_RENDER);
        if (loadErr != 0) {
            ++loadFailed;
            continue;
        }
        FT_GlyphSlot slot = face->glyph;
        if (!slot || slot->bitmap.rows == 0) {
            ++emptyBitmap;
            continue;
        }

        int top = slot->bitmap_top;
        int bottom = top - static_cast<int>(slot->bitmap.rows);

        if (sampled == 0) {
            maxTop = top;
            minBottom = bottom;
        } else {
            if (top > maxTop) maxTop = top;
            if (bottom < minBottom) minBottom = bottom;
        }
        ++sampled;
    }

    qDebug() << "[FontAnalyzer] measureRenderedExtent:"
             << "name=" << (face->family_name ? face->family_name : "?")
             << "sampled=" << sampled
             << "loadFailed=" << loadFailed
             << "emptyBitmap=" << emptyBitmap
             << "maxTop=" << maxTop
             << "minBottom=" << minBottom;

    if (sampled == 0)
        return 0.0f;

    int extent = maxTop - minBottom;
    if (extent <= 0)
        return 0.0f;

    // Break extent into top + |bottom| so callers can position baselines.
    profile.renderedMaxTop = static_cast<float>(maxTop) / static_cast<float>(kProbePx);
    profile.renderedMinBottom = static_cast<float>(-minBottom) / static_cast<float>(kProbePx);

    return static_cast<float>(extent) / static_cast<float>(kProbePx);
}

} // namespace pdfe::fonts
