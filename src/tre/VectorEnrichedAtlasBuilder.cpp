#include "tre/VectorEnrichedAtlasBuilder.h"

#include "tre/VectorFontAtlasBuilder.h"

namespace pdfe::tre {

VectorEnrichedAtlasResult VectorEnrichedAtlasBuilder::build(
    FT_Face face,
    int targetHeightPx,
    const std::u32string& requestedText,
    const RgbColor& textColor)
{
    VectorEnrichedAtlasResult result;
    result.targetHeightPx = targetHeightPx;
    result.atlas = std::make_unique<EnrichedAtlas>();

    if (!face || targetHeightPx < 1)
        return result;

    // Step 1: harvest every cmap glyph into a GlyphAtlas.
    VectorAtlasResult harvested = VectorFontAtlasBuilder::build(
        face, targetHeightPx, requestedText, textColor);

    result.missingCodepoints = std::move(harvested.missingCodepoints);
    result.rasterizedCount = harvested.rasterizedCount;

    // Capture a copy of the harvested atlas for later style calibration.
    // EnrichedAtlas::build moves the GlyphAtlas in, so we'd lose the
    // reference glyphs needed for ScanStyleTransfer calibration if we
    // didn't keep a separate copy. The atlas type is small (a hashmap of
    // QImages); a single copy is cheaper than re-running the FreeType
    // rasterization later.
    GlyphAtlas calibrationCopy;
    for (char32_t cp : harvested.atlas.codepoints()) {
        QImage g = harvested.atlas.glyph(cp);
        if (!g.isNull())
            calibrationCopy.addGlyph(cp, g, /*confidence=*/100);
    }

    // Step 2: build the EnrichedAtlas. This profiles the synthesizer and
    // pre-synthesizes any codepoints from `requestedText` that are missing
    // from the harvested atlas (via GlyphSynthesizer::synthesizeAll).
    result.atlas->build(std::move(harvested.atlas), requestedText);

    // Step 3: calibrate ScanStyleTransfer from the harvested glyphs. This
    // is what gives a later setFreeTypeFallback() invocation the visual
    // style (stroke weight, blur, ink density) of the embedded font
    // instead of the raw FreeType outlines. Safe to call even when the
    // FreeType fallback is never wired — the calibration just sits unused.
    if (calibrationCopy.size() > 0)
        result.atlas->calibrateStyleFrom(calibrationCopy);

    return result;
}

} // namespace pdfe::tre
