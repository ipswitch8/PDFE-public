#pragma once

#include "tre/TreTypes.h"
#include <QImage>
#include <string>
#include <vector>

namespace pdfe::tre {

class GlyphAtlas;
class EnrichedAtlas;
class BitmapCompositor;

/// Composes text bitmaps from glyph atlas character images. For characters
/// not found in the atlas, falls back to FreeType rendering via BitmapCompositor.
///
/// Used in the image text edit pipeline when the user has not changed font
/// characteristics -- preserves the exact visual appearance of the original
/// scanned document's typography.
class GlyphAtlasCompositor {
public:
    /// Compose text using glyph atlas images with FreeType fallback.
    ///
    /// @param text             UTF-32 text to render.
    /// @param atlas            Glyph atlas with character images.
    /// @param fontMatch        Font match info for FreeType fallback and spacing.
    /// @param bgColor          Background color for the output bitmap.
    /// @param targetWidth      Output bitmap width in pixels.
    /// @param targetHeight     Output bitmap height in pixels.
    /// @param dpi              Rendering DPI (for FreeType fallback sizing).
    /// @param bitmapCompositor Fallback renderer for missing characters (non-owning).
    /// @return Transparent ARGB32 bitmap with composed text.
    QImage compose(const std::u32string& text,
                   const GlyphAtlas& atlas,
                   const FontMatch& fontMatch,
                   const RgbColor& bgColor,
                   int targetWidth,
                   int targetHeight,
                   int dpi,
                   BitmapCompositor* bitmapCompositor) const;

    /// Compose text using an EnrichedAtlas (atlas + synthesis + lazy on-demand).
    ///
    /// This overload provides WYSIWYG consistency between overlay and save paths:
    /// the same EnrichedAtlas used for overlay rendering is passed here for saving,
    /// ensuring identical glyph selection. Falls back to BitmapCompositor (FreeType)
    /// only for characters that neither atlas nor synthesis can provide.
    QImage compose(const std::u32string& text,
                   const EnrichedAtlas& enrichedAtlas,
                   const FontMatch& fontMatch,
                   const RgbColor& bgColor,
                   int targetWidth,
                   int targetHeight,
                   int dpi,
                   BitmapCompositor* bitmapCompositor) const;
};

} // namespace pdfe::tre
