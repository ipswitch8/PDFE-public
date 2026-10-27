#include "tre/VectorFontAtlasBuilder.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_BITMAP_H

#include <QImage>
#include <unordered_set>

namespace pdfe::tre {

namespace {

/// Convert one rendered FT_Bitmap (8-bit gray coverage) into a
/// premultiplied-ARGB QImage with the glyph painted in @p color. Pixels
/// where coverage is zero remain fully transparent; pixels with non-zero
/// coverage are stored as `(coverage*color.r/255, ..., coverage)`.
///
/// Returns a null QImage if the bitmap has zero dimensions or its buffer
/// is null. Only `FT_PIXEL_MODE_GRAY` is handled — other modes (mono,
/// LCD subpixel) return null because subsetted vector fonts always
/// rasterize through `FT_RENDER_MODE_NORMAL` in our pipeline.
QImage glyphBitmapToImage(const FT_Bitmap& bitmap, const RgbColor& color)
{
    if (!bitmap.buffer || bitmap.width == 0 || bitmap.rows == 0)
        return {};

    if (bitmap.pixel_mode != FT_PIXEL_MODE_GRAY)
        return {};

    // Reject pathological dimensions a malformed font could craft. FreeType's
    // `FT_UInt` rows/width can in theory exceed INT_MAX; QImage takes signed
    // ints. Cap at a generous 65535 px (4 GiB worst case for 32-bit ARGB).
    if (bitmap.width > 65535u || bitmap.rows > 65535u)
        return {};

    // FreeType `bitmap.pitch` is signed: positive = top-down storage,
    // negative = bottom-up. Without normalization, `buffer + row * pitch`
    // walks BEHIND `buffer` on negative pitch (out-of-bounds read for any
    // row > 0). Normalize to absolute stride and select the source row
    // based on storage direction. Same defensive pattern FreeType's own
    // sample code uses.
    const int pitchAbs = bitmap.pitch >= 0 ? bitmap.pitch : -bitmap.pitch;
    if (pitchAbs <= 0 || static_cast<unsigned int>(pitchAbs) < bitmap.width)
        return {};
    const bool topDown = bitmap.pitch >= 0;

    QImage img(static_cast<int>(bitmap.width),
               static_cast<int>(bitmap.rows),
               QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);

    for (unsigned int row = 0; row < bitmap.rows; ++row) {
        QRgb* dst = reinterpret_cast<QRgb*>(img.scanLine(static_cast<int>(row)));
        unsigned int srcRow =
            topDown ? row : (bitmap.rows - 1u - row);
        const unsigned char* src =
            bitmap.buffer + static_cast<size_t>(srcRow) * static_cast<size_t>(pitchAbs);
        for (unsigned int col = 0; col < bitmap.width; ++col) {
            uint8_t coverage = src[col];
            if (coverage == 0)
                continue;
            uint8_t r = static_cast<uint8_t>((color.r * coverage) / 255);
            uint8_t g = static_cast<uint8_t>((color.g * coverage) / 255);
            uint8_t b = static_cast<uint8_t>((color.b * coverage) / 255);
            dst[col] = qRgba(r, g, b, coverage);
        }
    }

    return img;
}

} // namespace

VectorAtlasResult VectorFontAtlasBuilder::build(
    FT_Face face,
    int targetHeightPx,
    const std::u32string& requestedCodepoints,
    const RgbColor& textColor)
{
    VectorAtlasResult result;
    result.targetHeightPx = targetHeightPx;

    if (!face || targetHeightPx < 1)
        return result;

    // Set the EM pixel size. FT_Set_Pixel_Sizes returns 0 on success.
    // Bitmap-only fonts may reject arbitrary sizes; we treat that as a
    // build failure rather than silently producing a wrong-size atlas.
    if (FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(targetHeightPx)) != 0)
        return result;

    // Without a Unicode (or other) charmap, we can't map codepoints to
    // glyphs. Some PDF subsetted fonts only ship a custom Identity cmap;
    // those need a separate code path (CIDToGIDMap walk). For phase 2
    // we only support faces with a usable cmap selected — that covers
    // every TrueType/OpenType font in the wild and matches what
    // EmbeddedFontAnalyzer::analyzeOwnedFace expects.
    if (!face->charmap)
        return result;

    // Walk the cmap. FT_Get_First_Char / FT_Get_Next_Char never return
    // glyph index 0, so .notdef is naturally excluded from the atlas
    // (acceptance criterion: "handling of glyph index 0 (.notdef)
    // excluded from atlas").
    std::unordered_set<char32_t> covered;
    covered.reserve(64);

    FT_UInt glyphIndex = 0;
    FT_ULong charcode = FT_Get_First_Char(face, &glyphIndex);
    while (glyphIndex != 0) {
        // Render the glyph via FT_RENDER_MODE_NORMAL (8-bit gray
        // coverage). FT_LOAD_RENDER asks FreeType to load + render in
        // one call, matching the pattern in BitmapCompositor.
        if (FT_Load_Glyph(face, glyphIndex, FT_LOAD_RENDER) == 0 &&
            face->glyph &&
            face->glyph->format == FT_GLYPH_FORMAT_BITMAP)
        {
            QImage img = glyphBitmapToImage(face->glyph->bitmap, textColor);
            if (!img.isNull()) {
                char32_t cp = static_cast<char32_t>(charcode);
                // addGlyph dedupes by codepoint and only replaces on
                // strictly higher confidence. We pass a flat 100 since
                // every glyph here comes from the authoritative font.
                result.atlas.addGlyph(cp, img, /*confidence=*/100);
                covered.insert(cp);
            }
        }

        charcode = FT_Get_Next_Char(face, charcode, &glyphIndex);
    }

    result.rasterizedCount = result.atlas.size();

    // Build the missing-codepoint list. We dedupe so callers don't see
    // the same codepoint twice if the requested text repeats it. Skip
    // ASCII space (0x20) — it's whitespace, never represented by a
    // bitmap glyph in any reasonable text-edit context, and reporting
    // it as missing would create false positives on every line.
    if (!requestedCodepoints.empty()) {
        std::unordered_set<char32_t> reportedMissing;
        for (char32_t cp : requestedCodepoints) {
            if (cp == U' ')
                continue;
            if (covered.count(cp) > 0)
                continue;
            if (!reportedMissing.insert(cp).second)
                continue;
            result.missingCodepoints.push_back(cp);
        }
    }

    return result;
}

} // namespace pdfe::tre
