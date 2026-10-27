#include "tre/GlyphTemplateCache.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <QDebug>
#include <set>

namespace pdfe::tre {

TemplateMap GlyphTemplateCache::renderTemplates(
    const std::string& fontFilePath,
    int fontFaceIndex,
    int pixelHeight,
    const std::vector<char32_t>& codepoints)
{
    TemplateMap result;

    if (fontFilePath.empty() || pixelHeight <= 0 || codepoints.empty())
        return result;

    // Initialize FreeType
    FT_Library ftLibrary = nullptr;
    FT_Error err = FT_Init_FreeType(&ftLibrary);
    if (err || !ftLibrary)
        return result;

    struct LibGuard {
        FT_Library lib;
        ~LibGuard() { if (lib) FT_Done_FreeType(lib); }
    } libGuard{ftLibrary};

    // Load font face
    FT_Face face = nullptr;
    err = FT_New_Face(ftLibrary, fontFilePath.c_str(), fontFaceIndex, &face);
    if (err || !face)
        return result;

    struct FaceGuard {
        FT_Face face;
        ~FaceGuard() { if (face) FT_Done_Face(face); }
    } faceGuard{face};

    // Set pixel size to match atlas line height.
    // Using 72 DPI so that point size = pixel size.
    err = FT_Set_Char_Size(face, 0,
        static_cast<FT_F26Dot6>(pixelHeight * 64), 72, 72);
    if (err)
        return result;

    // Get font metrics for baseline positioning
    float ascender = static_cast<float>(face->size->metrics.ascender) / 64.0f;

    // Deduplicate codepoints
    std::set<char32_t> unique(codepoints.begin(), codepoints.end());

    for (char32_t cp : unique) {
        if (cp == U' ' || cp == U'\t' || cp == U'\n' || cp == U'\r')
            continue;

        FT_UInt glyphIdx = FT_Get_Char_Index(face, cp);
        if (glyphIdx == 0)
            continue;  // Font doesn't have this glyph

        err = FT_Load_Glyph(face, glyphIdx, FT_LOAD_RENDER);
        if (err)
            continue;

        const FT_Bitmap& bitmap = face->glyph->bitmap;
        if (!bitmap.buffer || bitmap.width == 0 || bitmap.rows == 0)
            continue;

        // Only handle grayscale bitmaps. Color bitmap strikes (BGRA from
        // emoji fonts) and monochrome bitmaps have different stride/pixel
        // layouts — the byte-level blit below assumes 1 byte per pixel.
        if (bitmap.pixel_mode != FT_PIXEL_MODE_GRAY)
            continue;

        // Create canvas at the target pixel height.
        // Width: glyph bitmap width + some padding for bitmap_left offset.
        int canvasW = static_cast<int>(bitmap.width) +
                      std::max(0, face->glyph->bitmap_left) + 2;
        QImage canvas(canvasW, pixelHeight, QImage::Format_ARGB32_Premultiplied);
        canvas.fill(Qt::transparent);

        // Position glyph using ascender baseline
        int glyphX = std::max(0, face->glyph->bitmap_left);
        int glyphY = static_cast<int>(ascender) - face->glyph->bitmap_top;

        // Blit FreeType bitmap onto canvas
        for (unsigned int row = 0; row < bitmap.rows; ++row) {
            int destY = glyphY + static_cast<int>(row);
            if (destY < 0 || destY >= pixelHeight)
                continue;

            QRgb* destRow = reinterpret_cast<QRgb*>(canvas.scanLine(destY));
            for (unsigned int col = 0; col < bitmap.width; ++col) {
                int destX = glyphX + static_cast<int>(col);
                if (destX < 0 || destX >= canvasW)
                    continue;

                uint8_t coverage = bitmap.buffer[row * bitmap.pitch + col];
                if (coverage > 0) {
                    // Black text with coverage as alpha. For black (R=G=B=0),
                    // premultiplied and straight alpha are identical.
                    destRow[destX] = qRgba(0, 0, 0, coverage);
                }
            }
        }

        result.emplace(cp, std::move(canvas));
    }

    if (!result.empty()) {
        qDebug() << "[GlyphTemplateCache] rendered" << result.size()
                 << "templates at height=" << pixelHeight
                 << "from" << QString::fromStdString(fontFilePath);
    }

    return result;
}

} // namespace pdfe::tre
